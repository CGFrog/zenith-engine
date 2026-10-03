#include "ProjectTemplate.hpp"

#include "editor/document/DefaultScene.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/project/ProjectConfig.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>
#include <vector>

namespace {
using Json = nlohmann::json;

constexpr size_t maximumDisplayNameLength = 64;
constexpr const char* projectExtension = ".zenith-project";
constexpr const char* startupSceneName = "main.zenith-scene";

struct TemplateToken {
    std::string_view name;
    std::string value;
};

bool isAsciiLetter(char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
}

bool isAsciiDigit(char character) {
    return character >= '0' && character <= '9';
}

std::string validateDisplayName(std::string_view name) {
    if (name.empty()) {
        return "Enter a project name";
    }
    if (name.size() > maximumDisplayNameLength) {
        return "The project name must be at most " + std::to_string(maximumDisplayNameLength) + " characters";
    }
    const bool allowed = std::ranges::all_of(name, [](char character) {
        return isAsciiLetter(character) || isAsciiDigit(character) || character == ' ' || character == '-' ||
               character == '_';
    });
    if (!allowed) {
        return "The project name may only use letters, digits, spaces, '-' and '_'";
    }
    if (projectIdentifier(name).empty()) {
        return "The project name needs at least one letter or digit";
    }
    return {};
}

bool isTokenCharacter(char character) {
    return (character >= 'A' && character <= 'Z') || character == '_';
}

// Replaces each {{NAME}} with its token's value; an unknown {{NAME}} is an
// error. Braces not around an upper-case name (C++ initializers) are kept.
bool substituteTokens(std::string_view text, std::span<const TemplateToken> tokens, std::string& output,
                      std::string& error) {
    output.clear();
    output.reserve(text.size());
    size_t position = 0;
    while (position < text.size()) {
        const size_t open = text.find("{{", position);
        if (open == std::string_view::npos) {
            output.append(text.substr(position));
            break;
        }
        size_t nameEnd = open + 2;
        while (nameEnd < text.size() && isTokenCharacter(text[nameEnd])) {
            ++nameEnd;
        }
        if (nameEnd == open + 2 || text.substr(nameEnd, 2) != "}}") {
            output.append(text.substr(position, open + 2 - position));
            position = open + 2;
            continue;
        }
        const std::string_view name = text.substr(open + 2, nameEnd - open - 2);
        const auto token = std::ranges::find(tokens, name, &TemplateToken::name);
        if (token == tokens.end()) {
            error = "Unknown template token {{" + std::string(name) + "}}";
            return false;
        }
        output.append(text.substr(position, open - position));
        output.append(token->value);
        position = nameEnd + 2;
    }
    return true;
}

std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return {text.begin(), text.end()};
}

std::filesystem::path fromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

bool readFile(const std::filesystem::path& path, std::string& contents) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    contents.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return !input.bad();
}

bool writeFile(const std::filesystem::path& path, std::string_view contents, std::string& error) {
    std::error_code directoryError;
    std::filesystem::create_directories(path.parent_path(), directoryError);
    if (directoryError) {
        error = "Could not create " + path.parent_path().string() + ": " + directoryError.message();
        return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output) {
        error = "Could not write " + path.string();
        return false;
    }
    return true;
}

bool copyTemplateFiles(const std::filesystem::path& templateDirectory, const std::filesystem::path& projectDirectory,
                       std::span<const TemplateToken> tokens, std::string& error) {
    std::error_code iterationError;
    std::filesystem::recursive_directory_iterator files(templateDirectory, iterationError);
    if (iterationError) {
        error = "Could not read the project template " + templateDirectory.string() + ": " + iterationError.message();
        return false;
    }
    for (const std::filesystem::directory_entry& entry : files) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::filesystem::path relative = entry.path().lexically_relative(templateDirectory);
        std::string relativePath;
        if (!substituteTokens(utf8(relative), tokens, relativePath, error)) {
            error += " in the template path " + relative.generic_string();
            return false;
        }
        std::string contents;
        if (!readFile(entry.path(), contents)) {
            error = "Could not read the template file " + entry.path().string();
            return false;
        }
        std::string substituted;
        if (!substituteTokens(contents, tokens, substituted, error)) {
            error += " in the template file " + relative.generic_string();
            return false;
        }
        if (!writeFile(projectDirectory / fromUtf8(relativePath), substituted, error)) {
            return false;
        }
    }
    return true;
}

