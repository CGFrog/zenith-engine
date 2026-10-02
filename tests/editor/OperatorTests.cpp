#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/operators/DefaultKeymap.hpp"
#include "editor/operators/Keymap.hpp"
#include "editor/operators/KeymapEditor.hpp"
#include "editor/operators/LastOperatorPanel.hpp"
#include "editor/operators/ObjectOperators.hpp"
#include "editor/operators/OperatorRunner.hpp"
#include "editor/operators/OperatorSearch.hpp"
#include "engine/assets/Assets.hpp"

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

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

// A document, history, and selection editing a real scene, as the editor
// session owns them, but without a window.
struct EditingFixture {
    AssetManager assets;
    Scene scene;
    EditorDocument document{std::filesystem::temp_directory_path()};
    CommandHistory history;
    EditorDiagnostics diagnostics;
    Selection selection;
    OperatorRegistry registry;
    OperatorRunner runner{registry, history};

    EditingFixture() {
        expect(registerObjectOperators(registry).success, "the object operators should register");
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

void testRegistry() {
    OperatorRegistry registry;
    const auto run = [](OperatorContext&, OperatorProperties&) { return OperatorResult::finished(); };
    expect(registry.add({.id = "game.jump", .label = "Jump", .execute = run}).success,
           "operators from outside the editor should register");
    expect(!registry.add({.id = "game.jump", .label = "Jump again", .execute = run}).success,
           "duplicate operator ids should be rejected");
    expect(!registry.add({.id = "", .label = "Nameless", .execute = run}).success, "operators need an id");
    expect(!registry.add({.id = "game.idle", .label = "Idle"}).success, "operators need an execute function");
    expect(registry.find("game.jump") != nullptr && registry.find("game.missing") == nullptr,
           "operators should be found by id");
    expect(registry.size() == 1, "rejected operators should not be registered");
}

void testPollAndUndo() {
    EditingFixture fixture;
    const OperatorResult nothingSelected = fixture.invoke("object.delete");
    expect(nothingSelected.status == OperatorStatus::Cancelled && !fixture.history.canUndo(),
           "an operator whose poll fails should be cancelled without changing history");
    {
        OperatorContext playing = fixture.context(EditorMode::Play);
        expect(fixture.runner.invoke("object.add_empty", playing).status == OperatorStatus::Cancelled &&
                   entityCount(fixture.scene) == 0,
               "authoring operators should not run in Play mode");
    }
    const OperatorResult unknown = fixture.invoke("object.missing");
    expect(unknown.status == OperatorStatus::Failed && fixture.diagnostics.hasUnacknowledgedError(),
           "unknown operators should fail and be reported");
    fixture.diagnostics.acknowledgeErrors();

    expect(static_cast<bool>(fixture.invoke("object.add_empty", R"({"location":[1,2,3]})")),
           "adding an empty should finish");
    expect(entityCount(fixture.scene) == 1 && fixture.selection.size() == 1 &&
               position(fixture.scene, *fixture.selection.active()) == glm::vec3(1.0f, 2.0f, 3.0f),
           "add operators should place and select the new object");
    expect(fixture.history.size() == 1 && fixture.document.dirty(),
           "an Undo operator should record exactly one undo step");

    fixture.invoke("object.select_all", R"({"action":"deselect"})");
    expect(fixture.selection.empty() && fixture.history.size() == 1,
           "selection operators should not record undo steps");
    fixture.invoke("object.select_all");
    expect(fixture.selection.size() == 1, "select all should select every object");
    fixture.invoke("object.select_all", R"({"action":"invert"})");
    expect(fixture.selection.empty(), "invert should deselect a fully selected scene");

    expect(fixture.invoke("object.select_all", R"({"action":"sideways"})").status == OperatorStatus::Failed,
           "invalid binding properties should fail");
    fixture.diagnostics.acknowledgeErrors();

    fixture.invoke("object.select_all");
    fixture.invoke("object.location_clear");
    expect(position(fixture.scene, *fixture.selection.active()) == glm::vec3(0.0f) && fixture.history.size() == 2,
           "clearing the location should be one undoable step");
    fixture.invoke("object.location_clear");
    expect(fixture.history.size() == 2, "an operator that changes nothing should record nothing");

    fixture.invoke("object.component_add", R"({"component":"point_light"})");
    const Entity empty = fixture.scene.findEntity(*fixture.selection.active());
    expect(fixture.scene.registry().all_of<PointLightComponent>(empty) && fixture.history.size() == 3,
           "adding a component by key should be undoable");
    fixture.invoke("object.component_remove", R"({"component":"point_light"})");
    expect(!fixture.scene.registry().all_of<PointLightComponent>(fixture.scene.findEntity(*fixture.selection.active())),
           "removing a component by key should remove it");
    expect(fixture.invoke("object.component_add", R"({"component":"not_a_component"})").status ==
                   OperatorStatus::Failed &&
               fixture.diagnostics.hasUnacknowledgedError(),
           "an unknown component should fail with a diagnostic");

    fixture.invoke("object.rename", R"({"name":"Renamed"})");
    expect(fixture.scene.registry().get<NameComponent>(fixture.scene.findEntity(*fixture.selection.active())).name ==
               "Renamed",
           "rename should name the active object");

    fixture.invoke("object.delete");
    expect(entityCount(fixture.scene) == 0 && fixture.selection.empty(), "delete should remove the selection");
    SceneEditing(fixture.scene, fixture.assets, fixture.document, fixture.history, fixture.diagnostics).undo();
    expect(entityCount(fixture.scene) == 1, "one undo should restore a deleted object");
}

void testAdjustLastOperation() {
    EditingFixture fixture;
    {
        OperatorContext context = fixture.context();
        const OperatorProperties first = withLocation(fixture.registry, "object.add_empty", {1.0f, 0.0f, 0.0f});
        fixture.runner.invoke("object.add_empty", context, &first);
    }
    const LastOperator* last = fixture.runner.lastOperator();
    expect(last != nullptr && last->id == "object.add_empty" && last->recordedUndoStep,
           "a Register operator should become the last operation");
    const size_t historyAfterFirst = fixture.history.size();

    const OperatorProperties adjusted = withLocation(fixture.registry, "object.add_empty", {0.0f, 4.0f, 0.0f});
    {
        OperatorContext context = fixture.context();
        expect(static_cast<bool>(fixture.runner.adjustLast(context, adjusted)), "adjusting should re-run");
    }
    expect(fixture.history.size() == historyAfterFirst && entityCount(fixture.scene) == 1,
           "adjusting should replace the operation's undo step, not add one");

    EditingFixture fresh;
    {
        OperatorContext context = fresh.context();
        fresh.runner.invoke("object.add_empty", context, &adjusted);
    }
    expect(sceneSummary(fixture.scene) == sceneSummary(fresh.scene),
           "an adjusted operation should match a fresh run with the edited properties");
    last = fixture.runner.lastOperator();
    expect(last != nullptr && last->properties.get<AddObjectProperties>()->location == glm::vec3(0.0f, 4.0f, 0.0f),
           "the last operation should show the adjusted properties");

    // Duplicate restores the selection it first saw before re-running.
    fixture.invoke("object.duplicate", R"({"offset":[1,0,0]})");
    OperatorProperties offset = fixture.runner.lastOperator()->properties;
    offset.get<DuplicateProperties>()->offset = {0.0f, 0.0f, 2.0f};
    {
        OperatorContext context = fixture.context();
        fixture.runner.adjustLast(context, offset);
    }
    expect(entityCount(fixture.scene) == 2 && fixture.selection.size() == 1 &&
               position(fixture.scene, *fixture.selection.active()) == glm::vec3(0.0f, 4.0f, 2.0f),
           "adjusting a duplicate should copy the original selection with the new offset");

    fixture.invoke("object.select_all");
    expect(fixture.runner.lastOperator() != nullptr, "operators without undo should keep the last operation");
    SceneEditing(fixture.scene, fixture.assets, fixture.document, fixture.history, fixture.diagnostics).undo();
    expect(fixture.runner.lastOperator() == nullptr, "undoing should end the chance to adjust the last operation");
    {
        OperatorContext context = fixture.context();
        expect(fixture.runner.adjustLast(context, offset).status == OperatorStatus::Cancelled,
               "adjusting without a last operation should do nothing");
    }

    // Operators registered by other modules use the same undo recording.
    const auto rename = [](OperatorContext& context, OperatorProperties&) {
        for (const Entity entity : context.editing.scene().registry().view<NameComponent>()) {
            context.editing.scene().registry().get<NameComponent>(entity).name = "Game object";
        }
        return OperatorResult::finished();
    };
    expect(fixture.registry
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
    expect(!fixture.history.canRedo() && names() != before, "a game Undo operator should change the scene");
    SceneEditing(fixture.scene, fixture.assets, fixture.document, fixture.history, fixture.diagnostics).undo();
    expect(names() == before, "one undo should revert a game Undo operator");
}

void testKeyChords() {
    for (const ImGuiKeyChord chord :
         {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, ImGuiKey_KeypadDecimal | 0, ImGuiMod_Ctrl | ImGuiKey_KeypadAdd,
          ImGuiMod_Shift | ImGuiKey_Tab, ImGuiKey_F5 | 0, ImGuiMod_Alt | ImGuiKey_A}) {
        const std::optional<ImGuiKeyChord> parsed = parseKeyChord(formatKeyChord(chord));
        expect(parsed && *parsed == chord, "key chords should survive formatting and parsing");
    }
    expect(formatKeyChord(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z) == "Ctrl+Shift+Z",
           "chords should read as Ctrl+Shift+Z");
    expect(formatKeyChord(ImGuiKey_Keypad1) == "Numpad 1", "keypad keys should read as Numpad keys");
    expect(!parseKeyChord("Ctrl+Nothing") && !parseKeyChord(""), "unknown chords should not parse");
}

void testDefaultKeymap() {
    Keymap keymap(defaultEditorKeymap());
    expect(keymap.defaultProblems().empty(), "the default keymap should have no conflicts");
    OperatorRegistry registry;
    registerObjectOperators(registry);
    for (const KeymapBinding& binding : keymap.bindings()) {
        if (binding.operatorId.starts_with("object.") && binding.operatorId != "object.duplicate_move" &&
            binding.operatorId != "object.delete_menu" && binding.operatorId != "object.add_menu" &&
            binding.operatorId != "object.rename_popup") {
            expect(registry.find(binding.operatorId) != nullptr, "object bindings should name registered operators");
        }
    }
    expect(keymap.shortcutLabel("edit.redo") == "Ctrl+Shift+Z", "menus should show the primary redo chord");
    expect(std::ranges::any_of(keymap.bindings(),
                               [](const KeymapBinding& binding) {
                                   return binding.operatorId == "edit.redo" &&
                                          binding.chord == (ImGuiMod_Ctrl | ImGuiKey_Y);
                               }),
           "Ctrl+Y should stay a redo alias");
    expect(keymap.shortcutLabel("object.select_all", R"({"action": "deselect"})") == "Alt+A",
           "shortcut labels should match binding properties");
    expect(keymap.shortcutLabel("object.duplicate") == "Shift+D", "region shortcuts should label menu items");

    // A second A within the double-click time deselects.
    const KeymapBinding* first = keymap.resolve(KeymapRegion::Viewport, ImGuiKey_A, 10.0, 0.3);
    const KeymapBinding* second = keymap.resolve(KeymapRegion::Viewport, ImGuiKey_A, 10.2, 0.3);
    const KeymapBinding* third = keymap.resolve(KeymapRegion::Viewport, ImGuiKey_A, 11.0, 0.3);
    expect(first != nullptr && first->properties == R"({"action":"select"})", "A should select all");
    expect(second != nullptr && second->properties == R"({"action":"deselect"})", "double-tapping A should deselect");
    expect(third != nullptr && third->properties == R"({"action":"select"})", "a slow second A should select again");
    expect(keymap.resolve(KeymapRegion::Outliner, ImGuiKey_G, 12.0, 0.3) == nullptr,
           "G should only grab in the viewport");
}

void testKeymapConflicts() {
    Keymap keymap(defaultEditorKeymap());
    const KeymapResult conflict =
        keymap.add({.operatorId = "play.start", .region = KeymapRegion::Global, .chord = ImGuiMod_Ctrl | ImGuiKey_S});
    expect(!conflict && conflict.error.find("file.save") != std::string::npos,
           "a chord already bound in the region should be refused, naming its operator");
    expect(!keymap.add({.operatorId = "play.start", .region = KeymapRegion::Viewport, .chord = ImGuiKey_F5}),
           "a region chord should conflict with the same Global chord");
    expect(keymap.add({.operatorId = "play.start", .region = KeymapRegion::Outliner, .chord = ImGuiKey_G}).success,
           "the same chord may mean different things in different regions");
    expect(!keymap.add({.operatorId = "play.start", .region = KeymapRegion::Global, .chord = ImGuiMod_Ctrl}),
           "a binding needs a key, not only modifiers");

    const auto saveIndex = static_cast<size_t>(
        std::ranges::find_if(keymap.bindings(),
                             [](const KeymapBinding& binding) { return binding.operatorId == "file.save"; }) -
        keymap.bindings().begin());
    expect(!keymap.rebind(saveIndex, ImGuiMod_Ctrl | ImGuiKey_N), "rebinding onto a used chord should be refused");
    expect(keymap.rebind(saveIndex, ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_S).success,
           "rebinding onto a free chord should work");
    expect(keymap.shortcutLabel("file.save") == "Ctrl+Alt+S", "menus should show the new chord");
    expect(keymap.resetOperator("file.save").success && keymap.shortcutLabel("file.save") == "Ctrl+S",
           "resetting an operator should restore its default chord");
}

void testKeymapFile() {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "zenith_operator_tests";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    const std::filesystem::path path = directory / "keymap.json";

    Keymap keymap(defaultEditorKeymap());
    expect(keymap.serializeOverrides().empty(), "an unchanged keymap should have nothing to save");
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
    expect(keymap.save(path).success && std::filesystem::exists(path), "a changed keymap should save");

    Keymap loaded(defaultEditorKeymap());
    const KeymapResult result = loaded.load(path);
    expect(result.success && result.warnings.empty(), "a saved keymap should load cleanly");
    std::vector<KeymapBinding> expected = keymap.bindings();
    std::vector<KeymapBinding> actual = loaded.bindings();
    const auto order = [](const KeymapBinding& left, const KeymapBinding& right) {
        return std::tie(left.operatorId, left.region, left.chord, left.trigger, left.properties) <
               std::tie(right.operatorId, right.region, right.chord, right.trigger, right.properties);
    };
    std::ranges::sort(expected, order);
    std::ranges::sort(actual, order);
    expect(expected == actual, "keymap.json should round-trip the user's bindings");
    expect(loaded.serializeOverrides().find("file.new") == std::string::npos,
           "keymap.json should store only differences from the defaults");

    loaded.resetToDefaults();
    expect(loaded.save(path).success && !std::filesystem::exists(path),
           "saving the default keymap should remove the user file");

    {
        std::ofstream(path) << "{ not json";
    }
    Keymap broken(defaultEditorKeymap());
    expect(!broken.load(path) && broken.bindings() == broken.defaults(),
           "an unreadable keymap should fall back to the defaults with an error");
    {
        std::ofstream(path) << R"({"version":1,"added":[{"operator":"play.start","region":"global","chord":"Ctrl+S"},
                                   {"operator":"play.start","chord":"Hyper+Q"}]})";
    }
    const KeymapResult skipped = broken.load(path);
    expect(skipped.success && skipped.warnings.size() == 2 && broken.shortcutLabel("file.save") == "Ctrl+S",
           "conflicting or invalid saved bindings should be skipped with warnings");
    std::filesystem::remove_all(directory, error);
}

