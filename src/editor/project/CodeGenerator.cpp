#include "CodeGenerator.hpp"

#include "editor/project/ProjectFileOperations.hpp"
#include "editor/project/TemplateText.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <system_error>
#include <utility>

namespace {
constexpr size_t maximumNameLength = 64;
constexpr std::string_view sourcesMarker = "# zenith:sources";
constexpr std::string_view includesMarker = "// zenith:includes";
constexpr std::string_view registerMarker = "// zenith:register";

bool isUpper(char character) {
    return character >= 'A' && character <= 'Z';
}

bool isLower(char character) {
    return character >= 'a' && character <= 'z';
}

bool isDigit(char character) {
    return character >= '0' && character <= '9';
}

bool isAlphanumeric(char character) {
    return isUpper(character) || isLower(character) || isDigit(character);
}

char lower(char character) {
    return isUpper(character) ? static_cast<char>(character - 'A' + 'a') : character;
}

std::string_view trimmed(std::string_view text) {
    const size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    const size_t last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

std::string_view kindSuffix(GeneratedCodeKind kind) {
    switch (kind) {
    case GeneratedCodeKind::Behaviour:
        return "";
    case GeneratedCodeKind::Component:
        return "Component";
    case GeneratedCodeKind::System:
        return "System";
    }
    return "";
}

std::string_view snippetName(GeneratedCodeKind kind) {
    switch (kind) {
    case GeneratedCodeKind::Behaviour:
        return "behaviour";
    case GeneratedCodeKind::Component:
        return "component";
    case GeneratedCodeKind::System:
        return "system";
    }
    return "";
}

std::string_view defaultCategory(GeneratedCodeKind kind) {
    return kind == GeneratedCodeKind::Behaviour ? "Behaviours" : "Gameplay";
}

// Splits name into words at case changes: "MaxHTTPSpeed" gives Max, HTTP,
// Speed.
std::vector<std::string> words(std::string_view name) {
    std::vector<std::string> result;
    std::string word;
    for (size_t index = 0; index < name.size(); ++index) {
        const char character = name[index];
        const bool startsWord = !word.empty() && isUpper(character) &&
                                (!isUpper(name[index - 1]) || (index + 1 < name.size() && isLower(name[index + 1])));
        if (startsWord) {
            result.push_back(std::move(word));
            word.clear();
        }
        word.push_back(character);
    }
    if (!word.empty()) {
        result.push_back(std::move(word));
    }
    return result;
}

std::string_view fieldCppType(GeneratedFieldType type) {
    switch (type) {
    case GeneratedFieldType::Bool:
        return "bool";
    case GeneratedFieldType::Int:
        return "int32_t";
    case GeneratedFieldType::Float:
        return "float";
    case GeneratedFieldType::Vec3:
        return "glm::vec3";
    case GeneratedFieldType::String:
        return "std::string";
    case GeneratedFieldType::Entity:
        return "SceneEntityId";
    }
    return "float";
}

std::string_view fieldInitializer(GeneratedFieldType type) {
    switch (type) {
    case GeneratedFieldType::Bool:
        return "{false}";
    case GeneratedFieldType::Int:
        return "{0}";
    case GeneratedFieldType::Float:
        return "{0.0f}";
    case GeneratedFieldType::Vec3:
        return "{0.0f}";
    case GeneratedFieldType::String:
    case GeneratedFieldType::Entity:
        return "";
    }
    return "";
}

std::string fieldDeclarations(const std::vector<GeneratedField>& fields, std::string_view typeName) {
    if (fields.empty()) {
        return "    // Add fields here, and reflect them in register" + std::string(typeName) + ".\n";
    }
    std::string text;
    for (const GeneratedField& field : fields) {
        text += "    " + std::string(fieldCppType(field.type)) + " " + field.name +
                std::string(fieldInitializer(field.type)) + ";\n";
    }
    return text;
}

std::string fieldIncludes(const std::vector<GeneratedField>& fields) {
    std::set<std::string> includes{"#include <cstdint>"};
    for (const GeneratedField& field : fields) {
        if (field.type == GeneratedFieldType::Vec3) {
            includes.insert("#include <glm/vec3.hpp>");
        } else if (field.type == GeneratedFieldType::String) {
            includes.insert("#include <string>");
        } else if (field.type == GeneratedFieldType::Entity) {
            includes.insert("#include \"engine/scene/SceneEntityId.hpp\"");
        }
    }
    std::string text;
    // Quoted project headers first, as the template's own files order them.
    for (const bool quoted : {true, false}) {
        for (const std::string& include : includes) {
            if ((include.find('"') != std::string::npos) == quoted) {
                text += include + "\n";
            }
        }
    }
    text.pop_back();
    return text;
}

std::string fieldSetup(const std::vector<GeneratedField>& fields, std::string_view typeName) {
    std::string text = "Fields<" + std::string(typeName) + ">()";
    for (const GeneratedField& field : fields) {
        text += "\n            .add(\"" + snakeCaseName(field.name) + "\", &" + std::string(typeName) +
                "::" + field.name + ")";
    }
    return text;
}

std::string validateRequest(const CodeGenerationRequest& request) {
    if (request.kind == GeneratedCodeKind::System && !request.fields.empty()) {
        return "A system has no fields; add them to a component it reads";
    }
    std::set<std::string> names;
    for (const GeneratedField& field : request.fields) {
        if (std::string error = validateGeneratedFieldName(field.name); !error.empty()) {
            return error;
        }
        if (request.kind == GeneratedCodeKind::Behaviour && field.name == "enabled") {
            return "Every behaviour already has an 'enabled' field";
        }
        if (!names.insert(snakeCaseName(field.name)).second) {
            return "The field '" + field.name + "' is listed twice";
        }
    }
    if (request.category.find_first_of("\"\\\r\n") != std::string::npos) {
        return "The category cannot contain quotes, backslashes, or line breaks";
    }
    return {};
}

struct PendingFile {
    std::filesystem::path path;
    std::string contents;
};

bool readSnippet(const std::filesystem::path& path, std::span<const TemplateToken> tokens, std::string& output,
                 std::string& error) {
    std::string snippet;
    if (!readTextFile(path, snippet)) {
        error = "Could not read the code snippet " + utf8PathText(path);
        return false;
    }
    if (!substituteTemplateTokens(snippet, tokens, output, error)) {
        error += " in the code snippet " + utf8PathText(path);
        return false;
    }
    return true;
}

bool insertOrExplain(std::string& text, std::string_view marker, std::string_view line, std::string_view file,
                     std::string_view where, std::string& error) {
    if (insertAboveMarker(text, marker, line)) {
        return true;
    }
    error = std::string(file) + " has no '" + std::string(marker) + "' marker. Add it on its own line " +
            std::string(where) + " (as templates/cpp_game does), then generate again.";
    return false;
}

uint32_t lineOf(std::string_view text, std::string_view needle) {
    const size_t position = text.find(needle);
    if (position == std::string_view::npos) {
        return 1;
    }
    return static_cast<uint32_t>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(position), '\n')) +
           1;
}
} // namespace

