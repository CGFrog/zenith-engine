#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/operators/CollectionOperators.hpp"
#include "editor/operators/DefaultKeymap.hpp"
#include "editor/operators/Keymap.hpp"
#include "editor/operators/KeymapDispatch.hpp"
#include "editor/operators/KeymapEditor.hpp"
#include "editor/operators/LastOperatorPanel.hpp"
#include "editor/operators/ObjectOperators.hpp"
#include "editor/operators/OperatorRunner.hpp"
#include "editor/operators/OperatorSearch.hpp"
#include "editor/operators/PlacementOperators.hpp"
#include "editor/operators/SelectOperators.hpp"
#include "engine/assets/Assets.hpp"

#include "support/Doctest.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

TEST_SUITE_BEGIN("Operator");

namespace {

// A document, history, and selection editing a real scene, as the editor
// session owns them, but without a window.
struct EditingFixture {
    AssetManager assets;
    Scene scene;
    testsupport::TempDirectory documentRoot{"operator-editing"};
    EditorDocument document{documentRoot.path()};
    CommandHistory history;
    EditorDiagnostics diagnostics;
    Selection selection;
    OperatorRegistry registry;
    OperatorRunner runner{registry, history};

    EditingFixture() {
        CHECK_MESSAGE(registerObjectOperators(registry).success, "the object operators should register");
    }

    OperatorContext context(EditorMode mode = EditorMode::Edit) {
        return {.editing = SceneEditing(scene, assets, document, history, diagnostics),
                .selection = selection,
                .mode = mode};
    }

    OperatorResult invoke(std::string_view id, std::string_view properties = {}) {
        OperatorContext operatorContext = context();
        return runner.invokeWithJson(id, operatorContext, properties);
    }
};

size_t entityCount(const Scene& scene) {
    return scene.registry().view<const SceneEntityIdComponent>().size();
}

// Names and rounded world positions of every entity, sorted, so scenes with
// different entity ids can be compared.
std::vector<std::tuple<std::string, int, int, int>> sceneSummary(Scene& scene) {
    scene.updateWorldTransforms();
    std::vector<std::tuple<std::string, int, int, int>> summary;
    for (const Entity entity : scene.registry().view<const NameComponent, const TransformComponent>()) {
        const glm::vec3 position(scene.registry().get<TransformComponent>(entity).world[3]);
        summary.emplace_back(
            scene.registry().get<NameComponent>(entity).name, static_cast<int>(std::lround(position.x * 100.0f)),
            static_cast<int>(std::lround(position.y * 100.0f)), static_cast<int>(std::lround(position.z * 100.0f)));
    }
    std::ranges::sort(summary);
    return summary;
}

glm::vec3 position(const Scene& scene, SceneEntityId id) {
    return glm::vec3(scene.registry().get<TransformComponent>(scene.findEntity(id)).local[3]);
}

OperatorProperties withLocation(const OperatorRegistry& registry, std::string_view id, glm::vec3 location) {
    OperatorProperties properties = registry.find(id)->properties;
    properties.get<AddObjectProperties>()->location = location;
    return properties;
}

TEST_CASE("Operator: registry") {
    OperatorRegistry registry;
    const auto run = [](OperatorContext&, OperatorProperties&) { return OperatorResult::finished(); };
    CHECK_MESSAGE(registry.add({.id = "game.jump", .label = "Jump", .execute = run}).success,
                  "operators from outside the editor should register");
    CHECK_MESSAGE(!registry.add({.id = "game.jump", .label = "Jump again", .execute = run}).success,
                  "duplicate operator ids should be rejected");
    CHECK_MESSAGE(!registry.add({.id = "", .label = "Nameless", .execute = run}).success, "operators need an id");
    CHECK_MESSAGE(!registry.add({.id = "game.idle", .label = "Idle"}).success, "operators need an execute function");
    CHECK_MESSAGE((registry.find("game.jump") != nullptr && registry.find("game.missing") == nullptr),
                  "operators should be found by id");
    CHECK_MESSAGE(registry.size() == 1, "rejected operators should not be registered");
}

TEST_CASE("Operator: poll and undo") {
    EditingFixture fixture;
    const OperatorResult nothingSelected = fixture.invoke("object.delete");
    CHECK_MESSAGE((nothingSelected.status == OperatorStatus::Cancelled && !fixture.history.canUndo()),
                  "an operator whose poll fails should be cancelled without changing history");
    {
        // Object operators edit the running scene live in Play
        // (LiveEditingTests); document-only ones refuse.
        OperatorContext playing = fixture.context(EditorMode::Play);
        CHECK_MESSAGE((fixture.runner.invoke("collection.new", playing).status == OperatorStatus::Cancelled &&
                       fixture.scene.collections().empty()),
                      "document-only operators should not run in Play mode");
    }
    const OperatorResult unknown = fixture.invoke("object.missing");
    CHECK_MESSAGE((unknown.status == OperatorStatus::Failed && fixture.diagnostics.hasUnacknowledgedError()),
                  "unknown operators should fail and be reported");
    fixture.diagnostics.acknowledgeErrors();

    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("object.add_empty", R"({"location":[1,2,3]})"))),
                  "adding an empty should finish");
    CHECK_MESSAGE((entityCount(fixture.scene) == 1 && fixture.selection.size() == 1 &&
                   position(fixture.scene, *fixture.selection.active()) == glm::vec3(1.0f, 2.0f, 3.0f)),
                  "add operators should place and select the new object");
    CHECK_MESSAGE((fixture.history.size() == 1 && fixture.document.dirty()),
                  "an Undo operator should record exactly one undo step");

    fixture.invoke("object.select_all", R"({"action":"deselect"})");
    CHECK_MESSAGE((fixture.selection.empty() && fixture.history.size() == 1),
                  "selection operators should not record undo steps");
    fixture.invoke("object.select_all");
    CHECK_MESSAGE(fixture.selection.size() == 1, "select all should select every object");
    fixture.invoke("object.select_all", R"({"action":"invert"})");
    CHECK_MESSAGE(fixture.selection.empty(), "invert should deselect a fully selected scene");

    CHECK_MESSAGE(fixture.invoke("object.select_all", R"({"action":"sideways"})").status == OperatorStatus::Failed,
                  "invalid binding properties should fail");
    fixture.diagnostics.acknowledgeErrors();

    fixture.invoke("object.select_all");
    fixture.invoke("object.location_clear");
    CHECK_MESSAGE(
        (position(fixture.scene, *fixture.selection.active()) == glm::vec3(0.0f) && fixture.history.size() == 2),
        "clearing the location should be one undoable step");
    fixture.invoke("object.location_clear");
    CHECK_MESSAGE(fixture.history.size() == 2, "an operator that changes nothing should record nothing");

    fixture.invoke("object.component_add", R"({"component":"point_light"})");
    const Entity empty = fixture.scene.findEntity(*fixture.selection.active());
    CHECK_MESSAGE((fixture.scene.registry().all_of<PointLightComponent>(empty) && fixture.history.size() == 3),
                  "adding a component by key should be undoable");
    fixture.invoke("object.component_remove", R"({"component":"point_light"})");
    CHECK_MESSAGE(
        (!fixture.scene.registry().all_of<PointLightComponent>(fixture.scene.findEntity(*fixture.selection.active()))),
        "removing a component by key should remove it");
    CHECK_MESSAGE((fixture.invoke("object.component_add", R"({"component":"not_a_component"})").status ==
                       OperatorStatus::Failed &&
                   fixture.diagnostics.hasUnacknowledgedError()),
                  "an unknown component should fail with a diagnostic");

    fixture.invoke("object.rename", R"({"name":"Renamed"})");
    CHECK_MESSAGE(
        (fixture.scene.registry().get<NameComponent>(fixture.scene.findEntity(*fixture.selection.active())).name ==
         "Renamed"),
        "rename should name the active object");

    fixture.invoke("object.delete");
    CHECK_MESSAGE((entityCount(fixture.scene) == 0 && fixture.selection.empty()), "delete should remove the selection");
    SceneEditing(fixture.scene, fixture.assets, fixture.document, fixture.history, fixture.diagnostics).undo();
    CHECK_MESSAGE(entityCount(fixture.scene) == 1, "one undo should restore a deleted object");
}

