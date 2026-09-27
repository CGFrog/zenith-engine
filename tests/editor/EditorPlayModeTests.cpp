#include "editor/application/EditorPlayMode.hpp"

#include "engine/assets/Assets.hpp"

#include <filesystem>
#include <iostream>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

std::string snapshot(const Scene& scene, const AssetManager& assets, const SceneDocumentOptions& options) {
    std::string result;
    const SceneDocumentResult serialized = SceneDocument::serialize(scene, assets, options, result);
    expect(serialized.success, "test scene should serialize");
    return result;
}
} // namespace

int main() {
    AssetManager assets;
    Scene scene;
    const Entity retained = scene.createEntity("Retained");
    const Entity deletedAtRuntime = scene.createEntity("Deleted only at runtime");
    const SceneEntityId retainedId = scene.id(retained);
    const SceneEntityId deletedId = scene.id(deletedAtRuntime);
    std::vector<SceneEntityId> selection{deletedId};
    const SceneDocumentOptions options{.assetRoot = std::filesystem::temp_directory_path()};
    const std::string editSnapshot = snapshot(scene, assets, options);

    EditorPlayMode playMode;
    for (int iteration = 0; iteration < 3; ++iteration) {
        expect(playMode.enter(scene, assets, options, selection).success, "entering Play should clone the edit scene");
        expect(playMode.mode() == EditorMode::Play && playMode.fixedUpdatesEnabled(),
               "Play should enable fixed simulation updates");
        scene.destroyEntity(scene.findEntity(deletedId));
        scene.registry().get<NameComponent>(scene.findEntity(retainedId)).name = "Runtime mutation";

        playMode.pause();
        expect(playMode.mode() == EditorMode::Paused && !playMode.fixedUpdatesEnabled(),
               "Paused mode should suspend fixed simulation updates");
        playMode.requestSingleStep();
        expect(playMode.fixedUpdatesEnabled() && playMode.beginFixedUpdate() && !playMode.beginFixedUpdate(),
               "single-step should permit exactly one paused fixed update");

        selection.clear();
        playMode.stop(scene, selection);
        expect(playMode.mode() == EditorMode::Edit, "Stop should return to Edit mode");
        expect(snapshot(scene, assets, options) == editSnapshot,
               "Stop should restore the byte-identical pre-play document after runtime deletion and mutation");
        expect(selection.size() == 1 && selection.front() == deletedId,
               "Stop should restore the pre-play editor selection");
    }

    return failures == 0 ? 0 : 1;
}
