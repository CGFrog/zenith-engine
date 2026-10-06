#pragma once

// Module > New Behaviour/Component/System: writes sources from snippets and edits CMakeLists and GameModule.cpp.

#include "engine/module/SystemScheduler.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Writes a new behaviour, component, or system into a game project made from
// templates/cpp_game, from the template's snippets, and wires it in:
//
//   src/<Type>.hpp, src/<Type>.cpp   from snippets/<kind>.{hpp,cpp}.in
//   src/CMakeLists.txt               the .cpp, above `# zenith:sources`
//   src/GameModule.cpp               the #include, above `// zenith:includes`,
//                                    and the register call, above
//                                    `// zenith:register`
//
// Snippets use the tokens {{CLASS_NAME}}, {{KEY}}, {{DISPLAY_NAME}},
// {{CATEGORY}}, {{FIELDS}}, {{FIELD_INCLUDES}}, {{FIELD_SETUP}},
// {{SYSTEM_NAME}}, and {{PHASE}}. Nothing is written unless every file can
// be: an existing type file or a missing marker fails the whole request.

enum class GeneratedCodeKind : uint8_t {
    Behaviour,
    Component,
    System,
};

// The C++ type of a generated field and how it is reflected.
enum class GeneratedFieldType : uint8_t {
    Bool,
    Int,
    Float,
    Vec3,
    String,
    // Another entity, by its stable id (SceneEntityId).
    Entity,
};

struct GeneratedField {
    // A camelCase C++ member name; the scene key is its snake_case form.
    std::string name;
    GeneratedFieldType type{GeneratedFieldType::Float};
};

struct CodeGenerationRequest {
    GeneratedCodeKind kind{GeneratedCodeKind::Behaviour};
    // The type's PascalCase name. A component's type gets a "Component" and a
    // system's a "System" suffix when the name lacks it ("Health" gives
    // HealthComponent).
    std::string name;
    // The Add Component category of a behaviour or component; empty uses
    // "Behaviours" or "Gameplay".
    std::string category;
    // A system's phase.
    SystemPhase phase{SystemPhase::Update};
    // A behaviour's or component's fields; systems have none.
    std::vector<GeneratedField> fields;
};

struct CodeGenerationResult {
    bool success{false};
    std::string error;
    // The new header and source, in that order.
    std::vector<std::filesystem::path> createdFiles;
    // Where to open the IDE: the new source at its register function.
    std::filesystem::path sourceFile;
    uint32_t sourceLine{1};

    explicit operator bool() const {
        return success;
    }
};

// The game module's source folder of the project whose .zenith-project file
// is projectFile: src/ beside it, where generateModuleCode writes.
std::filesystem::path projectSourceDirectory(const std::filesystem::path& projectFile);

// Generates request's type in the project whose .zenith-project file is
// projectFile (its stem is the module identifier systems are prefixed with),
// from the snippets in snippetDirectory.
CodeGenerationResult generateModuleCode(const std::filesystem::path& projectFile,
                                        const std::filesystem::path& snippetDirectory,
                                        const CodeGenerationRequest& request);

// The name of the type generateModuleCode creates (with its kind's suffix),
// or an error message for a name that is not a PascalCase identifier.
struct GeneratedTypeName {
    std::string typeName;
    std::string error;
};

GeneratedTypeName generatedTypeName(GeneratedCodeKind kind, std::string_view name);
// An error message for a field name that is not a camelCase identifier, or
// empty.
std::string validateGeneratedFieldName(std::string_view name);
// "MaxHealth" gives "max_health"; "HTTPServer" gives "http_server".
std::string snakeCaseName(std::string_view name);
// "MaxHealth" gives "Max Health".
std::string spacedDisplayName(std::string_view name);

// "float", "int", "bool", "vec3", "string", or "entity".
std::string_view generatedFieldTypeName(GeneratedFieldType type);
std::optional<GeneratedFieldType> parseGeneratedFieldType(std::string_view name);
// Parses "speed:float, target:entity" (spaces optional, an empty text is no
// fields); nullopt with error set for a bad entry.
std::optional<std::vector<GeneratedField>> parseGeneratedFields(std::string_view text, std::string& error);
std::string formatGeneratedFields(const std::vector<GeneratedField>& fields);

// Inserts line above the first line of text whose trimmed content starts
// with marker, indented as that line is. Returns false, leaving text alone,
// when no line has the marker. A line already present there is not added
// twice.
bool insertAboveMarker(std::string& text, std::string_view marker, std::string_view line);