std::string documentOf(EditingFixture& fixture) {
    std::string document;
    CHECK_MESSAGE(SceneDocument::serialize(fixture.scene, fixture.assets, fixture.document.options(), document).success,
                  "the scene should serialize");
    return document;
}

// Undoes and redoes the last step, checking both land on the recorded states.
void checkUndoRedo(EditingFixture& fixture, const std::string& before, const std::string& after, const char* message) {
    const SceneEditing editing(fixture.scene, fixture.assets, fixture.document, fixture.history, fixture.diagnostics);
    editing.undo();
    const bool undone = documentOf(fixture) == before;
    editing.redo();
    CHECK_MESSAGE((undone && documentOf(fixture) == after), message);
}

const CollectionMembershipComponent* membershipOf(EditingFixture& fixture, SceneEntityId id) {
    return fixture.scene.registry().try_get<CollectionMembershipComponent>(fixture.scene.findEntity(id));
}

TEST_CASE("Operator: collection operators") {
    EditingFixture fixture;
    CHECK_MESSAGE(fixture.invoke("collection.link", R"({"collection":1})").status == OperatorStatus::Cancelled,
                  "linking should not run without collections");
    fixture.invoke("object.add_empty");
    const SceneEntityId first = *fixture.selection.active();
    fixture.invoke("object.add_empty", R"({"location":[2,0,0]})");
    const SceneEntityId second = *fixture.selection.active();

    std::string before = documentOf(fixture);
    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("collection.new", R"({"name":"Props"})"))),
                  "a new collection should be created");
    const SceneCollection* props = fixture.scene.collections().find({1});
    CHECK_MESSAGE((props != nullptr && props->name == "Props" && membershipOf(fixture, second) != nullptr &&
                   membershipOf(fixture, second)->contains({1}) && membershipOf(fixture, first) == nullptr),
                  "a new collection should link the selection");
    CHECK_MESSAGE((fixture.runner.lastOperator() != nullptr &&
                   fixture.runner.lastOperator()->properties.get<NewCollectionProperties>()->created == 1),
                  "collection.new should report the id it created");
    checkUndoRedo(fixture, before, documentOf(fixture), "creating a collection should undo and redo");

    fixture.invoke("collection.new", R"({"name":"Crates","parent":1,"link_selection":false})");
    CHECK_MESSAGE(
        (fixture.scene.collections().find({2}) != nullptr && fixture.scene.collections().find({2})->parent.value == 1),
        "a collection should nest inside its parent");

    fixture.selection.assign({first, second});
    before = documentOf(fixture);
    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("collection.link", R"({"collection":2})")) &&
                   membershipOf(fixture, first)->contains({2}) &&
                   membershipOf(fixture, second)->collections == std::vector<SceneCollectionId>{{1}, {2}}),
                  "linking should add every selected object, keeping its other collections");
    checkUndoRedo(fixture, before, documentOf(fixture), "linking should undo and redo");
    CHECK_MESSAGE(fixture.invoke("collection.link", R"({"collection":2})").status == OperatorStatus::Cancelled,
                  "linking objects already in the collection should change nothing");
    CHECK_MESSAGE(fixture.invoke("collection.link", R"({"collection":9})").status == OperatorStatus::Failed,
                  "linking to an unknown collection should fail");
    fixture.diagnostics.acknowledgeErrors();

    before = documentOf(fixture);
    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("collection.unlink", R"({"collection":2})")) &&
                   membershipOf(fixture, first) == nullptr && membershipOf(fixture, second)->contains({1})),
                  "unlinking should remove the selected objects from only that collection");
    checkUndoRedo(fixture, before, documentOf(fixture), "unlinking should undo and redo");

    before = documentOf(fixture);
    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("collection.set_flag",
                                                    R"({"collection":1,"flag":"renderable","value":false})")) &&
                   !fixture.scene.collections().find({1})->renderable),
                  "set_flag should change a collection flag");
    checkUndoRedo(fixture, before, documentOf(fixture), "changing a collection flag should undo and redo");
    CHECK_MESSAGE(
        fixture.invoke("collection.set_flag", R"({"collection":1,"flag":"renderable","value":false})").status ==
            OperatorStatus::Cancelled,
        "setting a flag to its current value should change nothing");

    before = documentOf(fixture);
    CHECK_MESSAGE(
        (static_cast<bool>(fixture.invoke("object.collection_instance_add", R"({"collection":1,"location":[0,0,5]})"))),
        "a collection instance should be added");
    const Entity instance = fixture.scene.findEntity(*fixture.selection.active());
    CHECK_MESSAGE((fixture.scene.registry().get<CollectionInstanceComponent>(instance).collection.value == 1 &&
                   fixture.scene.registry().get<NameComponent>(instance).name == "Props"),
                  "the instance should reference the collection and take its name");
    checkUndoRedo(fixture, before, documentOf(fixture), "adding an instance should undo and redo");

    fixture.selection.assign({second});
    fixture.invoke("collection.link", R"({"collection":2})");
    before = documentOf(fixture);
    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("collection.delete", R"({"collection":1})"))),
                  "delete should finish");
    CHECK_MESSAGE((fixture.scene.collections().find({1}) == nullptr &&
                   fixture.scene.collections().find({2})->parent == SceneCollectionId{} &&
                   membershipOf(fixture, second)->collections == std::vector<SceneCollectionId>{{2}} &&
                   !fixture.scene.registry().all_of<CollectionInstanceComponent>(instance)),
                  "deleting a collection should lift its contents and drop its instances");
    checkUndoRedo(fixture, before, documentOf(fixture), "deleting a collection should undo and redo");
    CHECK_MESSAGE(!fixture.diagnostics.hasUnacknowledgedError(), "collection operators should not report errors");
}

