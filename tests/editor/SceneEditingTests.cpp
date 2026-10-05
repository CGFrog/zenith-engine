#include "editor/application/EditorDiagnostics.hpp"
#include "editor/application/EntityCommands.hpp"
#include "editor/application/SceneEditing.hpp"
#include "editor/document/EditorDocument.hpp"
#include "engine/assets/Assets.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <random>
#include <span>
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

size_t entityCount(const Scene& scene) {
    return scene.registry().view<const SceneEntityIdComponent>().size();
}

std::string serialize(const Scene& scene, const AssetManager& assets, const SceneDocumentOptions& options) {
    std::string output;
    const SceneDocumentResult result = SceneDocument::serialize(scene, assets, options, output);
    expect(result.success, "scene should serialize");
    return output;
}

// Each entity's children by stable id, in RelationshipComponent order.
std::map<uint64_t, std::vector<uint64_t>> hierarchy(const Scene& scene) {
    std::map<uint64_t, std::vector<uint64_t>> result;
    const auto view = scene.registry().view<const SceneEntityIdComponent, const RelationshipComponent>();
    for (const Entity entity : view) {
        std::vector<uint64_t>& children = result[scene.id(entity).value];
        for (const Entity child : view.get<const RelationshipComponent>(entity).children) {
            children.push_back(scene.id(child).value);
        }
    }
    return result;
}

// The hierarchy a full snapshot restore of document produces.
std::map<uint64_t, std::vector<uint64_t>> snapshotHierarchy(const std::string& document, AssetManager& assets,
                                                            const SceneDocumentOptions& options) {
    Scene restored;
    expect(SceneDocument::deserialize(document, assets, options, restored).success, "snapshot should load");
    return hierarchy(restored);
}

bool matchesSnapshot(const Scene& scene, const std::string& document, AssetManager& assets,
                     const SceneDocumentOptions& options) {
    return serialize(scene, assets, options) == document &&
           hierarchy(scene) == snapshotHierarchy(document, assets, options);
}

std::vector<Entity> entitiesById(const Scene& scene) {
    std::vector<Entity> entities;
    const auto view = scene.registry().view<const SceneEntityIdComponent>();
    entities.assign(view.begin(), view.end());
    std::ranges::sort(entities, {}, [&scene](Entity entity) { return scene.id(entity).value; });
    return entities;
}

bool isDescendantOrSelf(const Scene& scene, Entity candidate, Entity ancestor) {
    for (Entity current = candidate; current != entt::null;
         current = scene.registry().get<RelationshipComponent>(current).parent) {
        if (current == ancestor) {
            return true;
        }
    }
    return false;
}

// How randomEdit records its field edits: by capturing the whole scene
// (perform), or only the edited entity (captureEntities).
enum class EditCapture {
    WholeScene,
    Entities,
};

// Runs one randomly chosen edit through SceneEditing. Every operation is
// valid, so none should report an error.
void randomEdit(const SceneEditing& editing, std::mt19937& random, int step, EditCapture capture) {
    Scene& scene = editing.scene();
    const std::vector<Entity> entities = entitiesById(scene);
    const auto pick = [&]() -> Entity {
        return entities.empty() ? Entity{entt::null} : entities[random() % entities.size()];
    };
    const Entity target = pick();
    const uint32_t operation = entities.empty() ? 0 : random() % 8;
    const auto edit = [&](std::string name, std::string key, auto mutation) {
        if (capture == EditCapture::Entities) {
            SceneEditing::EntitySnapshot before;
            if (editing.captureEntities(std::span(&target, 1), before)) {
                mutation();
                editing.commit(std::move(name), std::move(key), std::move(before));
            }
            return;
        }
        editing.perform(std::move(name), std::move(key), [&] {
            mutation();
            return SceneResult{.success = true};
        });
    };
    switch (operation) {
    case 0:
        EntityCommands::createEntity(editing, random() % 3 == 0 ? Entity{entt::null} : target);
        break;
    case 1: {
        const std::array ids{scene.id(target)};
        EntityCommands::deleteEntities(editing, ids, "Delete");
        break;
    }
    case 2: {
        const Entity parent = random() % 4 == 0 ? Entity{entt::null} : pick();
        if (parent == entt::null || !isDescendantOrSelf(scene, parent, target)) {
            editing.perform("Reparent", {}, [&] { return scene.reparent(target, parent); });
        }
        break;
    }
    case 3:
        edit("Rename", {},
             [&] { scene.registry().get<NameComponent>(target).name = "Renamed " + std::to_string(step); });
        break;
    case 4:
        edit("Add Light", {}, [&] {
            scene.registry().emplace_or_replace<PointLightComponent>(target, glm::vec3(1.0f), 2.0f,
                                                                     1.0f + static_cast<float>(step));
        });
        break;
    case 5:
        edit("Remove Light", {}, [&] { scene.registry().remove<PointLightComponent>(target); });
        break;
    case 6:
        edit("Move", {}, [&] {
            scene.registry().get<TransformComponent>(target).local[3] =
                glm::vec4(static_cast<float>(step), 1.0f, 2.0f, 1.0f);
            scene.markTransformDirty(target);
        });
        break;
    default: {
        // A drag records several frames under one key.
        const std::string key = "drag:" + std::to_string(scene.id(target).value);
        for (int frame = 0; frame < 3; ++frame) {
            edit("Drag", key, [&] {
                scene.registry().get<TransformComponent>(target).local[3].y += 0.5f;
                scene.markTransformDirty(target);
            });
        }
        break;
    }
    }
}