// The generator the engine was configured with, so the project's compiler
// (and with it the module build hash) matches the engine's.
Json engineDescription(const std::filesystem::path& engineRoot) {
    std::string contents;
    if (!readFile(engineRoot / engineDescriptionFileName, contents)) {
        return Json::object();
    }
    Json description = Json::parse(contents, nullptr, false);
    return description.is_object() ? description : Json::object();
}

std::string descriptionValue(const Json& description, const char* key) {
    const auto found = description.find(key);
    return found != description.end() && found->is_string() ? found->get<std::string>() : std::string();
}

std::string projectPresets(const ProjectTemplateRequest& request, const std::string& identifier) {
    const Json engine = engineDescription(request.engineRoot);
    Json cacheVariables = {
        {"ZENITH_ROOT", {{"type", "PATH"}, {"value", utf8(request.engineRoot)}}},
        {"CMAKE_BUILD_TYPE", request.buildConfiguration},
    };
    Json configure = {
        {"name", projectBuildPresetName},
        {"displayName", request.displayName + " (" + request.buildConfiguration + ")"},
        {"description", "Builds the " + identifier + " game module against the Zenith engine at ZENITH_ROOT"},
        {"binaryDir", "${sourceDir}/build"},
    };
    if (const std::string generator = descriptionValue(engine, "generator"); !generator.empty()) {
        configure["generator"] = generator;
    }
    if (const std::string platform = descriptionValue(engine, "generatorPlatform"); !platform.empty()) {
        configure["architecture"] = {{"value", platform}, {"strategy", "set"}};
    }
    if (const std::string toolset = descriptionValue(engine, "generatorToolset"); !toolset.empty()) {
        configure["toolset"] = {{"value", toolset}, {"strategy", "set"}};
    }
    if (const std::string instance = descriptionValue(engine, "generatorInstance"); !instance.empty()) {
        cacheVariables["CMAKE_GENERATOR_INSTANCE"] = instance;
    }
    configure["cacheVariables"] = std::move(cacheVariables);
    const Json presets = {
        {"version", 3},
        {"configurePresets", Json::array({std::move(configure)})},
        {"buildPresets", Json::array({{{"name", projectBuildPresetName},
                                       {"configurePreset", projectBuildPresetName},
                                       {"configuration", request.buildConfiguration}}})},
    };
    return presets.dump(2) + '\n';
}

void setLocalTransform(Scene& scene, Entity entity, const glm::mat4& local) {
    scene.registry().get_or_emplace<TransformComponent>(entity).local = local;
    scene.markTransformDirty(entity);
}

void renameEntity(Scene& scene, Entity entity, std::string name) {
    scene.registry().get_or_emplace<NameComponent>(entity).name = std::move(name);
}

// Ground, a cube to look at, the sun and sky, and a camera looking at the
// cube. The cube model spans -1 to 1 on each axis.
SceneResult populateStarterScene(Scene& scene, AssetManager& assets, const std::filesystem::path& assetRoot) {
    addSun(scene);
    addEnvironment(scene);
    Entity ground = entt::null;
    if (SceneResult result = addCube(scene, assets, assetRoot, ground); !result) {
        return result;
    }
    renameEntity(scene, ground, "Ground");
    setLocalTransform(scene, ground,
                      glm::scale(glm::translate(glm::mat4(1.0f), {0.0f, -0.1f, 0.0f}), {10.0f, 0.1f, 10.0f}));
    Entity cube = entt::null;
    if (SceneResult result = addCube(scene, assets, assetRoot, cube); !result) {
        return result;
    }
    setLocalTransform(scene, cube, glm::scale(glm::translate(glm::mat4(1.0f), {0.0f, 0.5f, 0.0f}), glm::vec3(0.5f)));
    const Entity camera = scene.createEntity("Camera");
    scene.registry().emplace<CameraComponent>(camera);
    setLocalTransform(scene, camera,
                      glm::inverse(glm::lookAt(glm::vec3(0.0f, 2.5f, 6.0f), glm::vec3(0.0f, 0.5f, 0.0f),
                                               glm::vec3(0.0f, 1.0f, 0.0f))));
    scene.updateWorldTransforms();
    return {.success = true};
}