TEST_CASE("Operator: visibility operators") {
    EditingFixture fixture;
    fixture.invoke("object.add_empty");
    const SceneEntityId first = *fixture.selection.active();
    fixture.invoke("object.add_empty");
    const SceneEntityId second = *fixture.selection.active();
    fixture.invoke("object.add_empty");
    const SceneEntityId third = *fixture.selection.active();
    const auto isHidden = [&fixture](SceneEntityId id) {
        const auto* visibility = fixture.scene.registry().try_get<VisibilityComponent>(fixture.scene.findEntity(id));
        return visibility != nullptr && visibility->hidden;
    };
    CHECK_MESSAGE(fixture.invoke("object.unhide").status == OperatorStatus::Cancelled,
                  "reveal should not run while nothing is hidden");

    fixture.selection.assign({first});
    std::string before = documentOf(fixture);
    const uint64_t renderable = fixture.scene.revisions().renderable;
    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("object.hide")) && isHidden(first) && fixture.selection.empty()),
                  "hide should hide and deselect the selection");
    CHECK_MESSAGE(fixture.scene.revisions().renderable != renderable, "hiding should invalidate render caches");
    checkUndoRedo(fixture, before, documentOf(fixture), "hide should undo and redo");
    fixture.invoke("object.select_all");
    CHECK_MESSAGE((fixture.selection.size() == 2 && !fixture.selection.contains(first)),
                  "select all should skip hidden objects");

    fixture.selection.assign({second});
    before = documentOf(fixture);
    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("object.isolate")) && isHidden(third) && !isHidden(second) &&
                   fixture.selection.contains(second)),
                  "hide unselected should hide everything else");
    checkUndoRedo(fixture, before, documentOf(fixture), "hide unselected should undo and redo");

    before = documentOf(fixture);
    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("object.unhide")) && !isHidden(first) && !isHidden(third) &&
                   fixture.selection.size() == 2 && fixture.selection.contains(first) &&
                   fixture.selection.contains(third)),
                  "reveal should show and select every hidden object");
    CHECK_MESSAGE((!fixture.scene.registry().any_of<VisibilityComponent>(fixture.scene.findEntity(first))),
                  "revealed objects should not keep a default visibility component");
    checkUndoRedo(fixture, before, documentOf(fixture), "reveal should undo and redo");
    // Three added empties, then hide, hide unselected, and reveal.
    CHECK_MESSAGE(fixture.history.size() == 6, "each visibility operator should record one undo step");
}

TEST_CASE("Operator: adjust last operation") {
    EditingFixture fixture;
    {
        OperatorContext context = fixture.context();
        const OperatorProperties first = withLocation(fixture.registry, "object.add_empty", {1.0f, 0.0f, 0.0f});
        fixture.runner.invoke("object.add_empty", context, &first);
    }
    const LastOperator* last = fixture.runner.lastOperator();
    CHECK_MESSAGE((last != nullptr && last->id == "object.add_empty" && last->recordedUndoStep),
                  "a Register operator should become the last operation");
    const size_t historyAfterFirst = fixture.history.size();

    const OperatorProperties adjusted = withLocation(fixture.registry, "object.add_empty", {0.0f, 4.0f, 0.0f});
    {
        OperatorContext context = fixture.context();
        CHECK_MESSAGE((static_cast<bool>(fixture.runner.adjustLast(context, adjusted))), "adjusting should re-run");
    }
    CHECK_MESSAGE((fixture.history.size() == historyAfterFirst && entityCount(fixture.scene) == 1),
                  "adjusting should replace the operation's undo step, not add one");

    EditingFixture fresh;
    {
        OperatorContext context = fresh.context();
        fresh.runner.invoke("object.add_empty", context, &adjusted);
    }
    CHECK_MESSAGE(sceneSummary(fixture.scene) == sceneSummary(fresh.scene),
                  "an adjusted operation should match a fresh run with the edited properties");
    last = fixture.runner.lastOperator();
    CHECK_MESSAGE(
        (last != nullptr && last->properties.get<AddObjectProperties>()->location == glm::vec3(0.0f, 4.0f, 0.0f)),
        "the last operation should show the adjusted properties");

    // Duplicate restores the selection it first saw before re-running.
    fixture.invoke("object.duplicate", R"({"offset":[1,0,0]})");
    OperatorProperties offset = fixture.runner.lastOperator()->properties;
    offset.get<DuplicateProperties>()->offset = {0.0f, 0.0f, 2.0f};
    {
        OperatorContext context = fixture.context();
        fixture.runner.adjustLast(context, offset);
    }
    CHECK_MESSAGE((entityCount(fixture.scene) == 2 && fixture.selection.size() == 1 &&
                   position(fixture.scene, *fixture.selection.active()) == glm::vec3(0.0f, 4.0f, 2.0f)),
                  "adjusting a duplicate should copy the original selection with the new offset");

    fixture.invoke("object.select_all");
    CHECK_MESSAGE(fixture.runner.lastOperator() != nullptr, "operators without undo should keep the last operation");
    SceneEditing(fixture.scene, fixture.assets, fixture.document, fixture.history, fixture.diagnostics).undo();
    CHECK_MESSAGE(fixture.runner.lastOperator() == nullptr,
                  "undoing should end the chance to adjust the last operation");
    {
        OperatorContext context = fixture.context();
        CHECK_MESSAGE(fixture.runner.adjustLast(context, offset).status == OperatorStatus::Cancelled,
                      "adjusting without a last operation should do nothing");
    }

    // Operators registered by other modules use the same undo recording.
    const auto rename = [](OperatorContext& context, OperatorProperties&) {
        for (const Entity entity : context.editing.scene().registry().view<NameComponent>()) {
            context.editing.scene().registry().get<NameComponent>(entity).name = "Game object";
        }
        return OperatorResult::finished();
    };
    CHECK_MESSAGE(
        fixture.registry
            .add({.id = "game.rename_all", .label = "Rename All", .flags = OperatorFlags::Undo, .execute = rename})
            .success,
        "a game operator should register");
    const auto names = [&fixture] {
        std::vector<std::string> result;
        for (const Entity entity : fixture.scene.registry().view<NameComponent>()) {
            result.push_back(fixture.scene.registry().get<NameComponent>(entity).name);
        }
        std::ranges::sort(result);
        return result;
    };
    const std::vector<std::string> before = names();
    fixture.invoke("game.rename_all");
    CHECK_MESSAGE((!fixture.history.canRedo() && names() != before), "a game Undo operator should change the scene");
    SceneEditing(fixture.scene, fixture.assets, fixture.document, fixture.history, fixture.diagnostics).undo();
    CHECK_MESSAGE(names() == before, "one undo should revert a game Undo operator");
}