// Diff-based undo must give exactly what restoring full snapshots gives: the
// same serialized document and the same child order, at every step, whether
// the field edits capture the whole scene or only their entity.
void testRandomizedEquivalence(EditCapture capture) {
    AssetManager assets;
    Scene scene;
    EditorDocument document(std::filesystem::temp_directory_path());
    CommandHistory history(1024);
    EditorDiagnostics diagnostics;
    const SceneEditing editing(scene, assets, document, history, diagnostics);
    const SceneDocumentOptions& options = document.options();

    std::mt19937 random(20261002);
    // timeline[k] is the snapshot after the k-th recorded command.
    std::vector<std::string> timeline{serialize(scene, assets, options)};
    bool liveMatches = true;
    for (int step = 0; step < 200; ++step) {
        const size_t recorded = history.size();
        randomEdit(editing, random, step, capture);
        const std::string current = serialize(scene, assets, options);
        if (history.size() == recorded + 1) {
            timeline.push_back(current);
        } else {
            // Unchanged or coalesced into the previous command.
            timeline.back() = current;
        }
        liveMatches = liveMatches && hierarchy(scene) == snapshotHierarchy(current, assets, options);
    }
    expect(!diagnostics.hasUnacknowledgedError(), "valid randomized edits should not report errors");
    expect(timeline.size() > 100 && history.size() == timeline.size() - 1,
           "randomized edits should record one command per change");
    expect(liveMatches, "recording an edit should leave the hierarchy a snapshot restore would produce");

    bool undoMatches = true;
    for (size_t index = timeline.size() - 1; index > 0; --index) {
        editing.undo();
        undoMatches = undoMatches && matchesSnapshot(scene, timeline[index - 1], assets, options);
    }
    expect(undoMatches && !history.canUndo(), "every undo should match the full snapshot before that edit");
    bool redoMatches = true;
    for (size_t index = 1; index < timeline.size(); ++index) {
        editing.redo();
        redoMatches = redoMatches && matchesSnapshot(scene, timeline[index], assets, options);
    }
    expect(redoMatches && !history.canRedo(), "every redo should match the full snapshot after that edit");
    expect(!diagnostics.hasUnacknowledgedError(), "randomized undo and redo should not report errors");
}

// An edit that captured only some entities still records the entities it
// created, reports destroying one it did not capture, and edits the runtime
// scene live without recording.
void testEntityCaptureGuards() {
    AssetManager assets;
    Scene scene;
    EditorDocument document(std::filesystem::temp_directory_path());
    CommandHistory history;
    EditorDiagnostics diagnostics;
    const SceneEditing editing(scene, assets, document, history, diagnostics);
    const SceneDocumentOptions& options = document.options();
    const Entity parent = scene.createEntity("Parent");
    const Entity bystander = scene.createEntity("Bystander");
    const std::string original = serialize(scene, assets, options);

    SceneEditing::EntitySnapshot before;
    expect(editing.captureEntities(std::span(&parent, 1), before), "the parent should be captured");
    scene.setParent(scene.createEntity("Child"), parent);
    editing.commit("Add Child", {}, std::move(before));
    const std::string withChild = serialize(scene, assets, options);
    expect(history.size() == 1 && withChild != original, "an entity created by a captured edit should be recorded");
    editing.undo();
    expect(serialize(scene, assets, options) == original, "undo should remove the entity the edit created");
    editing.redo();
    expect(serialize(scene, assets, options) == withChild, "redo should create it again");
    expect(!diagnostics.hasUnacknowledgedError(), "a captured edit that creates an entity is valid");

    expect(editing.captureEntities(std::span(&parent, 1), before), "the parent should be captured again");
    scene.destroyEntity(bystander);
    editing.commit("Destroy", {}, std::move(before));
    expect(diagnostics.hasUnacknowledgedError(), "destroying an entity the edit did not capture should be reported");

    Scene runtimeScene;
    const Entity light = runtimeScene.createEntity("Light");
    runtimeScene.registry().emplace<PointLightComponent>(light);
    CommandHistory runtimeHistory;
    EditorDiagnostics runtimeDiagnostics;
    const SceneEditing live(runtimeScene, assets, document, runtimeHistory, runtimeDiagnostics, &scene);
    SceneEditing::EntitySnapshot liveBefore;
    expect(live.captureEntities(std::span(&light, 1), liveBefore), "a runtime entity should be captured");
    runtimeScene.registry().get<PointLightComponent>(light).range = 9.0f;
    live.commit("Edit Light", {}, std::move(liveBefore));
    expect(runtimeHistory.size() == 0 && runtimeScene.registry().get<PointLightComponent>(light).range == 9.0f &&
               !runtimeDiagnostics.hasUnacknowledgedError(),
           "a captured runtime edit should stay applied and record nothing");
}

