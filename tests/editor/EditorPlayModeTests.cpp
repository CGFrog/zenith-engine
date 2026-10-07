#include "editor/application/EditorPlayMode.hpp"

#include "engine/assets/Assets.hpp"

#include "support/Doctest.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_BEGIN("EditorPlayMode");

namespace {

std::string snapshot(const Scene& scene, const AssetManager& assets, const SceneDocumentOptions& options) {
    std::string result;
    const SceneDocumentResult serialized = SceneDocument::serialize(scene, assets, options, result);
    CHECK_MESSAGE(serialized.success, "test scene should serialize");
    return result;
}

// The edit scene the Play and Simulate cases run: two entities, one of which
// the runtime deletes, and its snapshot from before any run.
struct EditScene {
    explicit EditScene(std::string_view label)
        : root(label) {}

    testsupport::TempDirectory root;
    AssetManager assets;
    Scene scene;
    Entity retained{scene.createEntity("Retained")};
    Entity deletedAtRuntime{scene.createEntity("Deleted only at runtime")};
    SceneEntityId retainedId{scene.id(retained)};
    SceneEntityId deletedId{scene.id(deletedAtRuntime)};
    SceneDocumentOptions options{.assetRoot = root.path()};
    std::string editSnapshot{snapshot(scene, assets, options)};
};

} // namespace

// Simulate runs on a copy like Play, pauses and steps the same way, and
// resumes into Simulate rather than Play.
TEST_CASE("EditorPlayMode: simulate") {
    EditScene edit("play-mode-simulate");
    Scene& scene = edit.scene;
    AssetManager& assets = edit.assets;
    const SceneDocumentOptions& options = edit.options;
    const std::string& editSnapshot = edit.editSnapshot;
    EditorPlayMode playMode;
    std::vector<SceneEntityId> selection;
    CHECK_MESSAGE(playMode.simulationMode() == SimulationMode::Edit, "Edit mode runs Edit systems");
    CHECK_MESSAGE(playMode.enter(scene, assets, options, selection, EditorRuntime::Simulate).success,
                  "entering Simulate should clone the edit scene");
    CHECK_MESSAGE((playMode.mode() == EditorMode::Simulate && playMode.simulating() && playMode.running() &&
                   playMode.simulationMode() == SimulationMode::Simulate && playMode.fixedUpdatesEnabled() &&
                   playMode.beginFixedUpdate()),
                  "Simulate runs fixed updates in Simulate mode");
    CHECK_MESSAGE(!playMode.enter(scene, assets, options, selection, EditorRuntime::Play).success,
                  "Play cannot start while Simulate runs");
    scene.createEntity("Spawned while simulating");

    playMode.pause();
    CHECK_MESSAGE((playMode.mode() == EditorMode::Paused && playMode.simulating() && !playMode.running() &&
                   playMode.simulationMode() == SimulationMode::Simulate && !playMode.fixedUpdatesEnabled()),
                  "pausing Simulate keeps it a paused Simulate");
    playMode.requestFrameStep(1.0f / 60.0f);
    CHECK_MESSAGE((playMode.steppingFrame() && playMode.beginFixedUpdate()), "a paused Simulate steps a frame");
    playMode.finishFrame();
    CHECK_MESSAGE((!playMode.steppingFrame() && !playMode.beginFixedUpdate()), "the frame step ends with its frame");
    playMode.resume();
    CHECK_MESSAGE(playMode.mode() == EditorMode::Simulate, "resuming a paused Simulate returns to Simulate");

    playMode.stop(scene, selection);
    CHECK_MESSAGE((playMode.mode() == EditorMode::Edit && !playMode.simulating() &&
                   playMode.simulationMode() == SimulationMode::Edit),
                  "Stop leaves Simulate for Edit");
    CHECK_MESSAGE(snapshot(scene, assets, options) == editSnapshot, "Stop restores the scene from before Simulate");
    CHECK_MESSAGE((playMode.enter(scene, assets, options, selection).success && playMode.mode() == EditorMode::Play &&
                   !playMode.simulating() && playMode.simulationMode() == SimulationMode::Play),
                  "Play still starts as Play after a Simulate");
    playMode.stop(scene, selection);
}