TEST_CASE("Operator: key chords") {
    for (const ImGuiKeyChord chord :
         {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, ImGuiKey_KeypadDecimal | 0, ImGuiMod_Ctrl | ImGuiKey_KeypadAdd,
          ImGuiMod_Shift | ImGuiKey_Tab, ImGuiKey_F5 | 0, ImGuiMod_Alt | ImGuiKey_A}) {
        const std::optional<ImGuiKeyChord> parsed = parseKeyChord(formatKeyChord(chord));
        CHECK_MESSAGE((parsed && *parsed == chord), "key chords should survive formatting and parsing");
    }
    CHECK_MESSAGE(formatKeyChord(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z) == "Ctrl+Shift+Z",
                  "chords should read as Ctrl+Shift+Z");
    CHECK_MESSAGE(formatKeyChord(ImGuiKey_Keypad1) == "Numpad 1", "keypad keys should read as Numpad keys");
    CHECK_MESSAGE((!parseKeyChord("Ctrl+Nothing") && !parseKeyChord("")), "unknown chords should not parse");
}

TEST_CASE("Operator: default keymap") {
    Keymap keymap(defaultEditorKeymap());
    CHECK_MESSAGE(keymap.defaultProblems().empty(), "the default keymap should have no conflicts");
    OperatorRegistry registry;
    registerObjectOperators(registry);
    registerPlacementOperators(registry);
    registerSelectOperators(registry);
    for (const KeymapBinding& binding : keymap.bindings()) {
        if (binding.operatorId.starts_with("object.") && binding.operatorId != "object.duplicate_move" &&
            binding.operatorId != "object.delete_menu" && binding.operatorId != "object.add_menu" &&
            binding.operatorId != "object.rename_popup") {
            CHECK_MESSAGE(registry.find(binding.operatorId) != nullptr,
                          "object bindings should name registered operators");
        }
    }
    CHECK_MESSAGE(keymap.shortcutLabel("edit.redo") == "Ctrl+Shift+Z", "menus should show the primary redo chord");
    CHECK_MESSAGE(std::ranges::any_of(keymap.bindings(),
                                      [](const KeymapBinding& binding) {
                                          return binding.operatorId == "edit.redo" &&
                                                 binding.chord == (ImGuiMod_Ctrl | ImGuiKey_Y);
                                      }),
                  "Ctrl+Y should stay a redo alias");
    CHECK_MESSAGE(keymap.shortcutLabel("object.select_all", R"({"action": "deselect"})") == "Alt+A",
                  "shortcut labels should match binding properties");
    CHECK_MESSAGE(keymap.shortcutLabel("object.duplicate") == "Shift+D", "region shortcuts should label menu items");
    CHECK_MESSAGE((keymap.shortcutLabel("object.hide") == "H" && keymap.shortcutLabel("object.unhide") == "Alt+H" &&
                   keymap.shortcutLabel("object.isolate") == "Shift+H"),
                  "hide, reveal, and hide unselected should use Blender's keys");
    CHECK_MESSAGE(keymap.shortcutLabel("build.run") == "Ctrl+B", "Ctrl B should build from anywhere");
    for (const ImGuiKeyChord chord :
         {ImGuiKeyChord(ImGuiKey_MouseLeft), ImGuiMod_Shift | ImGuiKey_MouseLeft, ImGuiMod_Ctrl | ImGuiKey_MouseLeft}) {
        const KeymapBinding* drag = keymap.find(KeymapRegion::Viewport, chord, KeyTrigger::Drag);
        CHECK_MESSAGE((drag != nullptr && drag->operatorId == "view3d.select_box"),
                      "dragging with the left button should draw a selection box");
    }

    // A second A within the double-click time deselects.
    const KeymapBinding* first = keymap.resolve(KeymapRegion::Viewport, ImGuiKey_A, 10.0, 0.3);
    const KeymapBinding* second = keymap.resolve(KeymapRegion::Viewport, ImGuiKey_A, 10.2, 0.3);
    const KeymapBinding* third = keymap.resolve(KeymapRegion::Viewport, ImGuiKey_A, 11.0, 0.3);
    CHECK_MESSAGE((first != nullptr && first->properties == R"({"action":"select"})"), "A should select all");
    CHECK_MESSAGE((second != nullptr && second->properties == R"({"action":"deselect"})"),
                  "double-tapping A should deselect");
    CHECK_MESSAGE((third != nullptr && third->properties == R"({"action":"select"})"),
                  "a slow second A should select again");
    CHECK_MESSAGE(keymap.resolve(KeymapRegion::Outliner, ImGuiKey_G, 12.0, 0.3) == nullptr,
                  "G should only grab in the viewport");
}

TEST_CASE("Operator: keymap conflicts") {
    Keymap keymap(defaultEditorKeymap());
    const KeymapResult conflict =
        keymap.add({.operatorId = "play.start", .region = KeymapRegion::Global, .chord = ImGuiMod_Ctrl | ImGuiKey_S});
    CHECK_MESSAGE((!conflict && conflict.error.find("file.save") != std::string::npos),
                  "a chord already bound in the region should be refused, naming its operator");
    CHECK_MESSAGE(!keymap.add({.operatorId = "play.start", .region = KeymapRegion::Viewport, .chord = ImGuiKey_F5}),
                  "a region chord should conflict with the same Global chord");
    CHECK_MESSAGE(
        keymap.add({.operatorId = "play.start", .region = KeymapRegion::Outliner, .chord = ImGuiKey_G}).success,
        "the same chord may mean different things in different regions");
    CHECK_MESSAGE(!keymap.add({.operatorId = "play.start", .region = KeymapRegion::Global, .chord = ImGuiMod_Ctrl}),
                  "a binding needs a key, not only modifiers");

    const auto saveIndex = static_cast<size_t>(
        std::ranges::find_if(keymap.bindings(),
                             [](const KeymapBinding& binding) { return binding.operatorId == "file.save"; }) -
        keymap.bindings().begin());
    CHECK_MESSAGE(!keymap.rebind(saveIndex, ImGuiMod_Ctrl | ImGuiKey_N),
                  "rebinding onto a used chord should be refused");
    CHECK_MESSAGE(keymap.rebind(saveIndex, ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_S).success,
                  "rebinding onto a free chord should work");
    CHECK_MESSAGE(keymap.shortcutLabel("file.save") == "Ctrl+Alt+S", "menus should show the new chord");
    CHECK_MESSAGE((keymap.resetOperator("file.save").success && keymap.shortcutLabel("file.save") == "Ctrl+S"),
                  "resetting an operator should restore its default chord");
}