// A long history on a large scene stores only the edited entities.
void testLargeSceneHistory() {
    AssetManager assets;
    Scene scene;
    EditorDocument document(std::filesystem::temp_directory_path());
    CommandHistory history;
    EditorDiagnostics diagnostics;
    const SceneEditing editing(scene, assets, document, history, diagnostics);
    const SceneDocumentOptions& options = document.options();

    // Large enough that one full snapshot dwarfs the per-entity diffs; every
    // edit recorded with perform serializes the whole scene twice, so this
    // sets the cost.
    constexpr uint32_t entityCount = 1'000;
    constexpr uint32_t groupSize = 10;
    std::vector<Entity> entities;
    for (uint32_t index = 0; index < entityCount; ++index) {
        const Entity entity = scene.createEntity("Entity " + std::to_string(index));
        if (index % groupSize != 0) {
            scene.registry().get<RelationshipComponent>(entity).parent = entities[index - index % groupSize];
            scene.registry().get<RelationshipComponent>(entities[index - index % groupSize]).children.push_back(entity);
        }
        if (index % 10 == 0) {
            scene.registry().emplace<PointLightComponent>(entity);
        }
        entities.push_back(entity);
    }
    scene.updateWorldTransforms();
    const std::string original = serialize(scene, assets, options);
    const Entity untouched = entities[1];

    constexpr int editCount = 6;
    constexpr uint32_t editStride = entityCount / editCount;
    for (int edit = 0; edit < editCount; ++edit) {
        const Entity target = entities[100 + static_cast<size_t>(edit) * editStride];
        editing.perform("Edit", {}, [&] {
            if (edit % 3 == 0) {
                scene.registry().get<NameComponent>(target).name = "Edited " + std::to_string(edit);
            } else if (edit % 3 == 1) {
                scene.registry().get<TransformComponent>(target).local[3] = glm::vec4(1.0f, 2.0f, 3.0f, 1.0f);
            } else {
                scene.registry().emplace_or_replace<PointLightComponent>(target).range = 5.0f;
            }
            return SceneResult{.success = true};
        });
    }
    // Deleting a whole group and creating an entity are larger edits.
    const std::array groupRoot{scene.id(entities[groupSize * 3])};
    EntityCommands::deleteEntities(editing, groupRoot, "Delete Group");
    EntityCommands::createEntity(editing, entities[groupSize]);
    const std::string edited = serialize(scene, assets, options);
    const size_t commands = history.size();

    const size_t stored = history.storedBytes();
    const size_t snapshotBytes = commands * 2 * original.size();
    std::cout << "Large scene history: " << commands << " commands store " << stored << " bytes; full snapshots ("
              << original.size() << " bytes each, two per command) would store " << snapshotBytes << " bytes\n";
    expect(commands == editCount + 2, "every large-scene edit should be recorded");
    expect(stored * 100 < commands * original.size(),
           "diff history should store far less than one full snapshot per command");

    const auto undoStart = std::chrono::steady_clock::now();
    while (history.canUndo()) {
        editing.undo();
    }
    const auto undoTime = std::chrono::steady_clock::now() - undoStart;
    expect(serialize(scene, assets, options) == original,
           "undoing every large-scene edit should reproduce the original document byte for byte");
    expect(scene.findEntity(scene.id(untouched)) == untouched && scene.registry().valid(untouched),
           "entities no edit touched should keep their EnTT handles through undo");
    while (history.canRedo()) {
        editing.redo();
    }
    expect(serialize(scene, assets, options) == edited, "redoing every large-scene edit should reproduce the edits");
    expect(!diagnostics.hasUnacknowledgedError(), "large-scene undo and redo should not report errors");
    using Milliseconds = std::chrono::duration<double, std::milli>;
    std::cout << "Undoing " << commands << " commands took " << Milliseconds(undoTime).count() << " ms\n";
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

    testRandomizedEquivalence(EditCapture::WholeScene);
    testRandomizedEquivalence(EditCapture::Entities);
    testEntityCaptureGuards();
    testLargeSceneHistory();

    if (failures != 0) {
        std::cerr << failures << " scene editing test(s) failed\n";
        return 1;
    }
    return 0;
}