// The clock the engine reads: real time in Edit, the time scale while running,
// and stopped animation and audio while paused.
TEST_CASE("EditorPlayMode: time scale") {
    const testsupport::TempDirectory root("play-mode-time-scale");
    Scene scene;
    AssetManager assets;
    const SceneDocumentOptions options{.assetRoot = root.path()};
    EditorPlayMode playMode;
    std::vector<SceneEntityId> selection;
    CHECK_MESSAGE((playMode.setTimeScale(0.5f) == 0.5f && playMode.setTimeScale(-2.0f) == 0.0f &&
                   playMode.setTimeScale(11.0f) == 10.0f && playMode.timeScale() == 10.0f),
                  "the time scale clamps to [0, 10]");
    playMode.setTimeScale(0.5f);
    const SimulationClock editClock = playMode.simulationClock();
    CHECK_MESSAGE((editClock.timeScale == 1.0f && !editClock.paused), "Edit mode runs at real time");

    CHECK_MESSAGE(playMode.enter(scene, assets, options, selection).success, "Play starts for the clock test");
    const SimulationClock playing = playMode.simulationClock();
    CHECK_MESSAGE((playing.timeScale == 0.5f && !playing.paused && simulatedSeconds(playing, 0.1f) == 0.05f),
                  "Play runs at the time scale");
    playMode.pause();
    const SimulationClock paused = playMode.simulationClock();
    CHECK_MESSAGE((paused.paused && simulatedSeconds(paused, 0.1f) == 0.0f && audioPlaybackRate(paused) == 0.0f),
                  "pausing stops animation and audio");
    playMode.stop(scene, selection);
    CHECK_MESSAGE(playMode.timeScale() == 0.5f, "the time scale is kept across runs");
}

// play.step advances exactly one rendered frame: run the engine's frame loop
// (Engine::run) by hand and count what advanced.
TEST_CASE("EditorPlayMode: frame step") {
    const testsupport::TempDirectory root("play-mode-frame-step");
    Scene scene;
    AssetManager assets;
    const SceneDocumentOptions options{.assetRoot = root.path()};
    EditorPlayMode playMode;
    std::vector<SceneEntityId> selection;
    constexpr float fixedStep = 1.0f / 60.0f;
    playMode.setTimeScale(0.25f);
    CHECK_MESSAGE(playMode.enter(scene, assets, options, selection).success, "Play starts for the frame step test");
    playMode.requestFrameStep(fixedStep);
    CHECK_MESSAGE(!playMode.steppingFrame(), "a frame step needs a paused runtime scene");
    playMode.pause();

    struct FrameAdvance {
        float simulated{0.0f};
        int fixedSteps{0};
        float audioRate{0.0f};
    };

    float accumulator = 0.0f;
    const auto frame = [&](float wallSeconds) {
        FrameAdvance advance;
        const SimulationClock clock = playMode.simulationClock();
        advance.simulated = simulatedSeconds(clock, wallSeconds);
        advance.audioRate = audioPlaybackRate(clock);
        if (playMode.fixedUpdatesEnabled()) {
            accumulator += fixedStepAccumulation(clock, wallSeconds);
        } else {
            accumulator = 0.0f;
        }
        while (accumulator >= fixedStep && playMode.beginFixedUpdate()) {
            accumulator -= fixedStep;
            ++advance.fixedSteps;
        }
        playMode.finishFrame();
        return advance;
    };

    const FrameAdvance idle = frame(0.1f);
    CHECK_MESSAGE((idle.simulated == 0.0f && idle.fixedSteps == 0 && idle.audioRate == 0.0f),
                  "a paused frame advances nothing");
    playMode.requestFrameStep(fixedStep);
    CHECK_MESSAGE((playMode.steppingFrame() && playMode.advancesThisFrame()), "the step is pending for the next frame");
    const FrameAdvance stepped = frame(0.1f);
    CHECK_MESSAGE((stepped.simulated == fixedStep && stepped.fixedSteps == 1 && stepped.audioRate == 1.0f),
                  "a frame step advances one fixed time step, ignoring the time scale and the wall-clock frame time");
    const FrameAdvance after = frame(0.1f);
    CHECK_MESSAGE((after.simulated == 0.0f && after.fixedSteps == 0 && !playMode.steppingFrame()),
                  "the frame after a step is paused again");
    playMode.requestFrameStep(fixedStep);
    playMode.resume();
    CHECK_MESSAGE((!playMode.steppingFrame() && playMode.simulationClock().frameStepSeconds == 0.0f),
                  "resuming drops a pending frame step");
    playMode.stop(scene, selection);
}

