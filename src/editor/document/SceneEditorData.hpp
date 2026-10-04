#pragma once

#include <glm/vec3.hpp>

#include <filesystem>
#include <string>

// Authoring state that belongs to one scene but is not scene content, so it
// is neither undone nor seen by the runtime: today the 3D cursor.
//
// Storage: a per-scene sidecar file beside the scene, "<scene file>.editor.json"
// (for example "level.zenith-scene.editor.json"), holding
//   {"version": 1, "cursor": {"location": [x, y, z]}}
// The scene document itself stays engine data: its snapshots drive undo and
// Play mode, which must not carry editor state. The asset catalog ignores the
// sidecar's extension. A sidecar is only written when the data differs from
// the defaults (an existing one is removed otherwise), so scenes nobody moved
// the cursor in get no extra file. Unknown members are ignored, so later
// versions can add fields without a migration.
struct SceneEditorData {
    // The 3D cursor's world position, in meters.
    glm::vec3 cursorLocation{0.0f};

    bool operator==(const SceneEditorData&) const = default;
};

struct SceneEditorDataResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// "<scene path>.editor.json".
std::filesystem::path sceneEditorDataPath(const std::filesystem::path& scenePath);

// Reads the sidecar of the scene at scenePath into data. A missing sidecar is
// not an error and yields the defaults; an unreadable or malformed one fails
// with a description and also leaves the defaults in data.
SceneEditorDataResult readSceneEditorData(const std::filesystem::path& scenePath, SceneEditorData& data);
// Writes the sidecar of the scene at scenePath (through a temporary file), or
// removes it when data holds only defaults.
SceneEditorDataResult writeSceneEditorData(const std::filesystem::path& scenePath, const SceneEditorData& data);
