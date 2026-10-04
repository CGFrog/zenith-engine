#include "SceneEditorData.hpp"

#include "engine/platform/VersionedJson.hpp"

#include <nlohmann/json.hpp>

namespace {
// Kept beside the scene and often under version control, so an older editor
// still reads what a newer one wrote.
const VersionedJsonFormat sceneEditorDataFormat{.description = "scene editor data", .newer = NewerJsonVersions::Read};

SceneEditorDataResult failure(const std::filesystem::path& path, const std::string& reason) {
    return {.error = "Scene editor data '" + path.string() + "': " + reason};
}
} // namespace

std::filesystem::path sceneEditorDataPath(const std::filesystem::path& scenePath) {
    std::filesystem::path path = scenePath;
    path += ".editor.json";
    return path;
}

SceneEditorDataResult readSceneEditorData(const std::filesystem::path& scenePath, SceneEditorData& data) {
    data = {};
    const std::filesystem::path path = sceneEditorDataPath(scenePath);
    const VersionedJsonResult read = readVersionedJson(path, sceneEditorDataFormat);
    if (!read) {
        return {.error = read.error};
    }
    if (!read.found) {
        return {.success = true};
    }
    const nlohmann::json& document = read.document;
    const auto cursor = document.find("cursor");
    if (cursor == document.end()) {
        return {.success = true};
    }
    const auto location = cursor->is_object() ? cursor->find("location") : cursor->end();
    if (!cursor->is_object() || location == cursor->end() || !location->is_array() || location->size() != 3) {
        return failure(path, "cursor.location must be three numbers");
    }
    glm::vec3 value{0.0f};
    for (size_t index = 0; index < 3; ++index) {
        if (!(*location)[index].is_number()) {
            return failure(path, "cursor.location must be three numbers");
        }
        value[static_cast<glm::length_t>(index)] = (*location)[index].get<float>();
    }
    data.cursorLocation = value;
    return {.success = true};
}

SceneEditorDataResult writeSceneEditorData(const std::filesystem::path& scenePath, const SceneEditorData& data) {
    const std::filesystem::path path = sceneEditorDataPath(scenePath);
    std::error_code error;
    if (data == SceneEditorData{}) {
        std::filesystem::remove(path, error);
        if (error) {
            return failure(path, "could not be removed: " + error.message());
        }
        return {.success = true};
    }
    const nlohmann::json document{
        {"cursor", {{"location", {data.cursorLocation.x, data.cursorLocation.y, data.cursorLocation.z}}}}};
    if (const PlatformResult written = writeVersionedJson(path, sceneEditorDataFormat, document); !written) {
        return {.error = written.error};
    }
    return {.success = true};
}
