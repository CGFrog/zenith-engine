#include "editor/document/CommandHistory.hpp"

#include "engine/assets/Assets.hpp"

#include "support/Doctest.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace {

std::string snapshot(const Scene& scene, const AssetManager& assets, const SceneDocumentOptions& options) {
    std::string output;
    const SceneDocumentResult result = SceneDocument::serialize(scene, assets, options, output);
    CHECK_MESSAGE(result.success, "scene snapshot should serialize");
    return output;
}

SceneChanges diff(const std::string& before, const std::string& after) {
    SceneChanges changes;
    const SceneDocumentResult result = SceneDocument::diff(before, after, changes);
    CHECK_MESSAGE(result.success, "documents should diff");
    return changes;
}

// A scene with one entity whose name is renamed "Before", "After", "After 2"
// and "After 3", the snapshot of each, and the scene left at "Before".
struct RenameFixture {
    RenameFixture() {
        const Entity entity = scene.createEntity("Before");
        before = snapshot(scene, assets, options);
        scene.registry().get<NameComponent>(entity).name = "After";
        after = snapshot(scene, assets, options);
        scene.registry().get<NameComponent>(entity).name = "After 2";
        after2 = snapshot(scene, assets, options);
        scene.registry().get<NameComponent>(entity).name = "After 3";
        after3 = snapshot(scene, assets, options);
        SceneDocument::deserialize(before, assets, options, scene);
    }

    const testsupport::TempDirectory directory{"command-history"};
    SceneDocumentOptions options{.assetRoot = directory.path()};
    AssetManager assets;
    Scene scene;
    std::string before;
    std::string after;
    std::string after2;
    std::string after3;
    EditorCommandContext context{scene, assets, options};
};

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

} // namespace

TEST_SUITE_BEGIN("CommandHistory");

TEST_CASE("CommandHistory: execute undo redo and save move the history position") {
    RenameFixture fixture;
    EditorCommandContext& context = fixture.context;
    Scene& scene = fixture.scene;

    CommandHistory history(2);
    CHECK_MESSAGE(
        history.execute(std::make_unique<SceneStateCommand>("Rename", fixture.before, fixture.after), context).success,
        "command should execute");
    CHECK_MESSAGE(scene.registry().get<NameComponent>(scene.findEntity({1})).name == "After",
                  "execute should apply after state");
    CHECK_MESSAGE((history.dirty() && history.canUndo() && !history.canRedo()), "execute should advance dirty history");
    CHECK_MESSAGE(history.undo(context).success, "undo should succeed");
    CHECK_MESSAGE(snapshot(scene, fixture.assets, fixture.options) == fixture.before,
                  "undo should restore byte-identical serialized content");
    CHECK_MESSAGE(history.redo(context).success, "redo should succeed");
    CHECK_MESSAGE(snapshot(scene, fixture.assets, fixture.options) == fixture.after,
                  "redo should restore byte-identical serialized content");
    history.crossBoundary(CommandHistoryBoundary::SaveDocument);
    CHECK_MESSAGE(!history.dirty(), "markSaved should establish the clean history position");

    const size_t savedHistorySize = history.size();
    history.crossBoundary(CommandHistoryBoundary::EnterPlayMode);
    history.crossBoundary(CommandHistoryBoundary::LeavePlayMode);
    CHECK_MESSAGE((history.size() == savedHistorySize && history.canUndo() && !history.dirty()),
                  "play-mode transitions should preserve editable document history and its save point");
}

TEST_CASE("CommandHistory: continuous edits with one key coalesce") {
    RenameFixture fixture;
    EditorCommandContext& context = fixture.context;
    Scene& scene = fixture.scene;
    CommandHistory history(2);
    history.execute(std::make_unique<SceneStateCommand>("Rename", fixture.before, fixture.after), context);

    SceneDocument::deserialize(fixture.after, fixture.assets, fixture.options, scene);
    CHECK_MESSAGE(
        history.execute(std::make_unique<SceneStateCommand>("Rename", fixture.after, fixture.after2, "name:1"), context)
            .success,
        "first coalesced command should execute");
    SceneDocument::deserialize(fixture.after2, fixture.assets, fixture.options, scene);
    CHECK_MESSAGE(
        history
            .execute(std::make_unique<SceneStateCommand>("Rename", fixture.after2, fixture.after3, "name:1"), context)
            .success,
        "second coalesced command should execute");
    CHECK_MESSAGE(history.size() == 2, "continuous edits with one key should coalesce into one command");
    CHECK_MESSAGE(history.undo(context).success, "coalesced edit should undo");
    CHECK_MESSAGE(snapshot(scene, fixture.assets, fixture.options) == fixture.after,
                  "coalesced undo should restore state before the full drag");
}

TEST_CASE("CommandHistory: the command limit holds while the revision keeps advancing") {
    RenameFixture fixture;
    EditorCommandContext& context = fixture.context;
    CommandHistory history(2);
    history.execute(std::make_unique<SceneStateCommand>("Rename", fixture.before, fixture.after), context);

    SceneDocument::deserialize(fixture.after2, fixture.assets, fixture.options, fixture.scene);
    history.execute(std::make_unique<SceneStateCommand>("Edit 2", fixture.after, fixture.after2), context);
    const uint64_t revisionAtLimit = history.revision();
    history.execute(std::make_unique<SceneStateCommand>("Edit 3", fixture.after2, fixture.after3), context);
    CHECK_MESSAGE(history.size() == 2, "history should enforce its configured command limit");
    CHECK_MESSAGE(history.revision() > revisionAtLimit,
                  "the revision should keep advancing after the size stops growing at the limit");
    const uint64_t revisionBeforeUndo = history.revision();
    history.undo(context);
    history.redo(context);
    CHECK_MESSAGE(history.revision() == revisionBeforeUndo + 2, "undo and redo should each advance the revision");
}

