#include "editor/document/CommandHistory.hpp"

#include "engine/assets/Assets.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

std::string snapshot(const Scene& scene, const AssetManager& assets, const SceneDocumentOptions& options) {
    std::string output;
    const SceneDocumentResult result = SceneDocument::serialize(scene, assets, options, output);
    expect(result.success, "scene snapshot should serialize");
    return output;
}

SceneChanges diff(const std::string& before, const std::string& after) {
    SceneChanges changes;
    const SceneDocumentResult result = SceneDocument::diff(before, after, changes);
    expect(result.success, "documents should diff");
    return changes;
}

void testEntityDiffCoalescing(const std::filesystem::path& directory) {
    SceneDocumentOptions options{.assetRoot = directory};
    AssetManager assets;
    Scene scene;
    scene.createEntity("A");
    scene.createEntity("B");
    const std::string start = snapshot(scene, assets, options);
    scene.registry().get<NameComponent>(scene.findEntity({1})).name = "A1";
    scene.createEntity("Temporary");
    const std::string middle = snapshot(scene, assets, options);
    scene.registry().get<NameComponent>(scene.findEntity({1})).name = "A2";
    scene.registry().get<NameComponent>(scene.findEntity({2})).name = "B2";
    scene.destroyEntity(scene.findEntity({3}));
    const std::string end = snapshot(scene, assets, options);

    SceneDocument::deserialize(start, assets, options, scene);
    EditorCommandContext context{scene, assets, options};
    CommandHistory history;
    expect(history.execute(std::make_unique<EntityDiffCommand>("Drag", diff(start, middle), "drag"), context).success &&
               snapshot(scene, assets, options) == middle,
           "an entity diff command should apply its after state");
    const Entity untouched = scene.findEntity({2});
    expect(history.execute(std::make_unique<EntityDiffCommand>("Drag", diff(middle, end), "drag"), context).success &&
               snapshot(scene, assets, options) == end,
           "a coalescing entity diff command should apply its after state");
    expect(history.size() == 1, "entity diff commands sharing a key should coalesce");
    expect(history.storedBytes() > 0, "history should report the memory its commands keep");

    expect(history.undo(context).success && snapshot(scene, assets, options) == start,
           "undoing a coalesced entity diff should restore the state before the first edit");
    expect(scene.findEntity({3}) == entt::null && scene.findEntity({2}) == untouched,
           "an entity created and removed within one coalesced edit should not reappear, and rebuilt entities "
           "should keep their handles");
    expect(history.redo(context).success && snapshot(scene, assets, options) == end,
           "redoing a coalesced entity diff should apply the last edit");

    scene.registry().get<NameComponent>(scene.findEntity({1})).name = "A3";
    const std::string other = snapshot(scene, assets, options);
    SceneDocument::deserialize(end, assets, options, scene);
    expect(
        history.execute(std::make_unique<EntityDiffCommand>("Rename", diff(end, other), "rename"), context).success &&
            history.size() == 2,
        "entity diff commands with different keys should not coalesce");
    expect(history.execute(std::make_unique<SceneStateCommand>("Rename", other, end, "rename"), context).success &&
               history.size() == 3,
           "a snapshot command should not coalesce into an entity diff command");
}

// A command that only counts how often it is applied, for timeline tests.
class CountingCommand final : public EditorCommand {
  public:
    explicit CountingCommand(int& applied)
        : m_applied(applied) {}

    std::string_view name() const override {
        return "Count";
    }

    EditorCommandResult execute(EditorCommandContext&) override {
        ++m_applied;
        return {.success = true};
    }

    EditorCommandResult undo(EditorCommandContext&) override {
        --m_applied;
        return {.success = true};
    }

  private:
    int& m_applied;
};

void testUndoToMarker(EditorCommandContext& context) {
    int applied = 0;
    CommandHistory history;
    const auto record = [&](CommandHistory& target) {
        expect(target.execute(std::make_unique<CountingCommand>(applied), context).success, "a step should record");
    };
    record(history);
    record(history);
    record(history);
    // An earlier workflow undid two steps, leaving them to redo.
    history.undo(context);
    history.undo(context);
    const CommandHistoryMarker marker = history.marker();
    expect(history.stepsSince(marker) == 0, "no step follows a marker taken now");
    record(history);
    record(history);
    expect(history.size() == 3, "new steps discard the redo steps, so the size grows by less than the steps added");
    expect(history.stepsSince(marker) == 2, "the steps since a marker count those recorded after it");
    for (size_t steps = history.stepsSince(marker); steps > 0; --steps) {
        history.undo(context);
    }
    expect(applied == 1 && history.canUndo() && history.stepsSince(marker) == 0,
           "undoing the steps since a marker undoes exactly those steps");
    expect(history.redo(context).success && history.stepsSince(marker) == 1,
           "a redone step recorded after the marker counts again");

    applied = 0;
    CommandHistory limited(2);
    record(limited);
    const CommandHistoryMarker early = limited.marker();
    record(limited);
    record(limited);
    record(limited);
    expect(limited.stepsSince(early) == 2, "steps the limit dropped are not counted");
    limited.clear();
    expect(limited.stepsSince(early) == 0, "a cleared history has no steps since any marker");
}
} // namespace