TEST_CASE("Operator: keymap file") {
    const testsupport::TempDirectory scratch("operator-tests");
    const std::filesystem::path directory = scratch.path();
    std::error_code error;
    const std::filesystem::path path = directory / "keymap.json";

    Keymap keymap(defaultEditorKeymap());
    CHECK_MESSAGE(keymap.serializeOverrides().empty(), "an unchanged keymap should have nothing to save");
    const auto saveIndex = static_cast<size_t>(
        std::ranges::find_if(keymap.bindings(),
                             [](const KeymapBinding& binding) { return binding.operatorId == "file.save"; }) -
        keymap.bindings().begin());
    keymap.rebind(saveIndex, ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_S);
    keymap.add({.operatorId = "game.jump", .region = KeymapRegion::Viewport, .chord = ImGuiKey_J});
    keymap.add({.operatorId = "object.select_all",
                .region = KeymapRegion::Outliner,
                .chord = ImGuiMod_Ctrl | ImGuiKey_A,
                .properties = R"({"action":"select"})"});
    CHECK_MESSAGE((keymap.save(path).success && std::filesystem::exists(path)), "a changed keymap should save");

    Keymap loaded(defaultEditorKeymap());
    const KeymapResult result = loaded.load(path);
    CHECK_MESSAGE((result.success && result.warnings.empty()), "a saved keymap should load cleanly");
    std::vector<KeymapBinding> expected = keymap.bindings();
    std::vector<KeymapBinding> actual = loaded.bindings();
    const auto order = [](const KeymapBinding& left, const KeymapBinding& right) {
        return std::tie(left.operatorId, left.region, left.chord, left.trigger, left.properties) <
               std::tie(right.operatorId, right.region, right.chord, right.trigger, right.properties);
    };
    std::ranges::sort(expected, order);
    std::ranges::sort(actual, order);
    CHECK_MESSAGE(expected == actual, "keymap.json should round-trip the user's bindings");
    CHECK_MESSAGE(loaded.serializeOverrides().find("file.new") == std::string::npos,
                  "keymap.json should store only differences from the defaults");

    loaded.resetToDefaults();
    CHECK_MESSAGE((loaded.save(path).success && !std::filesystem::exists(path)),
                  "saving the default keymap should remove the user file");

    { std::ofstream(path) << "{ not json"; }
    Keymap broken(defaultEditorKeymap());
    CHECK_MESSAGE((!broken.load(path) && broken.bindings() == broken.defaults()),
                  "an unreadable keymap should fall back to the defaults with an error");
    {
        std::ofstream(path) << R"({"version":1,"added":[{"operator":"play.start","region":"global","chord":"Ctrl+S"},
                                   {"operator":"play.start","chord":"Hyper+Q"}]})";
    }
    const KeymapResult skipped = broken.load(path);
    CHECK_MESSAGE((skipped.success && skipped.warnings.size() == 2 && broken.shortcutLabel("file.save") == "Ctrl+S"),
                  "conflicting or invalid saved bindings should be skipped with warnings");
}

TEST_CASE("Operator: fuzzy search") {
    CHECK_MESSAGE(fuzzyMatchScore("dup", "Duplicate Objects").has_value(), "a prefix should match");
    CHECK_MESSAGE(fuzzyMatchScore("dob", "Duplicate Objects").has_value(), "word initials should match");
    CHECK_MESSAGE(!fuzzyMatchScore("xyz", "Duplicate Objects"), "unrelated text should not match");
    CHECK_MESSAGE(*fuzzyMatchScore("dup", "Duplicate") > *fuzzyMatchScore("dup", "Add Empty Up"),
                  "contiguous word-start matches should rank higher");

    EditingFixture fixture;
    Keymap keymap(defaultEditorKeymap());
    const OperatorContext context = fixture.context();
    const std::vector<OperatorSearchEntry> withoutSelection = searchOperators(fixture.registry, keymap, context, "");
    CHECK_MESSAGE(
        std::ranges::none_of(withoutSelection,
                             [](const OperatorSearchEntry& entry) { return entry.op->id == "object.delete"; }),
        "search should hide operators whose poll fails");
    const std::vector<OperatorSearchEntry> clear = searchOperators(fixture.registry, keymap, context, "clear");
    CHECK_MESSAGE(clear.empty(), "clear transform operators need a selection");
    fixture.invoke("object.add_empty");
    const OperatorContext selected = fixture.context();
    const std::vector<OperatorSearchEntry> rename = searchOperators(fixture.registry, keymap, selected, "rename");
    CHECK_MESSAGE(
        std::ranges::none_of(rename, [](const OperatorSearchEntry& entry) { return entry.op->id == "object.rename"; }),
        "search should hide Internal operators that need caller-supplied properties");
    CHECK_MESSAGE(fixture.invoke("object.rename", R"({"name":""})").status == OperatorStatus::Cancelled,
                  "renaming to an empty name should be refused");
    const std::vector<OperatorSearchEntry> add = searchOperators(fixture.registry, keymap, context, "add emp");
    CHECK_MESSAGE((!add.empty() && add.front().op->id == "object.add_empty"), "the best match should come first");
}

// Runs ImGui frames without a renderer.
class HeadlessImGui {
  public:
    HeadlessImGui() {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = {1280.0f, 720.0f};
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }

    ~HeadlessImGui() {
        ImGui::DestroyContext();
    }

    HeadlessImGui(const HeadlessImGui&) = delete;
    HeadlessImGui& operator=(const HeadlessImGui&) = delete;

    void frame(const std::function<void()>& draw) {
        ImGui::NewFrame();
        draw();
        ImGui::Render();
    }

    // Presses and releases key (with modifiers) over two frames.
    void press(ImGuiKeyChord chord, const std::function<void()>& draw) {
        ImGuiIO& io = ImGui::GetIO();
        const auto setModifiers = [&io, chord](bool down) {
            io.AddKeyEvent(ImGuiMod_Ctrl, down && (chord & ImGuiMod_Ctrl) != 0);
            io.AddKeyEvent(ImGuiMod_Shift, down && (chord & ImGuiMod_Shift) != 0);
            io.AddKeyEvent(ImGuiMod_Alt, down && (chord & ImGuiMod_Alt) != 0);
        };
        const ImGuiKey key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
        setModifiers(true);
        io.AddKeyEvent(key, true);
        frame(draw);
        io.AddKeyEvent(key, false);
        setModifiers(false);
        frame(draw);
    }
};

TEST_CASE("Operator: operator search popup") {
    EditingFixture fixture;
    fixture.invoke("object.add_empty");
    Keymap keymap(defaultEditorKeymap());
    OperatorSearch search;
    std::vector<std::string> chosen;
    HeadlessImGui imgui;
    const auto draw = [&] {
        const OperatorContext context = fixture.context();
        if (std::optional<std::string> id = search.draw(fixture.registry, keymap, context)) {
            chosen.push_back(std::move(*id));
        }
    };
    search.open();
    for (int frame = 0; frame < 3; ++frame) {
        imgui.frame(draw);
    }
    CHECK_MESSAGE((search.isOpen() && ImGui::GetIO().WantTextInput), "F3 search should open with its query focused");
    ImGui::GetIO().AddInputCharactersUTF8("dupl");
    imgui.frame(draw);
    imgui.press(ImGuiKey_Enter, draw);
    CHECK_MESSAGE((chosen.size() == 1 && chosen.front() == "object.duplicate"), "Enter should run the best match");
    CHECK_MESSAGE(!search.isOpen(), "running an operator should close the search");

    // Down moves the highlight to the next match.
    search.open();
    for (int frame = 0; frame < 3; ++frame) {
        imgui.frame(draw);
    }
    ImGui::GetIO().AddInputCharactersUTF8("clear");
    imgui.frame(draw);
    imgui.press(ImGuiKey_DownArrow, draw);
    imgui.press(ImGuiKey_Enter, draw);
    CHECK_MESSAGE((chosen.size() == 2 && chosen.back() == "object.rotation_clear"),
                  "Down then Enter should run the second match");

    search.open();
    for (int frame = 0; frame < 3; ++frame) {
        imgui.frame(draw);
    }
    imgui.press(ImGuiKey_Escape, draw);
    imgui.frame(draw);
    CHECK_MESSAGE((!search.isOpen() && chosen.size() == 2), "Escape should close the search without running anything");
}