GeneratedTypeName generatedTypeName(GeneratedCodeKind kind, std::string_view name) {
    if (name.empty()) {
        return {.error = "Enter a name"};
    }
    if (name.size() > maximumNameLength) {
        return {.error = "The name must be at most " + std::to_string(maximumNameLength) + " characters"};
    }
    if (!isUpper(name.front()) || !std::ranges::all_of(name, isAlphanumeric)) {
        return {.error = "The name must be a PascalCase C++ identifier: letters and digits, starting with a capital"};
    }
    const std::string_view suffix = kindSuffix(kind);
    std::string typeName(name);
    if (!name.ends_with(suffix)) {
        typeName += suffix;
    } else if (name == suffix) {
        return {.error = "Name the " + std::string(snippetName(kind)) + " for what it does, such as Health" +
                         std::string(suffix)};
    }
    constexpr std::array<std::string_view, 4> reserved{"Behaviour", "GameModule", "Scene", "Entity"};
    if (std::ranges::find(reserved, typeName) != reserved.end()) {
        return {.error = "'" + typeName + "' names an engine or template type"};
    }
    return {.typeName = std::move(typeName)};
}

std::string validateGeneratedFieldName(std::string_view name) {
    if (name.empty()) {
        return "A field needs a name";
    }
    if (name.size() > maximumNameLength || !isLower(name.front()) || !std::ranges::all_of(name, isAlphanumeric)) {
        return "The field '" + std::string(name) +
               "' must be a camelCase C++ identifier: letters and digits, starting with a lower-case letter";
    }
    return {};
}