// An asset only the parked edit scene uses (here through a transient entity,
// which never enters Play) survives a forced collection during Play, and is
// collectable again once Stop hands the scene back to the engine's lease.
TEST_CASE("EditorPlayMode: parked edit scene keeps its assets") {
    const testsupport::TempDirectory root("play-mode-parked-assets");
    AssetManager assets;
    const MaterialHandle material =
        assets.createMaterial(MaterialAsset{.baseColor = {1.0f, 0.0f, 0.0f, 1.0f}}, "edit only").handle;
    Scene scene;
    const Entity preview = scene.createEntity("Generator preview");
    scene.markTransient(preview);
    scene.registry().emplace<MaterialOverrideComponent>(preview, MaterialOverrideComponent{.material = material});
    const SceneDocumentOptions options{.assetRoot = root.path()};
    EditorPlayMode playMode;
    std::vector<SceneEntityId> selection;

    CHECK_MESSAGE(playMode.enter(scene, assets, options, selection).success, "Play starts for the lease test");
    CHECK_MESSAGE(assets.leaseSceneAssets(scene).materials.empty(), "the runtime scene does not use the material");
    assets.collectUnused(true);
    CHECK_MESSAGE(assets.material(material) != nullptr, "entering Play leases the parked edit scene's assets");
    playMode.leaseEditSceneAssets(assets);
    assets.collectUnused(true);
    CHECK_MESSAGE(assets.material(material) != nullptr, "a renewed lease keeps the parked edit scene's assets");

    playMode.stop(scene, selection);
    playMode.leaseEditSceneAssets(assets);
    CHECK_MESSAGE(assets.material(material) != nullptr, "Stop restores the edit scene with its asset loaded");
    scene.destroyEntity(preview);
    assets.collectUnused(true);
    CHECK_MESSAGE(assets.material(material) == nullptr, "Stop releases the parked edit scene's leases");
}

// Play runs on a copy of the edit scene, pauses and steps, and Stop restores
// the document and selection; repeated runs behave the same.
TEST_CASE("EditorPlayMode: stop restores the edit scene and selection after every run") {
    EditScene edit("play-mode-stop");
    Scene& scene = edit.scene;
    AssetManager& assets = edit.assets;
    const SceneDocumentOptions& options = edit.options;
    const SceneEntityId retainedId = edit.retainedId;
    const SceneEntityId deletedId = edit.deletedId;
    std::vector<SceneEntityId> selection{deletedId};

    EditorPlayMode playMode;
    for (int iteration = 0; iteration < 3; ++iteration) {
        CAPTURE(iteration);
        CHECK_MESSAGE(playMode.enter(scene, assets, options, selection).success,
                      "entering Play should clone the edit scene");
        CHECK_MESSAGE((playMode.mode() == EditorMode::Play && playMode.fixedUpdatesEnabled()),
                      "Play should enable fixed simulation updates");
        scene.destroyEntity(scene.findEntity(deletedId));
        scene.registry().get<NameComponent>(scene.findEntity(retainedId)).name = "Runtime mutation";

        playMode.pause();
        CHECK_MESSAGE((playMode.mode() == EditorMode::Paused && !playMode.fixedUpdatesEnabled()),
                      "Paused mode should suspend fixed simulation updates");
        playMode.requestFrameStep(1.0f / 60.0f);
        CHECK_MESSAGE((playMode.fixedUpdatesEnabled() && playMode.beginFixedUpdate()),
                      "a frame step should permit the paused frame's fixed updates");
        playMode.finishFrame();
        CHECK_MESSAGE(!playMode.fixedUpdatesEnabled(), "the frame step should end with its frame");

        selection.clear();
        playMode.stop(scene, selection);
        CHECK_MESSAGE(playMode.mode() == EditorMode::Edit, "Stop should return to Edit mode");
        CHECK_MESSAGE(snapshot(scene, assets, options) == edit.editSnapshot,
                      "Stop should restore the byte-identical pre-play document after runtime deletion and mutation");
        CHECK_MESSAGE((selection.size() == 1 && selection.front() == deletedId),
                      "Stop should restore the pre-play editor selection");
    }
}

TEST_SUITE_END();