TEST_CASE("CommandHistory: open and new start a clean timeline") {
    RenameFixture fixture;
    EditorCommandContext& context = fixture.context;
    CommandHistory history(2);
    history.execute(std::make_unique<SceneStateCommand>("Rename", fixture.before, fixture.after), context);

    history.crossBoundary(CommandHistoryBoundary::OpenDocument);
    CHECK_MESSAGE((history.size() == 0 && !history.canUndo() && !history.canRedo() && !history.dirty()),
                  "Open should start a clean history timeline");
    history.execute(std::make_unique<SceneStateCommand>("Edit after open", fixture.after3, fixture.after2), context);
    history.crossBoundary(CommandHistoryBoundary::NewDocument);
    CHECK_MESSAGE((history.size() == 0 && !history.canUndo() && !history.canRedo() && !history.dirty()),
                  "New should start a clean history timeline");
}

TEST_CASE("CommandHistory: entity diff coalescing") {
    const testsupport::TempDirectory directory("command-history-entity-diff");
    SceneDocumentOptions options{.assetRoot = directory.path()};
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
    CHECK_MESSAGE(
        (history.execute(std::make_unique<EntityDiffCommand>("Drag", diff(start, middle), "drag"), context).success &&
         snapshot(scene, assets, options) == middle),
        "an entity diff command should apply its after state");
    const Entity untouched = scene.findEntity({2});
    CHECK_MESSAGE(
        (history.execute(std::make_unique<EntityDiffCommand>("Drag", diff(middle, end), "drag"), context).success &&
         snapshot(scene, assets, options) == end),
        "a coalescing entity diff command should apply its after state");
    CHECK_MESSAGE(history.size() == 1, "entity diff commands sharing a key should coalesce");
    CHECK_MESSAGE(history.storedBytes() > 0, "history should report the memory its commands keep");

    CHECK_MESSAGE((history.undo(context).success && snapshot(scene, assets, options) == start),
                  "undoing a coalesced entity diff should restore the state before the first edit");
    CHECK_MESSAGE((scene.findEntity({3}) == entt::null && scene.findEntity({2}) == untouched),
                  "an entity created and removed within one coalesced edit should not reappear, and rebuilt entities "
                  "should keep their handles");
    CHECK_MESSAGE((history.redo(context).success && snapshot(scene, assets, options) == end),
                  "redoing a coalesced entity diff should apply the last edit");

    scene.registry().get<NameComponent>(scene.findEntity({1})).name = "A3";
    const std::string other = snapshot(scene, assets, options);
    SceneDocument::deserialize(end, assets, options, scene);
    CHECK_MESSAGE(
        (history.execute(std::make_unique<EntityDiffCommand>("Rename", diff(end, other), "rename"), context).success &&
         history.size() == 2),
        "entity diff commands with different keys should not coalesce");
    CHECK_MESSAGE(
        (history.execute(std::make_unique<SceneStateCommand>("Rename", other, end, "rename"), context).success &&
         history.size() == 3),
        "a snapshot command should not coalesce into an entity diff command");
}

TEST_CASE("CommandHistory: undo to marker") {
    RenameFixture fixture;
    EditorCommandContext& context = fixture.context;
    int applied = 0;
    CommandHistory history;
    const auto record = [&](CommandHistory& target) {
        CHECK_MESSAGE(target.execute(std::make_unique<CountingCommand>(applied), context).success,
                      "a step should record");
    };
    record(history);
    record(history);
    record(history);
    // An earlier workflow undid two steps, leaving them to redo.
    history.undo(context);
    history.undo(context);
    const CommandHistoryMarker marker = history.marker();
    CHECK_MESSAGE(history.stepsSince(marker) == 0, "no step follows a marker taken now");
    record(history);
    record(history);
    CHECK_MESSAGE(history.size() == 3,
                  "new steps discard the redo steps, so the size grows by less than the steps added");
    CHECK_MESSAGE(history.stepsSince(marker) == 2, "the steps since a marker count those recorded after it");
    for (size_t steps = history.stepsSince(marker); steps > 0; --steps) {
        history.undo(context);
    }
    CHECK_MESSAGE((applied == 1 && history.canUndo() && history.stepsSince(marker) == 0),
                  "undoing the steps since a marker undoes exactly those steps");
    CHECK_MESSAGE((history.redo(context).success && history.stepsSince(marker) == 1),
                  "a redone step recorded after the marker counts again");

    applied = 0;
    CommandHistory limited(2);
    record(limited);
    const CommandHistoryMarker early = limited.marker();
    record(limited);
    record(limited);
    record(limited);
    CHECK_MESSAGE(limited.stepsSince(early) == 2, "steps the limit dropped are not counted");
    limited.clear();
    CHECK_MESSAGE(limited.stepsSince(early) == 0, "a cleared history has no steps since any marker");
}

TEST_SUITE_END();
