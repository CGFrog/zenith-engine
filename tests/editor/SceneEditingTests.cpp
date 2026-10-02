#include "editor/application/EditorDiagnostics.hpp"
#include "editor/application/EntityCommands.hpp"
#include "editor/application/SceneEditing.hpp"
#include "editor/document/EditorDocument.hpp"
#include "engine/assets/Assets.hpp"

#include <filesystem>
#include <iostream>
#include <span>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

size_t entityCount(const Scene& scene) {
    return scene.registry().view<const SceneEntityIdComponent>().size();
}
} // namespace

int main() {
    AssetManager assets;
    Scene scene;
    EditorDocument document(std::filesystem::temp_directory_path());
    CommandHistory history;
    EditorDiagnostics diagnostics;
    const SceneEditing editing(scene, assets, document, history, diagnostics);
    Selection selection;

    const SceneEntityId root = EntityCommands::createEntity(editing, entt::null);
    expect(static_cast<bool>(root) && entityCount(scene) == 1, "creating an entity should add it to the scene");
    expect(history.canUndo() && document.dirty(), "creating an entity should record an undoable, dirtying edit");

    const SceneEntityId child = EntityCommands::createEntity(editing, scene.findEntity(root));
    const Entity childEntity = scene.findEntity(child);
    expect(childEntity != entt::null &&
               scene.registry().get<RelationshipComponent>(childEntity).parent == scene.findEntity(root),
           "creating an entity with a parent should parent it");

    // Coalesced edits merge into one undo step; unchanged commits record nothing.
    const size_t before = history.size();
    for (const char* name : {"First", "Second"}) {
        editing.perform("Rename Entity", "name:root", [&] {
            scene.registry().get<NameComponent>(scene.findEntity(root)).name = name;
            return SceneResult{.success = true};
        });
    }
    expect(history.size() == before + 1, "edits sharing a coalesce key should merge into one command");
    editing.perform("No-op", {}, [] { return SceneResult{.success = true}; });
    expect(history.size() == before + 1, "an edit that changes nothing should not be recorded");
    editing.undo();
    expect(scene.registry().get<NameComponent>(scene.findEntity(root)).name == "Entity",
           "undoing a coalesced edit should restore the value before the first edit");
    editing.redo();
    expect(scene.registry().get<NameComponent>(scene.findEntity(root)).name == "Second",
           "redoing a coalesced edit should apply the last edit");

    selection.selectOnly(root);
    EntityCommands::duplicateSelection(editing, selection);
    expect(entityCount(scene) == 4 && selection.size() == 1 && selection.active() != root,
           "duplicating should copy the subtree and select the copy");
    editing.undo();
    selection.removeMissing(scene);
    expect(entityCount(scene) == 2 && selection.empty(), "one undo should remove the whole duplicate");

    selection.selectOnly(root);
    EntityCommands::deleteSelection(editing, selection);
    expect(entityCount(scene) == 0 && selection.empty(), "deleting a parent should delete its children");
    editing.undo();
    expect(entityCount(scene) == 2 && scene.findEntity(child) != entt::null,
           "undoing a delete should restore the subtree with its stable ids");

    expect(!diagnostics.hasUnacknowledgedError(), "successful edits should not report errors");
    editing.perform("Failing edit", {}, [] { return SceneResult{.error = "deliberate failure"}; });
    expect(diagnostics.hasUnacknowledgedError() && !diagnostics.entries().empty() &&
               diagnostics.entries().back().message == "deliberate failure",
           "a failed edit should be reported to diagnostics");
    diagnostics.acknowledgeErrors();
    expect(!diagnostics.hasUnacknowledgedError() && !diagnostics.entries().empty(),
           "acknowledging errors should keep the diagnostic entries");

    EntityCommands::placeModel(editing, selection, std::filesystem::temp_directory_path() / "missing.gltf", entt::null);
    expect(diagnostics.hasUnacknowledgedError() && entityCount(scene) == 2,
           "placing a missing model should report and leave the scene unchanged");

    if (failures != 0) {
        std::cerr << failures << " scene editing test(s) failed\n";
        return 1;
    }
    return 0;
}