TEST_CASE("Operator: keymap editor window") {
    OperatorRegistry registry;
    registerObjectOperators(registry);
    Keymap keymap(defaultEditorKeymap());
    KeymapEditor editor;
    HeadlessImGui imgui;
    bool open = true;
    bool changed = false;
    const auto draw = [&] { changed |= editor.draw(open, keymap, registry); };
    imgui.frame(draw);
    CHECK_MESSAGE((!changed && !editor.capturing()), "drawing the keymap editor should not change the keymap");

    const auto deleteIndex = static_cast<size_t>(
        std::ranges::find_if(keymap.bindings(),
                             [](const KeymapBinding& binding) { return binding.operatorId == "object.delete"; }) -
        keymap.bindings().begin());
    const KeymapRegion region = keymap.bindings()[deleteIndex].region;
    editor.captureRebind(deleteIndex);
    CHECK_MESSAGE(editor.capturing(), "clicking a shortcut should capture the next key");
    imgui.press(ImGuiMod_Ctrl | ImGuiKey_K, draw);
    CHECK_MESSAGE((changed && !editor.capturing() &&
                   keymap.bindings()[deleteIndex].chord == (ImGuiMod_Ctrl | ImGuiKey_K) &&
                   keymap.bindings()[deleteIndex].region == region),
                  "the captured chord should replace the binding");

    changed = false;
    editor.captureRebind(deleteIndex);
    imgui.press(ImGuiMod_Alt | ImGuiKey_G, draw);
    CHECK_MESSAGE(
        (!changed && !editor.message().empty() && keymap.bindings()[deleteIndex].chord == (ImGuiMod_Ctrl | ImGuiKey_K)),
        "a conflicting chord should be refused with a warning");

    editor.captureRebind(deleteIndex);
    imgui.press(ImGuiKey_Escape, draw);
    CHECK_MESSAGE((!changed && !editor.capturing()), "Escape should cancel capturing");

    editor.captureNewBinding("object.add_empty", KeymapRegion::Viewport);
    imgui.press(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_E, draw);
    CHECK_MESSAGE((changed && keymap.shortcutLabel("object.add_empty") == "Ctrl+Shift+E"),
                  "capturing a new shortcut should add a binding");
}

size_t bindingIndex(const Keymap& keymap, std::string_view operatorId, ImGuiKeyChord chord) {
    const auto found = std::ranges::find_if(keymap.bindings(), [&](const KeymapBinding& binding) {
        return binding.operatorId == operatorId && binding.chord == chord;
    });
    return static_cast<size_t>(found - keymap.bindings().begin());
}

TEST_CASE("Operator: mouse chords and triggers") {
    for (const ImGuiKeyChord chord : {ImGuiMod_Ctrl | ImGuiKey_MouseRight, ImGuiKey_MouseLeft | 0,
                                      ImGuiMod_Shift | ImGuiMod_Alt | ImGuiKey_MouseMiddle, ImGuiKey_MouseX2 | 0}) {
        const std::optional<ImGuiKeyChord> parsed = parseKeyChord(formatKeyChord(chord));
        CHECK_MESSAGE((parsed && *parsed == chord && isMouseChord(chord)), "mouse chords should survive formatting");
    }
    CHECK_MESSAGE(formatKeyChord(ImGuiMod_Ctrl | ImGuiKey_MouseRight) == "Ctrl+RMB",
                  "mouse chords should read as Ctrl+RMB");
    CHECK_MESSAGE((!isMouseChord(ImGuiKey_X) && parseKeyChord("X1") == (ImGuiKey_MouseX1 | 0)),
                  "X1 should name the mouse button, not a key");
    for (const KeyTrigger trigger : allKeyTriggers) {
        CHECK_MESSAGE(parseKeyTrigger(keyTriggerName(trigger)) == trigger, "trigger names should round-trip");
    }

    Keymap keymap(defaultEditorKeymap());
    CHECK_MESSAGE(
        (keymap.find(KeymapRegion::Viewport, ImGuiMod_Ctrl | ImGuiKey_MouseRight, KeyTrigger::Drag) != nullptr &&
         keymap.find(KeymapRegion::Viewport, ImGuiMod_Alt | ImGuiKey_MouseLeft, KeyTrigger::Click) != nullptr &&
         keymap.find(KeymapRegion::Viewport, ImGuiMod_Shift | ImGuiKey_MouseRight, KeyTrigger::Click) != nullptr),
        "the lasso, select cycle, and 3D cursor gestures should be default bindings");
    CHECK_MESSAGE((keymap.bindsChord(KeymapRegion::Viewport, ImGuiMod_Ctrl | ImGuiKey_MouseRight) &&
                   !keymap.bindsChord(KeymapRegion::Viewport, ImGuiKey_MouseRight | 0) &&
                   !keymap.bindsChord(KeymapRegion::Viewport, ImGuiKey_MouseMiddle | 0)),
                  "plain RMB and MMB should stay free for camera navigation");
    CHECK_MESSAGE((std::ranges::none_of(keymap.chords(KeymapRegion::Viewport), isMouseChord) &&
                   !keymap.mouseChords(KeymapRegion::Viewport).empty()),
                  "keyboard polling should not see mouse chords");

    // Conflicts consider the trigger: Ctrl+RMB may click and drag.
    const KeymapResult dragConflict = keymap.add({.operatorId = "game.paint",
                                                  .region = KeymapRegion::Viewport,
                                                  .chord = ImGuiMod_Ctrl | ImGuiKey_MouseRight,
                                                  .trigger = KeyTrigger::Drag});
    CHECK_MESSAGE((!dragConflict && dragConflict.error.find("view3d.select_lasso") != std::string::npos),
                  "a second Ctrl+RMB drag should conflict with the lasso");
    CHECK_MESSAGE(keymap
                      .add({.operatorId = "game.paint",
                            .region = KeymapRegion::Viewport,
                            .chord = ImGuiMod_Ctrl | ImGuiKey_MouseRight,
                            .trigger = KeyTrigger::Click})
                      .success,
                  "a Ctrl+RMB click may sit beside the Ctrl+RMB drag");
    CHECK_MESSAGE(
        !keymap.add({.operatorId = "game.paint", .region = KeymapRegion::Global, .chord = ImGuiKey_MouseX1 | 0}),
        "mouse chords should only bind in the viewport");
    const size_t boxSelect = bindingIndex(keymap, "view3d.select_box", ImGuiKey_B);
    CHECK_MESSAGE((keymap.setTrigger(boxSelect, KeyTrigger::Hold).success &&
                   keymap.find(KeymapRegion::Viewport, ImGuiKey_B, KeyTrigger::Hold) != nullptr &&
                   keymap.resolve(KeymapRegion::Viewport, ImGuiKey_B, 1.0, 0.3) == nullptr),
                  "a Hold binding should not run on the press");

    // keymap.json round-trips Hold and Release triggers and mouse chords.
    keymap.add({.operatorId = "game.charge",
                .region = KeymapRegion::Viewport,
                .chord = ImGuiKey_C,
                .trigger = KeyTrigger::Release});
    keymap.rebind(bindingIndex(keymap, "view3d.select_lasso", ImGuiMod_Ctrl | ImGuiKey_MouseRight),
                  ImGuiMod_Alt | ImGuiKey_MouseX1);
    Keymap loaded(defaultEditorKeymap());
    const KeymapResult result = loaded.loadOverrides(keymap.serializeOverrides());
    std::vector<KeymapBinding> expected = keymap.bindings();
    std::vector<KeymapBinding> actual = loaded.bindings();
    const auto order = [](const KeymapBinding& left, const KeymapBinding& right) {
        return std::tie(left.operatorId, left.region, left.chord, left.trigger, left.properties) <
               std::tie(right.operatorId, right.region, right.chord, right.trigger, right.properties);
    };
    std::ranges::sort(expected, order);
    std::ranges::sort(actual, order);
    CHECK_MESSAGE((result.success && result.warnings.empty() && expected == actual),
                  "keymap.json should round-trip Hold, Release, Click triggers and mouse chords");
    CHECK_MESSAGE((keymap.serializeOverrides().find(R"("trigger": "release")") != std::string::npos &&
                   keymap.serializeOverrides().find("Alt+X1") != std::string::npos),
                  "keymap.json should name triggers and mouse buttons");
    Keymap broken(defaultEditorKeymap());
    const KeymapResult skipped = broken.loadOverrides(
        R"({"version":1,"added":[{"operator":"a","region":"viewport","chord":"Q","trigger":"wiggle"},
                                {"operator":"b","region":"outliner","chord":"LMB"}]})");
    CHECK_MESSAGE((skipped.success && skipped.warnings.size() == 2),
                  "unknown triggers and mouse chords outside the viewport should be skipped with warnings");
}