ProjectTemplateResult writeProject(const ProjectTemplateRequest& request, const std::string& identifier,
                                   const std::filesystem::path& projectDirectory, AssetManager& assets) {
    const std::array tokens{
        TemplateToken{.name = "PROJECT_NAME", .value = request.displayName},
        TemplateToken{.name = "PROJECT_IDENTIFIER", .value = identifier},
        TemplateToken{.name = "ZENITH_ROOT", .value = utf8(request.engineRoot)},
        TemplateToken{.name = "BUILD_CONFIG", .value = request.buildConfiguration},
    };
    std::string error;
    if (!copyTemplateFiles(request.templateDirectory, projectDirectory, tokens, error)) {
        return {.error = std::move(error)};
    }
    if (!writeFile(projectDirectory / "CMakePresets.json", projectPresets(request, identifier), error)) {
        return {.error = std::move(error)};
    }

    const std::filesystem::path assetRoot = projectDirectory / "assets";
    const std::filesystem::path starterModel = assetRoot / defaultCubeModel;
    std::error_code copyError;
    std::filesystem::create_directories(starterModel.parent_path(), copyError);
    std::filesystem::copy_file(request.engineAssetRoot / defaultCubeModel, starterModel,
                               std::filesystem::copy_options::overwrite_existing, copyError);
    if (copyError) {
        return {.error = "Could not copy the starter model from " +
                         (request.engineAssetRoot / defaultCubeModel).string() + ": " + copyError.message()};
    }

    ProjectConfig project{
        .displayName = request.displayName,
        .assetRoot = assetRoot,
        .sceneRoot = projectDirectory / "scenes",
        .startupScene = projectDirectory / "scenes" / startupSceneName,
        .renderSettings = RenderSettings{.environment = EnvironmentQuality::ImageBasedLow},
        .gameModule = projectDirectory / "modules" / (identifier + ".zmodule"),
    };
    Scene scene;
    if (SceneResult result = populateStarterScene(scene, assets, assetRoot); !result) {
        return {.error = "Could not create the starter scene: " + result.error};
    }
    std::filesystem::create_directories(project.sceneRoot, copyError);
    if (const SceneDocumentResult saved =
            SceneDocument::save(project.startupScene, scene, assets, {.assetRoot = assetRoot});
        !saved) {
        return {.error = "Could not save the starter scene: " + saved.error};
    }
    const std::filesystem::path projectFile = projectDirectory / (identifier + projectExtension);
    if (const ProjectConfigResult saved = saveProjectConfig(projectFile, project); !saved) {
        return {.error = saved.error};
    }
    return {.success = true, .projectFile = projectFile};
}
} // namespace

std::filesystem::path findEngineBuildRoot(std::span<const std::filesystem::path> candidates) {
    for (const std::filesystem::path& candidate : candidates) {
        std::error_code error;
        if (!candidate.empty() && std::filesystem::is_regular_file(candidate / engineConfigFileName, error)) {
            return candidate.lexically_normal();
        }
    }
    return {};
}

std::filesystem::path findProjectTemplate(std::span<const std::filesystem::path> templateRoots,
                                          std::string_view templateName) {
    for (const std::filesystem::path& root : templateRoots) {
        std::error_code error;
        const std::filesystem::path candidate = root / templateName;
        if (!root.empty() && std::filesystem::is_directory(candidate, error)) {
            return candidate.lexically_normal();
        }
    }
    return {};
}

