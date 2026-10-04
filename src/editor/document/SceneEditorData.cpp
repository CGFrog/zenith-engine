#include "SceneEditorData.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

namespace {
constexpr int currentVersion = 1;

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
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return {.success = true};
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return failure(path, "could not be opened");
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    const nlohmann::json document = nlohmann::json::parse(contents.str(), nullptr, false);
    if (!document.is_object()) {
        return failure(path, "is not a JSON object");
    }
    const auto version = document.find("version");
    if (version == document.end() || !version->is_number_integer() || version->get<int>() < 1) {
        return failure(path, "has no valid version");
    }
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
        {"version", currentVersion},
        {"cursor", {{"location", {data.cursorLocation.x, data.cursorLocation.y, data.cursorLocation.z}}}}};
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << document.dump(2) << '\n';
        output.flush();
        if (!output) {
            std::filesystem::remove(temporary, error);
            return failure(path, "could not be written");
        }
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        const std::string reason = error.message();
        std::filesystem::remove(temporary, error);
        return failure(path, "could not replace the previous file: " + reason);
    }
    return {.success = true};
}