std::string snakeCaseName(std::string_view name) {
    std::string result;
    for (const std::string& word : words(name)) {
        if (!result.empty()) {
            result.push_back('_');
        }
        std::ranges::transform(word, std::back_inserter(result), lower);
    }
    return result;
}

std::string spacedDisplayName(std::string_view name) {
    std::string result;
    for (const std::string& word : words(name)) {
        result += (result.empty() ? "" : " ") + word;
    }
    return result;
}

std::string_view generatedFieldTypeName(GeneratedFieldType type) {
    switch (type) {
    case GeneratedFieldType::Bool:
        return "bool";
    case GeneratedFieldType::Int:
        return "int";
    case GeneratedFieldType::Float:
        return "float";
    case GeneratedFieldType::Vec3:
        return "vec3";
    case GeneratedFieldType::String:
        return "string";
    case GeneratedFieldType::Entity:
        return "entity";
    }
    return "float";
}

std::optional<GeneratedFieldType> parseGeneratedFieldType(std::string_view name) {
    for (const GeneratedFieldType type :
         {GeneratedFieldType::Bool, GeneratedFieldType::Int, GeneratedFieldType::Float, GeneratedFieldType::Vec3,
          GeneratedFieldType::String, GeneratedFieldType::Entity}) {
        if (generatedFieldTypeName(type) == name) {
            return type;
        }
    }
    return std::nullopt;
}

std::optional<std::vector<GeneratedField>> parseGeneratedFields(std::string_view text, std::string& error) {
    std::vector<GeneratedField> fields;
    if (trimmed(text).empty()) {
        return fields;
    }
    size_t start = 0;
    while (start <= text.size()) {
        const size_t comma = std::min(text.find(',', start), text.size());
        const std::string_view entry = trimmed(text.substr(start, comma - start));
        const size_t colon = entry.find(':');
        if (colon == std::string_view::npos) {
            error = "Write each field as name:type, for example speed:float";
            return std::nullopt;
        }
        const std::string_view typeName = trimmed(entry.substr(colon + 1));
        const std::optional<GeneratedFieldType> type = parseGeneratedFieldType(typeName);
        if (!type) {
            error = "Unknown field type '" + std::string(typeName) + "'; use bool, int, float, vec3, string, or entity";
            return std::nullopt;
        }
        fields.push_back({.name = std::string(trimmed(entry.substr(0, colon))), .type = *type});
        start = comma + 1;
    }
    return fields;
}

std::string formatGeneratedFields(const std::vector<GeneratedField>& fields) {
    std::string text;
    for (const GeneratedField& field : fields) {
        text += (text.empty() ? "" : ", ") + field.name + ":" + std::string(generatedFieldTypeName(field.type));
    }
    return text;
}

bool insertAboveMarker(std::string& text, std::string_view marker, std::string_view line) {
    std::string_view previousContent;
    size_t lineStart = 0;
    while (lineStart <= text.size()) {
        const size_t lineEnd = std::min(text.find('\n', lineStart), text.size());
        const std::string_view current(text.data() + lineStart, lineEnd - lineStart);
        const std::string_view content = trimmed(current);
        if (content.starts_with(marker)) {
            // Generated lines sit directly above the marker; one already
            // there is not added again.
            if (previousContent == trimmed(line)) {
                return true;
            }
            const std::string indentation(current.substr(0, current.find_first_not_of(" \t")));
            const bool crlf = current.ends_with('\r');
            text.insert(lineStart, indentation + std::string(line) + (crlf ? "\r\n" : "\n"));
            return true;
        }
        previousContent = content;
        lineStart = lineEnd + 1;
    }
    return false;
}

