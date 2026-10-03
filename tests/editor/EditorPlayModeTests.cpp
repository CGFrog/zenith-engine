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

// Simulate runs on a copy like Play, pauses and steps the same way, and
// resumes into Simulate rather than Play.
void testSimulate(Scene& scene, AssetManager& assets, const SceneDocumentOptions& options,
                  const std::string& editSnapshot) {
    EditorPlayMode playMode;
    std::vector<SceneEntityId> selection;
    expect(playMode.simulationMode() == SimulationMode::Edit, "Edit mode runs Edit systems");
    expect(playMode.enter(scene, assets, options, selection, EditorRuntime::Simulate).success,
           "entering Simulate should clone the edit scene");
    expect(playMode.mode() == EditorMode::Simulate && playMode.simulating() && playMode.running() &&
               playMode.simulationMode() == SimulationMode::Simulate && playMode.fixedUpdatesEnabled() &&
               playMode.beginFixedUpdate(),
           "Simulate runs fixed updates in Simulate mode");
    expect(!playMode.enter(scene, assets, options, selection, EditorRuntime::Play).success,
           "Play cannot start while Simulate runs");
    scene.createEntity("Spawned while simulating");

    playMode.pause();
    expect(playMode.mode() == EditorMode::Paused && playMode.simulating() && !playMode.running() &&
               playMode.simulationMode() == SimulationMode::Simulate && !playMode.fixedUpdatesEnabled(),
           "pausing Simulate keeps it a paused Simulate");
    playMode.requestSingleStep();
    expect(playMode.beginFixedUpdate() && !playMode.beginFixedUpdate(), "a paused Simulate single steps once");
    playMode.resume();
    expect(playMode.mode() == EditorMode::Simulate, "resuming a paused Simulate returns to Simulate");

    playMode.stop(scene, selection);
    expect(playMode.mode() == EditorMode::Edit && !playMode.simulating() &&
               playMode.simulationMode() == SimulationMode::Edit,
           "Stop leaves Simulate for Edit");
    expect(snapshot(scene, assets, options) == editSnapshot, "Stop restores the scene from before Simulate");
    expect(playMode.enter(scene, assets, options, selection).success && playMode.mode() == EditorMode::Play &&
               !playMode.simulating() && playMode.simulationMode() == SimulationMode::Play,
           "Play still starts as Play after a Simulate");
    playMode.stop(scene, selection);
}

// The clock the engine reads: real time in Edit, the time scale while running,
// and stopped animation and audio while paused.
void testTimeScale() {
    Scene scene;
    AssetManager assets;
    const SceneDocumentOptions options{.assetRoot = std::filesystem::temp_directory_path()};
    EditorPlayMode playMode;
    std::vector<SceneEntityId> selection;
    expect(playMode.setTimeScale(0.5f) == 0.5f && playMode.setTimeScale(-2.0f) == 0.0f &&
               playMode.setTimeScale(11.0f) == 10.0f && playMode.timeScale() == 10.0f,
           "the time scale clamps to [0, 10]");
    playMode.setTimeScale(0.5f);
    const SimulationClock editClock = playMode.simulationClock();
    expect(editClock.timeScale == 1.0f && !editClock.paused, "Edit mode runs at real time");

    expect(playMode.enter(scene, assets, options, selection).success, "Play starts for the clock test");
    const SimulationClock playing = playMode.simulationClock();
    expect(playing.timeScale == 0.5f && !playing.paused && simulatedSeconds(playing, 0.1f) == 0.05f,
           "Play runs at the time scale");
    playMode.pause();
    const SimulationClock paused = playMode.simulationClock();
    expect(paused.paused && simulatedSeconds(paused, 0.1f) == 0.0f && audioPlaybackRate(paused) == 0.0f,
           "pausing stops animation and audio");
    playMode.stop(scene, selection);
    expect(playMode.timeScale() == 0.5f, "the time scale is kept across runs");
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

    testSimulate(scene, assets, options, editSnapshot);
    testTimeScale();
    return failures == 0 ? 0 : 1;
}