int main() {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "zenith_command_history_tests";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    SceneDocumentOptions options{.assetRoot = directory};
    AssetManager assets;
    Scene scene;
    const Entity entity = scene.createEntity("Before");
    const std::string before = snapshot(scene, assets, options);
    scene.registry().get<NameComponent>(entity).name = "After";
    const std::string after = snapshot(scene, assets, options);
    SceneDocument::deserialize(before, assets, options, scene);
    EditorCommandContext context{scene, assets, options};

    CommandHistory history(2);
    expect(history.execute(std::make_unique<SceneStateCommand>("Rename", before, after), context).success,
           "command should execute");
    expect(scene.registry().get<NameComponent>(scene.findEntity({1})).name == "After",
           "execute should apply after state");
    expect(history.dirty() && history.canUndo() && !history.canRedo(), "execute should advance dirty history");
    expect(history.undo(context).success, "undo should succeed");
    expect(snapshot(scene, assets, options) == before, "undo should restore byte-identical serialized content");
    expect(history.redo(context).success, "redo should succeed");
    expect(snapshot(scene, assets, options) == after, "redo should restore byte-identical serialized content");
    history.crossBoundary(CommandHistoryBoundary::SaveDocument);
    expect(!history.dirty(), "markSaved should establish the clean history position");

    const size_t savedHistorySize = history.size();
    history.crossBoundary(CommandHistoryBoundary::EnterPlayMode);
    history.crossBoundary(CommandHistoryBoundary::LeavePlayMode);
    expect(history.size() == savedHistorySize && history.canUndo() && !history.dirty(),
           "play-mode transitions should preserve editable document history and its save point");

    scene.registry().get<NameComponent>(scene.findEntity({1})).name = "After 2";
    const std::string after2 = snapshot(scene, assets, options);
    SceneDocument::deserialize(after, assets, options, scene);
    expect(history.execute(std::make_unique<SceneStateCommand>("Rename", after, after2, "name:1"), context).success,
           "first coalesced command should execute");
    scene.registry().get<NameComponent>(scene.findEntity({1})).name = "After 3";
    const std::string after3 = snapshot(scene, assets, options);
    SceneDocument::deserialize(after2, assets, options, scene);
    expect(history.execute(std::make_unique<SceneStateCommand>("Rename", after2, after3, "name:1"), context).success,
           "second coalesced command should execute");
    expect(history.size() == 2, "continuous edits with one key should coalesce into one command");
    expect(history.undo(context).success, "coalesced edit should undo");
    expect(snapshot(scene, assets, options) == after, "coalesced undo should restore state before the full drag");

    SceneDocument::deserialize(after2, assets, options, scene);
    history.execute(std::make_unique<SceneStateCommand>("Edit 2", after, after2), context);
    const uint64_t revisionAtLimit = history.revision();
    history.execute(std::make_unique<SceneStateCommand>("Edit 3", after2, after3), context);
    expect(history.size() == 2, "history should enforce its configured command limit");
    expect(history.revision() > revisionAtLimit,
           "the revision should keep advancing after the size stops growing at the limit");
    const uint64_t revisionBeforeUndo = history.revision();
    history.undo(context);
    history.redo(context);
    expect(history.revision() == revisionBeforeUndo + 2, "undo and redo should each advance the revision");

    history.crossBoundary(CommandHistoryBoundary::OpenDocument);
    expect(history.size() == 0 && !history.canUndo() && !history.canRedo() && !history.dirty(),
           "Open should start a clean history timeline");
    history.execute(std::make_unique<SceneStateCommand>("Edit after open", after3, after2), context);
    history.crossBoundary(CommandHistoryBoundary::NewDocument);
    expect(history.size() == 0 && !history.canUndo() && !history.canRedo() && !history.dirty(),
           "New should start a clean history timeline");
    testEntityDiffCoalescing(directory);
    testUndoToMarker(context);
    std::filesystem::remove_all(directory, error);
    return failures == 0 ? 0 : 1;
}