void testFuzzySearch() {
    expect(fuzzyMatchScore("dup", "Duplicate Objects").has_value(), "a prefix should match");
    expect(fuzzyMatchScore("dob", "Duplicate Objects").has_value(), "word initials should match");
    expect(!fuzzyMatchScore("xyz", "Duplicate Objects"), "unrelated text should not match");
    expect(*fuzzyMatchScore("dup", "Duplicate") > *fuzzyMatchScore("dup", "Add Empty Up"),
           "contiguous word-start matches should rank higher");

    EditingFixture fixture;
    Keymap keymap(defaultEditorKeymap());
    const OperatorContext context = fixture.context();
    const std::vector<OperatorSearchEntry> withoutSelection = searchOperators(fixture.registry, keymap, context, "");
    expect(std::ranges::none_of(withoutSelection,
                                [](const OperatorSearchEntry& entry) { return entry.op->id == "object.delete"; }),
           "search should hide operators whose poll fails");
    const std::vector<OperatorSearchEntry> clear = searchOperators(fixture.registry, keymap, context, "clear");
    expect(clear.empty(), "clear transform operators need a selection");
    fixture.invoke("object.add_empty");
    const OperatorContext selected = fixture.context();
    const std::vector<OperatorSearchEntry> rename = searchOperators(fixture.registry, keymap, selected, "rename");
    expect(
        std::ranges::none_of(rename, [](const OperatorSearchEntry& entry) { return entry.op->id == "object.rename"; }),
        "search should hide Internal operators that need caller-supplied properties");
    expect(fixture.invoke("object.rename", R"({"name":""})").status == OperatorStatus::Cancelled,
           "renaming to an empty name should be refused");
    const std::vector<OperatorSearchEntry> add = searchOperators(fixture.registry, keymap, context, "add emp");
    expect(!add.empty() && add.front().op->id == "object.add_empty", "the best match should come first");
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

void testOperatorSearchPopup() {
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
    expect(search.isOpen() && ImGui::GetIO().WantTextInput, "F3 search should open with its query focused");
    ImGui::GetIO().AddInputCharactersUTF8("dupl");
    imgui.frame(draw);
    imgui.press(ImGuiKey_Enter, draw);
    expect(chosen.size() == 1 && chosen.front() == "object.duplicate", "Enter should run the best match");
    expect(!search.isOpen(), "running an operator should close the search");

    // Down moves the highlight to the next match.
    search.open();
    for (int frame = 0; frame < 3; ++frame) {
        imgui.frame(draw);
    }
    ImGui::GetIO().AddInputCharactersUTF8("clear");
    imgui.frame(draw);
    imgui.press(ImGuiKey_DownArrow, draw);
    imgui.press(ImGuiKey_Enter, draw);
    expect(chosen.size() == 2 && chosen.back() == "object.rotation_clear",
           "Down then Enter should run the second match");

    search.open();
    for (int frame = 0; frame < 3; ++frame) {
        imgui.frame(draw);
    }
    imgui.press(ImGuiKey_Escape, draw);
    imgui.frame(draw);
    expect(!search.isOpen() && chosen.size() == 2, "Escape should close the search without running anything");
}

void testKeymapEditorWindow() {
    OperatorRegistry registry;
    registerObjectOperators(registry);
    Keymap keymap(defaultEditorKeymap());
    KeymapEditor editor;
    HeadlessImGui imgui;
    bool open = true;
    bool changed = false;
    const auto draw = [&] { changed |= editor.draw(open, keymap, registry); };
    imgui.frame(draw);
    expect(!changed && !editor.capturing(), "drawing the keymap editor should not change the keymap");

    const auto deleteIndex = static_cast<size_t>(
        std::ranges::find_if(keymap.bindings(),
                             [](const KeymapBinding& binding) { return binding.operatorId == "object.delete"; }) -
        keymap.bindings().begin());
    const KeymapRegion region = keymap.bindings()[deleteIndex].region;
    editor.captureRebind(deleteIndex);
    expect(editor.capturing(), "clicking a shortcut should capture the next key");
    imgui.press(ImGuiMod_Ctrl | ImGuiKey_K, draw);
    expect(changed && !editor.capturing() && keymap.bindings()[deleteIndex].chord == (ImGuiMod_Ctrl | ImGuiKey_K) &&
               keymap.bindings()[deleteIndex].region == region,
           "the captured chord should replace the binding");

    changed = false;
    editor.captureRebind(deleteIndex);
    imgui.press(ImGuiMod_Alt | ImGuiKey_G, draw);
    expect(!changed && !editor.message().empty() &&
               keymap.bindings()[deleteIndex].chord == (ImGuiMod_Ctrl | ImGuiKey_K),
           "a conflicting chord should be refused with a warning");

    editor.captureRebind(deleteIndex);
    imgui.press(ImGuiKey_Escape, draw);
    expect(!changed && !editor.capturing(), "Escape should cancel capturing");

    editor.captureNewBinding("object.add_empty", KeymapRegion::Viewport);
    imgui.press(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_E, draw);
    expect(changed && keymap.shortcutLabel("object.add_empty") == "Ctrl+Shift+E",
           "capturing a new shortcut should add a binding");
}

void testLastOperatorPanel() {
    EditingFixture fixture;
    fixture.invoke("object.add_cube");
    fixture.diagnostics.acknowledgeErrors();
    fixture.invoke("object.add_empty", R"({"location":[0,1,0]})");
    LastOperatorPanel panel;
    HeadlessImGui imgui;
    bool edited = false;
    imgui.frame([&] { edited = panel.draw(fixture.runner.lastOperator(), {0.0f, 720.0f}).has_value(); });
    expect(!edited, "drawing the adjust panel without input should not re-run the operator");
    imgui.frame([&] { edited = panel.draw(nullptr, {0.0f, 720.0f}).has_value(); });
    expect(!edited, "the adjust panel should draw nothing without a last operation");
}
} // namespace

int main() {
    testRegistry();
    testPollAndUndo();
    testAdjustLastOperation();
    testKeyChords();
    testDefaultKeymap();
    testKeymapConflicts();
    testKeymapFile();
    testFuzzySearch();
    testOperatorSearchPopup();
    testKeymapEditorWindow();
    testLastOperatorPanel();
    if (failures != 0) {
        std::cerr << failures << " operator test(s) failed\n";
        return 1;
    }
    return 0;
}