std::string projectIdentifier(std::string_view displayName) {
    std::string identifier;
    bool pendingSeparator = false;
    for (const char character : displayName) {
        if (isAsciiLetter(character) || isAsciiDigit(character)) {
            if (pendingSeparator && !identifier.empty()) {
                identifier.push_back('_');
            }
            pendingSeparator = false;
            identifier.push_back(
                static_cast<char>(character >= 'A' && character <= 'Z' ? character - 'A' + 'a' : character));
        } else {
            pendingSeparator = true;
        }
    }
    if (!identifier.empty() && isAsciiDigit(identifier.front())) {
        identifier.insert(0, "game_");
    }
    // The identifier names a CMake target, which must not be one CMake
    // reserves or generates itself.
    constexpr std::array<std::string_view, 12> reservedTargets{
        "all",     "all_build",     "clean",     "edit_cache", "help",      "install", "list_install_components",
        "package", "rebuild_cache", "run_tests", "test",       "zero_check"};
    if (std::ranges::find(reservedTargets, identifier) != reservedTargets.end()) {
        identifier += "_game";
    }
    return identifier;
}

ProjectTemplateResult generateProjectFromTemplate(const ProjectTemplateRequest& request, AssetManager& assets) {
    if (std::string error = validateDisplayName(request.displayName); !error.empty()) {
        return {.error = std::move(error)};
    }
    std::error_code error;
    if (request.templateDirectory.empty() || !std::filesystem::is_directory(request.templateDirectory, error)) {
        return {.error = "The project template was not found: " + request.templateDirectory.string()};
    }
    if (request.parentDirectory.empty()) {
        return {.error = "Choose a folder to create the project in"};
    }
    const std::string identifier = projectIdentifier(request.displayName);
    const std::filesystem::path parent = std::filesystem::absolute(request.parentDirectory, error).lexically_normal();
    const std::filesystem::path projectDirectory = parent / identifier;
    const bool existed = std::filesystem::exists(projectDirectory, error);
    if (existed && !std::filesystem::is_empty(projectDirectory, error)) {
        return {.error = "The folder " + projectDirectory.string() + " already exists and is not empty"};
    }
    std::filesystem::create_directories(projectDirectory, error);
    if (error) {
        return {.error = "Could not create " + projectDirectory.string() + ": " + error.message()};
    }

    ProjectTemplateResult result = writeProject(request, identifier, projectDirectory, assets);
    if (!result) {
        // Leave nothing half-generated behind.
        if (existed) {
            for (const std::filesystem::directory_entry& entry :
                 std::filesystem::directory_iterator(projectDirectory, error)) {
                std::filesystem::remove_all(entry.path(), error);
            }
        } else {
            std::filesystem::remove_all(projectDirectory, error);
        }
    }
    return result;
}

std::optional<BuildRequest> projectBuildRequest(const std::filesystem::path& projectFile) {
    if (projectFile.empty()) {
        return std::nullopt;
    }
    const std::filesystem::path directory = projectFile.parent_path();
    std::string contents;
    if (!readFile(directory / "CMakePresets.json", contents)) {
        return std::nullopt;
    }
    const Json presets = Json::parse(contents, nullptr, false);
    if (!presets.is_object() || !presets.contains("configurePresets") || !presets["configurePresets"].is_array()) {
        return std::nullopt;
    }
    const bool hasPreset = std::ranges::any_of(presets["configurePresets"], [](const Json& preset) {
        return preset.is_object() && preset.value("name", std::string()) == projectBuildPresetName;
    });
    if (!hasPreset) {
        return std::nullopt;
    }
    const std::string preset(projectBuildPresetName);
    return BuildRequest{.command = "cmake --preset " + preset + " && cmake --build --preset " + preset,
                        .workingDirectory = directory};
}
