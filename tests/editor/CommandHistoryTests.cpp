#include "editor/document/CommandHistory.hpp"
#include "editor/document/PrefabAuthoring.hpp"

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

void testPrefabAuthoring(const std::filesystem::path& directory) {
    SceneDocumentOptions options{.assetRoot = directory};
    AssetManager assets;
    Scene scene;
    const Entity authoredRoot = scene.createEntity("Authored root");
    scene.registry().emplace<PointLightComponent>(authoredRoot, glm::vec3(1.0f), 10.0f, 4.0f);
    const Entity authoredChild = scene.createEntity("Authored child");
    scene.setParent(authoredChild, authoredRoot);
    EditorCommandContext context{scene, assets, options};
    CommandHistory history(32);
    PrefabAuthoring authoring(directory);

    expect(authoring.create(history, context, authoredRoot, "reusable.prefab").success,
           "prefab creation should execute through command history");
    expect(std::filesystem::is_regular_file(directory / "reusable.prefab"), "create should write prefab source");
    expect(history.undo(context).success && !std::filesystem::exists(directory / "reusable.prefab"),
           "undo should remove a newly created prefab source");
    expect(history.redo(context).success && std::filesystem::is_regular_file(directory / "reusable.prefab"),
           "redo should restore the prefab source");

    std::shared_ptr<PrefabAuthoringInstance> first;
    std::shared_ptr<PrefabAuthoringInstance> second;
    expect(authoring.instantiate(history, context, "reusable.prefab", {100}, first).success && first->linked,
           "prefab instantiate should create a linked authoring instance");
    expect(authoring.instantiate(history, context, "reusable.prefab", {200}, second).success && second->linked,
           "one prefab source should instantiate more than once");
    const size_t entitiesWithInstances = scene.registry().view<const SceneEntityIdComponent>().size();
    expect(history.undo(context).success && !second->linked,
           "undo should remove the latest prefab instance and its link state");
    expect(history.redo(context).success && second->linked &&
               scene.registry().view<const SceneEntityIdComponent>().size() == entitiesWithInstances,
           "redo should restore an identical prefab instance");
    const std::string beforeMissing = snapshot(scene, assets, options);
    const size_t historyBeforeMissing = history.size();
    std::shared_ptr<PrefabAuthoringInstance> missing;
    expect(!authoring.instantiate(history, context, "missing.prefab", {201}, missing) &&
               snapshot(scene, assets, options) == beforeMissing && history.size() == historyBeforeMissing,
           "failed prefab commands should preserve the open document and history timeline");

    expect(authoring.overrideProperty(history, context, first, {{1}, "name", R"("Applied name")"}).success &&
               scene.registry().get<NameComponent>(scene.findEntity(first->root)).name == "Applied name",
           "property override should be applied by an undoable command and stored separately from source");
    expect(authoring.overrideProperty(history, context, second, {{1}, "name", R"("Second instance")"}).success,
           "a second instance should retain an independent override");
    PrefabOverrideInspection inspection;
    expect(authoring.inspect(*first, inspection).success && inspection.active.size() == 1,
           "override inspection should identify active overrides");
    expect(authoring.apply(history, context, first).success, "apply should update the prefab source through history");
    expect(scene.registry().get<NameComponent>(scene.findEntity(first->root)).name == "Applied name" &&
               scene.registry().get<NameComponent>(scene.findEntity(second->root)).name == "Second instance" &&
               first->overrides.empty() && second->overrides.size() == 1,
           "apply should propagate source changes while preserving other instance overrides");
    expect(history.undo(context).success && !first->overrides.empty() &&
               scene.registry().get<NameComponent>(scene.findEntity(first->root)).name == "Applied name" &&
               scene.registry().get<NameComponent>(scene.findEntity(second->root)).name == "Second instance",
           "undo apply should restore source, instances, and override metadata");
    expect(history.redo(context).success && first->overrides.empty() &&
               scene.registry().get<NameComponent>(scene.findEntity(second->root)).name == "Second instance",
           "redo apply should deterministically propagate again");

    expect(authoring.overrideProperty(history, context, first, {{1}, "name", R"("Temporary")"}).success,
           "instance override should remain undoable after apply");
    expect(authoring.revert(history, context, first).success && first->overrides.empty(),
           "revert should refresh from source and clear instance overrides");
    expect(history.undo(context).success && first->overrides.size() == 1,
           "undo revert should restore override metadata and scene state");

    const std::string pointLightOverride = R"({"color":[0.25,0.5,1.0],"intensity":99.0,"range":12.0})";
    expect(authoring.overrideProperty(history, context, first, {{1}, "component.pointLight", pointLightOverride})
                   .success &&
               scene.registry().get<PointLightComponent>(scene.findEntity(first->root)).intensity == 99.0f,
           "component overrides should be separate, inspectable, and undoable");
    expect(history.undo(context).success &&
               scene.registry().get<PointLightComponent>(scene.findEntity(first->root)).intensity == 10.0f,
           "undo component override should restore the inherited component");

    expect(authoring.unpack(history, context, second).success && !second->linked,
           "unpack should preserve entities while removing the source link");
    const SceneEntityId unpackedRoot = second->root;
    expect(scene.findEntity(unpackedRoot) != entt::null, "unpack should keep instantiated entities");
    expect(history.undo(context).success && second->linked, "undo unpack should restore the prefab link");

    expect(authoring.setOverride(*first, {{999}, "name", R"("Orphan")"}).success,
           "orphaned override fixture should be accepted for diagnosis");
    expect(authoring.inspect(*first, inspection).success && inspection.orphaned.size() == 1,
           "inspection should report orphaned overrides separately");
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
    testPrefabAuthoring(directory);
    std::filesystem::remove_all(directory, error);
    return failures == 0 ? 0 : 1;
}