TEST_CASE("Operator: keymap gestures") {
    Keymap keymap(defaultEditorKeymap());
    keymap.add({.operatorId = "test.hold",
                .region = KeymapRegion::Viewport,
                .chord = ImGuiKey_J,
                .trigger = KeyTrigger::Hold});
    keymap.add({.operatorId = "test.release",
                .region = KeymapRegion::Viewport,
                .chord = ImGuiKey_J,
                .trigger = KeyTrigger::Release});
    constexpr float threshold = 6.0f;

    // A key held past the hold time runs Hold once, then Release.
    KeymapGesture held(KeymapRegion::Viewport, ImGuiKey_J, 1.0, {100.0f, 100.0f});
    CHECK_MESSAGE(
        held.advance(keymap, {.down = true, .timeSeconds = 1.1, .mouse = {100.0f, 100.0f}}, threshold).empty(),
        "nothing should run before the hold time");
    const std::vector<KeymapInvocation> hold =
        held.advance(keymap, {.down = true, .timeSeconds = 1.1 + keyHoldSeconds, .mouse = {100.0f, 100.0f}}, threshold);
    CHECK_MESSAGE((hold.size() == 1 && hold.front().operatorId == "test.hold"), "holding the key should run Hold");
    CHECK_MESSAGE(
        held.advance(keymap, {.down = true, .timeSeconds = 3.0, .mouse = {100.0f, 100.0f}}, threshold).empty(),
        "Hold should run once");
    const std::vector<KeymapInvocation> release =
        held.advance(keymap, {.down = false, .timeSeconds = 3.1, .mouse = {100.0f, 100.0f}}, threshold);
    CHECK_MESSAGE((release.size() == 1 && release.front().operatorId == "test.release" && held.finished()),
                  "letting go should run Release and finish");

    // A click runs on release without movement; a drag replaces it.
    const ImGuiKeyChord altLeft = ImGuiMod_Alt | ImGuiKey_MouseLeft;
    KeymapGesture click(KeymapRegion::Viewport, altLeft, 0.0, {10.0f, 10.0f});
    click.advance(keymap, {.down = true, .timeSeconds = 0.05, .mouse = {12.0f, 11.0f}}, threshold);
    const std::vector<KeymapInvocation> clicked =
        click.advance(keymap, {.down = false, .timeSeconds = 0.1, .mouse = {12.0f, 11.0f}}, threshold);
    CHECK_MESSAGE((clicked.size() == 1 && clicked.front().operatorId == "view3d.select_cycle"),
                  "Alt+LMB released in place should run the click binding");
    const ImGuiKeyChord ctrlRight = ImGuiMod_Ctrl | ImGuiKey_MouseRight;
    KeymapGesture drag(KeymapRegion::Viewport, ctrlRight, 0.0, {10.0f, 10.0f});
    const std::vector<KeymapInvocation> dragged =
        drag.advance(keymap, {.down = true, .timeSeconds = 0.05, .mouse = {30.0f, 10.0f}}, threshold);
    CHECK_MESSAGE((dragged.size() == 1 && dragged.front().operatorId == "view3d.select_lasso" &&
                   dragged.front().properties == R"({"mode":"set","right_drag":true})"),
                  "moving past the threshold should run the drag binding with its properties");
    CHECK_MESSAGE(drag.advance(keymap, {.down = false, .timeSeconds = 0.2, .mouse = {30.0f, 10.0f}}, threshold).empty(),
                  "a drag should not also click");
}

// Space opens operator search from anywhere, but not in a text field and not
// while a game view with input focus owns the keyboard.
TEST_CASE("Operator: space opens operator search") {
    Keymap keymap(defaultEditorKeymap());
    CHECK_MESSAGE((keymap.resolve(KeymapRegion::Global, ImGuiKey_Space, 10.0, 0.3) != nullptr &&
                   keymap.resolve(KeymapRegion::Global, ImGuiKey_Space, 10.0, 0.3)->operatorId == "wm.search_menu"),
                  "Space should open operator search by default");
    CHECK_MESSAGE(keymap.resolve(KeymapRegion::Global, ImGuiKey_F3, 10.0, 0.3) != nullptr,
                  "F3 should keep opening operator search");
    CHECK_MESSAGE((chordReachesGame(ImGuiKey_Space) && chordReachesGame(ImGuiMod_Shift | ImGuiKey_W) &&
                   !chordReachesGame(ImGuiKey_F5) && !chordReachesGame(ImGuiMod_Ctrl | ImGuiKey_S) &&
                   !chordReachesGame(ImGuiKey_Escape)),
                  "the game owns plain keys, while function keys, Escape, and Ctrl and Alt chords stay the editor's");

    KeymapDispatcher dispatcher;
    HeadlessImGui imgui;
    std::vector<KeymapInvocation> ran;
    KeyboardOwner owner = KeyboardOwner::Editor;
    const auto draw = [&] {
        ImGui::Begin("Keys");
        if (const std::optional<KeymapInvocation> pressed = dispatcher.pressKeys(keymap, KeymapRegion::Global, owner)) {
            ran.push_back(*pressed);
        }
        ImGui::End();
    };
    imgui.frame(draw);
    imgui.press(ImGuiKey_Space, draw);
    CHECK_MESSAGE((ran.size() == 1 && ran.front().operatorId == "wm.search_menu"),
                  "pressing Space should invoke operator search");
    ran.clear();
    owner = KeyboardOwner::Game;
    imgui.press(ImGuiKey_Space, draw);
    CHECK_MESSAGE(ran.empty(), "Space should reach the game, not operator search, while a game view owns the keyboard");
    imgui.press(ImGuiKey_F3, draw);
    CHECK_MESSAGE((ran.size() == 1 && ran.front().operatorId == "wm.search_menu"),
                  "F3 should still open operator search while the game owns the keyboard");
}

