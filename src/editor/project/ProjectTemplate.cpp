#include "ProjectTemplate.hpp"

#include "editor/document/DefaultScene.hpp"
#include "editor/project/TemplateText.hpp"
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

std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return {text.begin(), text.end()};
}

std::filesystem::path fromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

bool copyTemplateFiles(const std::filesystem::path& templateDirectory, const std::filesystem::path& projectDirectory,
                       std::span<const TemplateToken> tokens, std::string& error) {
    std::error_code iterationError;
    std::filesystem::recursive_directory_iterator files(templateDirectory, iterationError);
    if (iterationError) {
        error = "Could not read the project template " + templateDirectory.string() + ": " + iterationError.message();
        return false;
    }
    for (; files != std::filesystem::recursive_directory_iterator(); files.increment(iterationError)) {
        if (iterationError) {
            error =
                "Could not read the project template " + templateDirectory.string() + ": " + iterationError.message();
            return false;
        }
        const std::filesystem::directory_entry& entry = *files;
        const std::filesystem::path relative = entry.path().lexically_relative(templateDirectory);
        if (entry.is_directory() && relative == std::filesystem::path(templateSnippetDirectoryName)) {
            files.disable_recursion_pending();
            continue;
        }
        if (relative == std::filesystem::path(templateInfoFileName)) {
            continue;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        std::string relativePath;
        if (!substituteTemplateTokens(utf8(relative), tokens, relativePath, error)) {
            error += " in the template path " + relative.generic_string();
            return false;
        }
        std::string contents;
        if (!readTextFile(entry.path(), contents)) {
            error = "Could not read the template file " + entry.path().string();
            return false;
        }
        std::string substituted;
        if (!substituteTemplateTokens(contents, tokens, substituted, error)) {
            error += " in the template file " + relative.generic_string();
            return false;
        }
        if (!writeTextFile(projectDirectory / fromUtf8(relativePath), substituted, error)) {
            return false;
        }
    }
    return true;
}

// The generator the engine was configured with, so the project's compiler
// (and with it the module build hash) matches the engine's.
Json engineDescription(const std::filesystem::path& engineRoot) {
    std::string contents;
    if (!readTextFile(engineRoot / engineDescriptionFileName, contents)) {
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
    ProjectTemplateInfo info;
    if (const ProjectTemplateInfoResult read = readProjectTemplateInfo(request.templateDirectory, info); !read) {
        return {.error = read.error};
    }
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
    if (!writeTextFile(projectDirectory / "CMakePresets.json", projectPresets(request, identifier), error)) {
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
        .inputActions = info.inputActions.empty() ? std::filesystem::path{} : projectDirectory / info.inputActions,
        .tags = info.tags,
    };
    std::filesystem::create_directories(project.sceneRoot, copyError);
    if (!info.startupScene.empty()) {
        // The template's own scene, copied with its files.
        project.startupScene = projectDirectory / info.startupScene;
        if (!std::filesystem::is_regular_file(project.startupScene, copyError)) {
            return {.error = "The template's startup scene " + info.startupScene.generic_string() + " is missing"};
        }
    } else {
        Scene scene;
        if (SceneResult result = populateStarterScene(scene, assets, assetRoot); !result) {
            return {.error = "Could not create the starter scene: " + result.error};
        }
        if (const SceneDocumentResult saved =
                SceneDocument::save(project.startupScene, scene, assets, {.assetRoot = assetRoot});
            !saved) {
            return {.error = "Could not save the starter scene: " + saved.error};
        }
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

ProjectTemplateInfoResult readProjectTemplateInfo(const std::filesystem::path& directory, ProjectTemplateInfo& info) {
    ProjectTemplateInfo read{.directory = directory, .name = directory.filename().string()};
    const std::filesystem::path file = directory / templateInfoFileName;
    std::error_code error;
    if (!std::filesystem::exists(file, error)) {
        info = std::move(read);
        return {.success = true};
    }
    std::string contents;
    if (!readTextFile(file, contents)) {
        return {.error = "Could not read " + file.string()};
    }
    const Json document = Json::parse(contents, nullptr, false);
    if (!document.is_object()) {
        return {.error = file.string() + " is not a JSON object"};
    }
    const auto text = [&document](std::string_view key, std::string& value) {
        const auto found = document.find(key);
        if (found == document.end()) {
            return true;
        }
        if (!found->is_string()) {
            return false;
        }
        value = found->get<std::string>();
        return true;
    };
    std::string startupScene;
    std::string inputActions;
    if (!text("name", read.name) || !text("description", read.description) || !text("startupScene", startupScene) ||
        !text("inputActions", inputActions)) {
        return {.error = file.string() + ": name, description, startupScene, and inputActions are text"};
    }
    read.startupScene = fromUtf8(startupScene);
    read.inputActions = fromUtf8(inputActions);
    if (const auto tags = document.find("tags"); tags != document.end()) {
        if (!tags->is_array() || !std::ranges::all_of(*tags, [](const Json& tag) { return tag.is_string(); })) {
            return {.error = file.string() + ": tags must be an array of names"};
        }
        read.tags = tags->get<std::vector<std::string>>();
    }
    for (const std::filesystem::path& path : {read.startupScene, read.inputActions}) {
        if (path.is_absolute() || path.lexically_normal().string().starts_with("..")) {
            return {.error = file.string() + ": paths must stay inside the template"};
        }
    }
    info = std::move(read);
    return {.success = true};
}

std::vector<ProjectTemplateInfo> listProjectTemplates(std::span<const std::filesystem::path> templateRoots) {
    std::vector<ProjectTemplateInfo> templates;
    for (const std::filesystem::path& root : templateRoots) {
        std::error_code error;
        if (root.empty() || !std::filesystem::is_directory(root, error)) {
            continue;
        }
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(root, error)) {
            const std::string folder = entry.path().filename().string();
            const bool known = std::ranges::any_of(templates, [&folder](const ProjectTemplateInfo& info) {
                return info.directory.filename().string() == folder;
            });
            ProjectTemplateInfo info;
            if (!entry.is_directory() || known || !readProjectTemplateInfo(entry.path().lexically_normal(), info)) {
                continue;
            }
            templates.push_back(std::move(info));
        }
    }
    std::ranges::sort(templates, [](const ProjectTemplateInfo& first, const ProjectTemplateInfo& second) {
        const bool firstIsDefault = first.directory.filename().string() == cppGameTemplateName;
        const bool secondIsDefault = second.directory.filename().string() == cppGameTemplateName;
        if (firstIsDefault != secondIsDefault) {
            return firstIsDefault;
        }
        return first.name < second.name;
    });
    return templates;
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
    if (!readTextFile(directory / "CMakePresets.json", contents)) {
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