CodeGenerationResult generateModuleCode(const std::filesystem::path& projectFile,
                                        const std::filesystem::path& snippetDirectory,
                                        const CodeGenerationRequest& request) {
    const GeneratedTypeName type = generatedTypeName(request.kind, request.name);
    if (!type.error.empty()) {
        return {.error = type.error};
    }
    if (std::string error = validateRequest(request); !error.empty()) {
        return {.error = std::move(error)};
    }
    const std::string& typeName = type.typeName;
    const std::string_view suffix = kindSuffix(request.kind);
    const std::string baseName = typeName.substr(0, typeName.size() - suffix.size());
    const std::string moduleIdentifier = projectFile.stem().string();
    const std::filesystem::path sourceDirectory = projectFile.parent_path() / "src";

    const std::array tokens{
        TemplateToken{.name = "CLASS_NAME", .value = typeName},
        TemplateToken{.name = "KEY", .value = snakeCaseName(baseName)},
        TemplateToken{.name = "DISPLAY_NAME", .value = spacedDisplayName(baseName)},
        TemplateToken{.name = "CATEGORY",
                      .value =
                          request.category.empty() ? std::string(defaultCategory(request.kind)) : request.category},
        TemplateToken{.name = "FIELDS", .value = fieldDeclarations(request.fields, typeName)},
        TemplateToken{.name = "FIELD_INCLUDES", .value = fieldIncludes(request.fields)},
        TemplateToken{.name = "FIELD_SETUP", .value = fieldSetup(request.fields, typeName)},
        TemplateToken{.name = "SYSTEM_NAME", .value = moduleIdentifier + "." + snakeCaseName(baseName)},
        TemplateToken{.name = "PHASE", .value = std::string(systemPhaseName(request.phase))},
    };

    std::string error;
    std::vector<PendingFile> created;
    for (const std::string_view extension : {".hpp", ".cpp"}) {
        PendingFile file{.path = sourceDirectory / (typeName + std::string(extension))};
        std::error_code existsError;
        if (std::filesystem::exists(file.path, existsError)) {
            return {.error = utf8PathText(file.path) + " already exists; choose another name"};
        }
        const std::filesystem::path snippet =
            snippetDirectory / (std::string(snippetName(request.kind)) + std::string(extension) + ".in");
        if (!readSnippet(snippet, tokens, file.contents, error)) {
            return {.error = std::move(error)};
        }
        created.push_back(std::move(file));
    }

    PendingFile cmakeLists{.path = sourceDirectory / "CMakeLists.txt"};
    PendingFile registration{.path = sourceDirectory / "GameModule.cpp"};
    for (PendingFile* file : {&cmakeLists, &registration}) {
        if (!readTextFile(file->path, file->contents)) {
            return {.error = "Could not read " + utf8PathText(file->path) +
                             "; code generation needs a project made from the C++ game template"};
        }
    }
    if (!insertOrExplain(cmakeLists.contents, sourcesMarker, typeName + ".cpp", "src/CMakeLists.txt",
                         "in the add_library source list", error) ||
        !insertOrExplain(registration.contents, includesMarker, "#include \"" + typeName + ".hpp\"",
                         "src/GameModule.cpp", "below the #include lines", error) ||
        !insertOrExplain(registration.contents, registerMarker, "register" + typeName + "(registrar);",
                         "src/GameModule.cpp", "inside zenith_module_register", error)) {
        return {.error = std::move(error)};
    }

    std::vector<std::filesystem::path> written;
    for (const PendingFile& file : {created[0], created[1], cmakeLists, registration}) {
        if (!writeTextFile(file.path, file.contents, error)) {
            // The new files are removed again; an edited file that failed to
            // write is unchanged.
            for (const std::filesystem::path& path : written) {
                if (path != cmakeLists.path && path != registration.path) {
                    std::error_code removeError;
                    std::filesystem::remove(path, removeError);
                }
            }
            return {.error = std::move(error)};
        }
        written.push_back(file.path);
    }
    return {.success = true,
            .createdFiles = {created[0].path, created[1].path},
            .sourceFile = created[1].path,
            .sourceLine = lineOf(created[1].contents, "void register" + typeName + "(")};
}