TEST_CASE("Operator: keymap dispatcher") {
    Keymap keymap(defaultEditorKeymap());
    keymap.add({.operatorId = "test.release",
                .region = KeymapRegion::Viewport,
                .chord = ImGuiKey_J,
                .trigger = KeyTrigger::Release});
    KeymapDispatcher dispatcher;
    HeadlessImGui imgui;
    std::vector<KeymapInvocation> ran;
    const auto run = [&](const KeymapInvocation& invocation) { ran.push_back(invocation); };
    // Viewport keys and Shift+RMB press, follow, and finish through ImGui.
    const auto draw = [&] {
        for (const KeymapInvocation& invocation : dispatcher.update(keymap)) {
            run(invocation);
        }
        if (const std::optional<KeymapInvocation> pressed = dispatcher.pressKeys(keymap, KeymapRegion::Viewport)) {
            run(*pressed);
        }
        dispatcher.dispatchMouse(keymap, KeymapRegion::Viewport, allMouseButtons, run);
    };
    imgui.frame(draw);
    imgui.press(ImGuiKey_J, draw);
    CHECK_MESSAGE((ran.size() == 1 && ran.front().operatorId == "test.release" && !dispatcher.gesture()),
                  "letting go of a key should run its Release binding");

    ran.clear();
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(200.0f, 200.0f);
    io.AddKeyEvent(ImGuiMod_Shift, true);
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    uint32_t claimed = 0;
    imgui.frame([&] {
        dispatcher.update(keymap);
        claimed = dispatcher.dispatchMouse(keymap, KeymapRegion::Viewport, allMouseButtons, run);
    });
    CHECK_MESSAGE((claimed == (1u << ImGuiMouseButton_Right) && ran.empty() && dispatcher.gesture()),
                  "a Shift+RMB press should be claimed and wait for its click");
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
    io.AddKeyEvent(ImGuiMod_Shift, false);
    imgui.frame(draw);
    CHECK_MESSAGE((ran.size() == 1 && ran.front().operatorId == "view3d.cursor3d_place"),
                  "releasing Shift+RMB in place should place the 3D cursor");

    ran.clear();
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    imgui.frame([&] { claimed = dispatcher.dispatchMouse(keymap, KeymapRegion::Viewport, allMouseButtons, run); });
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
    imgui.frame(draw);
    CHECK_MESSAGE((claimed == 0 && ran.empty()), "a plain RMB press should be left to camera navigation");
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    imgui.frame([&] { claimed = dispatcher.dispatchMouse(keymap, KeymapRegion::Viewport, 0, run); });
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    imgui.frame(draw);
    CHECK_MESSAGE((claimed == 0 && ran.empty()), "presses outside the allowed buttons should be ignored");

    // A gesture whose operator is not registered leaves the press alone.
    dispatcher.setOperatorFilter([](std::string_view operatorId) { return operatorId != "view3d.cursor3d_place"; });
    io.AddKeyEvent(ImGuiMod_Shift, true);
    imgui.frame(draw);
    CHECK_MESSAGE((!dispatcher.mouseButtonBound(keymap, KeymapRegion::Viewport, ImGuiMouseButton_Right) &&
                   dispatcher.mouseButtonBound(keymap, KeymapRegion::Viewport, ImGuiMouseButton_Left)),
                  "an unregistered gesture should not bind its button");
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    imgui.frame([&] { claimed = dispatcher.dispatchMouse(keymap, KeymapRegion::Viewport, allMouseButtons, run); });
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
    imgui.frame(draw);
    io.AddKeyEvent(ImGuiMod_Shift, false);
    imgui.frame(draw);
    CHECK_MESSAGE((claimed == 0 && ran.empty()), "an unregistered gesture should neither claim nor run");
}

TEST_CASE("Operator: keymap editor mouse binding") {
    OperatorRegistry registry;
    registerObjectOperators(registry);
    Keymap keymap(defaultEditorKeymap());
    KeymapEditor editor;
    HeadlessImGui imgui;
    bool open = true;
    bool changed = false;
    const auto draw = [&] { changed |= editor.draw(open, keymap, registry); };
    imgui.frame(draw);

    const size_t lasso = bindingIndex(keymap, "view3d.select_lasso", ImGuiMod_Ctrl | ImGuiKey_MouseRight);
    editor.captureRebind(lasso);
    imgui.frame(draw);
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Alt, true);
    io.AddMouseButtonEvent(ImGuiMouseButton_Middle, true);
    imgui.frame(draw);
    io.AddMouseButtonEvent(ImGuiMouseButton_Middle, false);
    io.AddKeyEvent(ImGuiMod_Alt, false);
    imgui.frame(draw);
    CHECK_MESSAGE((changed && !editor.capturing() &&
                   keymap.bindings()[lasso].chord == (ImGuiMod_Alt | ImGuiKey_MouseMiddle) &&
                   keymap.bindings()[lasso].trigger == KeyTrigger::Drag),
                  "capturing a viewport shortcut should take a mouse chord and keep its trigger");

    changed = false;
    const size_t save = bindingIndex(keymap, "file.save", ImGuiMod_Ctrl | ImGuiKey_S);
    editor.captureRebind(save);
    imgui.frame(draw);
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    imgui.frame(draw);
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
    imgui.frame(draw);
    CHECK_MESSAGE((!changed && editor.capturing()), "a global shortcut should not capture a mouse button");
    imgui.press(ImGuiKey_Escape, draw);
}

TEST_CASE("Operator: last operator panel") {
    EditingFixture fixture;
    fixture.invoke("object.add_cube");
    fixture.diagnostics.acknowledgeErrors();
    fixture.invoke("object.add_empty", R"({"location":[0,1,0]})");
    LastOperatorPanel panel;
    HeadlessImGui imgui;
    bool edited = false;
    imgui.frame([&] { edited = panel.draw(fixture.runner.lastOperator(), {0.0f, 720.0f}).has_value(); });
    CHECK_MESSAGE(!edited, "drawing the adjust panel without input should not re-run the operator");
    imgui.frame([&] { edited = panel.draw(nullptr, {0.0f, 720.0f}).has_value(); });
    CHECK_MESSAGE(!edited, "the adjust panel should draw nothing without a last operation");
}
} // namespace

TEST_SUITE_END();
