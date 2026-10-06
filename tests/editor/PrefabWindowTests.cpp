// Prefab windows: prefabs open in floating windows of their own while the
// level stays open. The first half drives PrefabWindowSet headless (each
// window's own document, history, and selection, saving, and refreshing other
// documents). The second half runs the whole editor session headlessly with
// ui.prefab_windows on, for what only the session decides: which document
// keys act on, refreshing the level on save, the unsaved-changes prompt,
// Play with a window open, module reloads waiting for the windows, and
// recovering an autosaved window.

#include "HeadlessImGui.hpp"

#include "editor/application/EditorApplication.hpp"
#include "editor/application/EditorDiagnostics.hpp"
#include "editor/application/PrefabWindowSet.hpp"
#include "editor/application/SceneEditing.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/document/PrefabAuthoring.hpp"
#include "editor/document/PrefabWriteCommand.hpp"
#include "editor/operators/Keymap.hpp"
#include "engine/application/Application.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/Prefab.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

fs::path freshDirectory(const fs::path& root, const std::string& name) {
    const fs::path directory = root / name;
    std::error_code error;
    fs::remove_all(directory, error);
    fs::create_directories(directory);
    return directory;
}

std::string readText(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

Entity findNamed(const Scene& scene, std::string_view name) {
    const auto view = scene.registry().view<const NameComponent>();
    for (const Entity entity : view) {
        if (view.get<const NameComponent>(entity).name == name) {
            return entity;
        }
    }
    return entt::null;
}

// The instance roots of reference in scene, nested ones included.
std::vector<Entity> instancesOf(const Scene& scene, const std::string& reference) {
    std::vector<Entity> roots;
    for (const auto [entity, instance] : scene.registry().view<const PrefabInstanceComponent>().each()) {
        if (instance.prefab == reference) {
            roots.push_back(entity);
        }
    }
    return roots;
}

size_t litLamps(const Scene& scene) {
    size_t lit = 0;
    for (const Entity root : instancesOf(scene, "lamp.prefab")) {
        lit += scene.registry().all_of<PointLightComponent>(root) ? 1 : 0;
    }
    return lit;
}

// ---------------------------------------------------------------------------
// PrefabWindowSet, headless.

// Authors lamp.prefab (a Lamp with a Shade child, no light) and shelf.prefab
// (a Shelf holding one lamp instance) under root.
void authorPrefabs(const fs::path& root) {
    AssetManager assets;
    const SceneDocumentOptions options{.assetRoot = root};
    PrefabAuthoring authoring(assets, options);
    Scene staging;
    const Entity lamp = staging.createEntity("Lamp");
    staging.setParent(staging.createEntity("Shade"), lamp);
    expect(authoring.create(staging, lamp, "lamp.prefab").success, "fixture: the lamp prefab should be created");
    Scene shelfStaging;
    const Entity shelf = shelfStaging.createEntity("Shelf");
    Entity nested = entt::null;
    expect(authoring.instantiate(shelfStaging, "lamp.prefab", shelf, nested).success &&
               authoring.create(shelfStaging, shelf, "shelf.prefab").success,
           "fixture: the shelf prefab should be created");
}

struct Fixture {
    fs::path root;
    AssetManager assets;
    EditorDocument level;
    EditorDiagnostics diagnostics;
    PrefabWriteLedger ledger;
    PrefabWindowSet windows;

    explicit Fixture(fs::path assetRoot)
        : root(std::move(assetRoot))
        , level(root) {
        authorPrefabs(root);
    }

    PrefabWriteHooks hooks() {
        return {.fileWritten = [this](const fs::path& file, const std::optional<std::string>& bytes, const Scene*) {
            ledger.record(file, bytes);
        }};
    }

    PrefabWindowDocument* open(const std::string& reference) {
        PrefabWindowDocument* window = nullptr;
        const PrefabResult result = windows.open(reference, level, assets, window);
        return result ? window : nullptr;
    }

    SceneEditing editing(PrefabWindowDocument& window) {
        return {window.scene, assets, window.document, window.history, diagnostics};
    }

    // Gives the prefab root of window a point light, as one undo step.
    void light(PrefabWindowDocument& window) {
        editing(window).perform("Add Light", {}, [&window] {
            window.scene.registry().emplace<PointLightComponent>(window.scene.findEntity(window.target.root),
                                                                 glm::vec3(1.0f), 10.0f, 4.0f);
            return SceneResult{.success = true};
        });
    }
};

void testWindowsHoldTheirOwnDocuments(const fs::path& base) {
    Fixture fixture(freshDirectory(base, "documents"));
    PrefabWindowDocument* lamp = fixture.open("lamp.prefab");
    if (lamp == nullptr) {
        expect(false, "documents: the lamp should open in a window");
        return;
    }
    expect(lamp->target.displayName == "Lamp" && lamp->target.reference == "lamp.prefab" &&
               findNamed(lamp->scene, "Shade") != entt::null,
           "documents: the window holds the prefab");
    expect(lamp->selection.active() == lamp->target.root && !lamp->document.dirty() && !lamp->history.canUndo(),
           "documents: the window starts clean with its root selected");
    expect(!lamp->document.path(), "documents: the window's document is not the level's file");
    expect(fixture.open("./lamp.prefab") == lamp && fixture.windows.size() == 1,
           "documents: opening an open prefab returns its window");
    expect(fixture.open("missing.prefab") == nullptr && fixture.windows.size() == 1,
           "documents: a prefab that cannot open adds no window");

    PrefabWindowDocument* shelf = fixture.open("shelf.prefab");
    expect(shelf != nullptr && shelf != lamp && shelf->id != lamp->id && fixture.windows.size() == 2,
           "documents: a second prefab opens a second window");
    fixture.light(*lamp);
    expect(lamp->document.dirty() && lamp->history.size() == 1 && shelf != nullptr && shelf->history.size() == 0,
           "documents: an edit is the edited window's step only");
    expect(fixture.windows.owner(lamp->document) == lamp && fixture.windows.owner(fixture.level) == nullptr,
           "documents: a document's window is found by its document");

    const uint64_t lampId = lamp->id;
    expect(fixture.windows.close(lampId) && fixture.windows.find(lampId) == nullptr && fixture.windows.size() == 1,
           "documents: closing drops the window");
    expect(!fixture.windows.close(lampId), "documents: a closed window cannot close again");
    PrefabWindowDocument* reopened = fixture.open("lamp.prefab");
    expect(reopened != nullptr && reopened->id != lampId && !reopened->document.dirty(),
           "documents: reopening starts a new, clean window with a new id");
    fixture.windows.closeAll();
    expect(fixture.windows.empty(), "documents: closeAll drops every window");
}

void testSaveRefreshesOtherWindows(const fs::path& base) {
    Fixture fixture(freshDirectory(base, "save"));
    PrefabWindowDocument* shelf = fixture.open("shelf.prefab");
    PrefabWindowDocument* lamp = fixture.open("lamp.prefab");
    if (shelf == nullptr || lamp == nullptr) {
        expect(false, "save: both prefabs should open");
        return;
    }
    const fs::path file = fixture.root / "lamp.prefab";
    const std::string original = readText(file);
    fixture.light(*lamp);
    SavedPrefabDocument saved;
    expect(fixture.windows.save(*lamp, fixture.assets, fixture.diagnostics, fixture.hooks(), saved).success,
           "save: the lamp saves");
    expect(readText(file) != original && saved.before == original && saved.after == readText(file),
           "save: the file is written and the save reports before and after");
    expect(!lamp->document.dirty() && fixture.ledger.recorded(lamp->target.file) != nullptr,
           "save: the window becomes clean and the write is in the ledger");

    PrefabDocument previous;
    expect(deserializePrefab(original, previous).success, "save: the previous content parses");
    size_t refreshed = 0;
    const PrefabResult result = fixture.windows.refreshInstances(
        fixture.assets, fixture.diagnostics, "lamp.prefab", &previous, "Refresh Lamp instances", nullptr, refreshed);
    expect(result.success && refreshed == 1 && litLamps(shelf->scene) == 1,
           "save: the shelf's nested lamp is refreshed");
    expect(shelf->history.size() == 1 && shelf->history.canUndo(), "save: the refresh is one step of the shelf's");
    expect(lamp->history.size() == 1, "save: the window editing the lamp itself is not refreshed");
    fixture.editing(*shelf).undo();
    expect(litLamps(shelf->scene) == 0, "save: undo in the shelf puts its lamp back");
}

void testWindowsFollowMovedFiles(const fs::path& base) {
    Fixture fixture(freshDirectory(base, "moved"));
    PrefabWindowDocument* lamp = fixture.open("lamp.prefab");
    if (lamp == nullptr) {
        expect(false, "moved: the lamp should open");
        return;
    }
    fs::create_directories(fixture.root / "props");
    const fs::path from = fs::absolute(fixture.root / "lamp.prefab").lexically_normal();
    const fs::path to = fs::absolute(fixture.root / "props" / "lamp.prefab").lexically_normal();
    fs::rename(from, to);
    const ProjectFileMove move{.from = from, .to = to};
    fixture.windows.filesMoved(std::span(&move, 1), fixture.root, fixture.assets, fixture.diagnostics);
    expect(lamp->target.reference == "props/lamp.prefab" && fixture.windows.find("props/lamp.prefab") == lamp,
           "moved: the window follows its prefab's file");
    fixture.light(*lamp);
    SavedPrefabDocument saved;
    expect(fixture.windows.save(*lamp, fixture.assets, fixture.diagnostics, fixture.hooks(), saved).success &&
               !fs::exists(from) && readText(to).find("point_light") != std::string::npos,
           "moved: saving writes the file where it now lives");
}

// ---------------------------------------------------------------------------
// The whole session, headless.

// Declaration order is teardown order in reverse: the session ends before the
// ImGui context.
class EditorHarness {
  public:
    EditorHarness(const fs::path& root, ProjectConfig project)
        : m_application(root / "settings", ZENITH_EDITOR_FONT, std::move(project), std::nullopt, {})
        , m_session(m_application, scene, assets, m_physics, m_audio, m_rendering, m_gameUi, logger) {}

    SceneResult start() {
        return m_session.start();
    }

    ApplicationFrameResult frame(const InputState& input = {}) {
        m_session.update(input, 1.0f / 60.0f);
        buildFontAtlas();
        m_imgui.frame([this] { m_session.drawUi(); });
        return m_session.finishFrame();
    }

    ApplicationFrameResult press(ImGuiKeyChord chord) {
        m_session.update(InputState{}, 1.0f / 60.0f);
        buildFontAtlas();
        m_imgui.press(chord, [this] { m_session.drawUi(); });
        return m_session.finishFrame();
    }

    // Holds chord down over one frame; release lets it go.
    ApplicationFrameResult hold(ImGuiKeyChord chord) {
        m_session.update(InputState{}, 1.0f / 60.0f);
        buildFontAtlas();
        m_imgui.hold(chord, [this] { m_session.drawUi(); });
        return m_session.finishFrame();
    }

    ApplicationFrameResult release(ImGuiKeyChord chord) {
        m_session.update(InputState{}, 1.0f / 60.0f);
        buildFontAtlas();
        m_imgui.release(chord, [this] { m_session.drawUi(); });
        return m_session.finishFrame();
    }

    // Presses a button of the window (or popup) whose name is name, if it is
    // open, and runs the frame that handles it.
    bool clickButton(const char* name, const char* button) {
        const ImGuiWindow* window = HeadlessImGuiContext::findWindow(name);
        if (window == nullptr || !window->Active) {
            return false;
        }
        HeadlessImGuiContext::activateNextFrame(HeadlessImGuiContext::itemId(*window, button));
        frame();
        return true;
    }

    // Focuses the level's first viewport area, as a click there does, and
    // runs a frame: global keys then act on the level again.
    void focusLevel() {
        if (ImGuiWindow* window = HeadlessImGuiContext::findWindow("Viewport##viewport.")) {
            ImGui::FocusWindow(window);
        }
        frame();
    }

    // Moves the window whose name contains name over the middle of the one
    // named over, brings it to the front as a click would, and runs a frame.
    void placeOver(const char* name, const char* over) {
        ImGuiWindow* window = HeadlessImGuiContext::findWindow(name);
        const ImGuiWindow* below = HeadlessImGuiContext::findWindow(over);
        if (window == nullptr || below == nullptr) {
            return;
        }
        const ImVec2 centre = below->Rect().GetCenter();
        ImGui::SetWindowPos(window, {centre.x - 150.0f, centre.y - 100.0f});
        ImGui::SetWindowSize(window, {300.0f, 200.0f});
        ImGui::FocusWindow(window);
        frame();
    }

    // A point (screen pixels) from the centre of the window whose name
    // contains name towards its lower right where that window is on top,
    // inside the window named under too; nullopt when there is none.
    std::optional<glm::vec2> pointOnTop(const char* name, const char* under) const {
        const ImGuiWindow* window = HeadlessImGuiContext::findWindow(name);
        const ImGuiWindow* below = HeadlessImGuiContext::findWindow(under);
        if (window == nullptr || below == nullptr || !window->Active) {
            return std::nullopt;
        }
        const ImVec2 centre = window->Rect().GetCenter();
        for (float y = centre.y; y < window->Pos.y + window->Size.y - 8.0f; y += 4.0f) {
            for (float x = centre.x; x < window->Pos.x + window->Size.x - 8.0f; x += 4.0f) {
                ImGuiWindow* hovered = nullptr;
                ImGui::FindHoveredWindowEx({x, y}, false, &hovered, nullptr);
                if (hovered != nullptr && hovered->RootWindowDockTree == window->RootWindowDockTree &&
                    below->Rect().Contains(ImVec2(x, y))) {
                    return glm::vec2(x, y);
                }
            }
        }
        return std::nullopt;
    }

    bool windowOpen(const char* name) const {
        const ImGuiWindow* window = HeadlessImGuiContext::findWindow(name);
        return window != nullptr && window->Active;
    }

    ApplicationSession& session() {
        return m_session;
    }

    RenderRuntime& rendering() {
        return m_rendering;
    }

    void shutdown() {
        m_session.shutdown();
    }

  private:
    static void buildFontAtlas() {
        ImFontAtlas& fonts = *ImGui::GetIO().Fonts;
        if (!fonts.IsBuilt()) {
            unsigned char* pixels = nullptr;
            int width = 0;
            int height = 0;
            fonts.GetTexDataAsRGBA32(&pixels, &width, &height);
        }
    }

    HeadlessImGui m_imgui;

  public:
    Scene scene;
    AssetManager assets;
    Logger logger{LoggerConfig{.echoToStderr = false}};

  private:
    PhysicsWorld m_physics;
    AudioSystem m_audio;
    RenderRuntime m_rendering;
    GameUiContext m_gameUi;
    EditorApplication m_application;
    ApplicationSession m_session;
};

constexpr ImGuiKeyChord editLampKey = ImGuiKey_F9;
constexpr ImGuiKeyChord closeKey = ImGuiKey_F10;
// Adds a point light to the active object: an undoable edit of the prefab
// root, whose instances then gain a light.
constexpr ImGuiKeyChord addLightKey = ImGuiKey_F11;
constexpr ImGuiKeyChord reloadKey = ImGuiKey_F8;
constexpr ImGuiKeyChord editShelfKey = ImGuiMod_Shift | ImGuiKey_F9;
// Selects every object named Lamp in the focused document.
constexpr ImGuiKeyChord selectLampsKey = ImGuiMod_Shift | ImGuiKey_F10;
constexpr ImGuiKeyChord applyKey = ImGuiMod_Shift | ImGuiKey_F11;
// A Click gesture (runs on release) adding a point light to the active
// object, like addLightKey.
constexpr ImGuiKeyChord clickLightKey = ImGuiKey_F12;
// The lamp window's host, by its ImGui id (its title shows the dirty mark).
constexpr const char* lampWindow = "###prefab.1";

// A project under root with lamp.prefab (without a light), shelf.prefab
// (holding one lamp), and a startup level holding two lamp instances and a
// "Level marker", prefab windows on, and keys bound to the prefab operators.
ProjectConfig prefabProject(const fs::path& root, bool withModule = false, std::string extraSettings = {}) {
    const fs::path assetRoot = root / "assets";
    fs::create_directories(assetRoot);
    fs::create_directories(root / "scenes");
    fs::create_directories(root / "settings");
    AssetManager assets;
    const SceneDocumentOptions options{.assetRoot = assetRoot};
    PrefabAuthoring authoring(assets, options);
    Scene staging;
    const Entity lamp = staging.createEntity("Lamp");
    staging.setParent(staging.createEntity("Shade"), lamp);
    expect(authoring.create(staging, lamp, "lamp.prefab").success, "session: the lamp prefab should be created");
    Scene shelfStaging;
    const Entity shelf = shelfStaging.createEntity("Shelf");
    Entity nested = entt::null;
    expect(authoring.instantiate(shelfStaging, "lamp.prefab", shelf, nested).success &&
               authoring.create(shelfStaging, shelf, "shelf.prefab").success,
           "session: the shelf prefab should be created");
    Scene level;
    level.createEntity("Level marker");
    Entity instance = entt::null;
    expect(authoring.instantiate(level, "lamp.prefab", entt::null, instance).success &&
               authoring.instantiate(level, "lamp.prefab", entt::null, instance).success,
           "session: the level's lamps should instantiate");
    const fs::path levelPath = root / "scenes" / "level.zenith-scene";
    expect(SceneDocument::save(levelPath, level, assets, options).success, "session: the level should save");

    const auto binding = [](ImGuiKeyChord chord, const char* operatorId, nlohmann::json properties) {
        return nlohmann::json{{"operator", operatorId},
                              {"region", keymapRegionName(KeymapRegion::Global)},
                              {"chord", formatKeyChord(chord)},
                              {"properties", std::move(properties)}};
    };
    std::ofstream(root / "settings" / "keymap.json") << nlohmann::json{
        {"version", 1},
        {"removed", nlohmann::json::array()},
        {"added",
         {binding(editLampKey, "prefab.edit", {{"asset", "lamp.prefab"}}),
          binding(editShelfKey, "prefab.edit", {{"asset", "shelf.prefab"}}),
          binding(selectLampsKey, "outliner.select_matching", {{"filter", "Lamp"}}),
          binding(applyKey, "prefab.apply", nlohmann::json::object()),
          [&] {
              nlohmann::json click = binding(clickLightKey, "object.component_add", {{"component", "point_light"}});
              click["trigger"] = "click";
              return click;
          }(),
          binding(closeKey, "prefab.exit_edit", nlohmann::json::object()),
          binding(addLightKey, "object.component_add", {{"component", "point_light"}}),
          binding(reloadKey, "module.reload",
                  nlohmann::json::object())}}}.dump();
    std::ofstream(root / "settings" / "settings.ini") << "ui.prefab_windows=true\n" << extraSettings;

    ProjectConfig project;
    project.displayName = "Prefab window test";
    project.assetRoot = assetRoot;
    project.sceneRoot = root / "scenes";
    project.startupScene = levelPath;
    if (withModule) {
        project.gameModule = ZENITH_EDITOR_TEST_MODULE_V1;
    }
    return project;
}

// The render scene a created view draws, or empty when no view draws one.
RenderSceneId boundScene(const RenderRuntime& rendering) {
    for (uint32_t index = 1; index < RenderRuntime::maximumViews; ++index) {
        for (uint32_t generation = 1; generation < 64; ++generation) {
            const RenderViewId view{.index = index, .generation = generation};
            if (rendering.viewAlive(view)) {
                if (const RenderSceneId scene = rendering.viewScene(view)) {
                    return scene;
                }
            }
        }
    }
    return {};
}

bool inLevel(const Scene& scene) {
    return findNamed(scene, "Level marker") != entt::null;
}

void testWindowEditsThePrefabBesideTheLevel(const fs::path& root) {
    EditorHarness editor(root, prefabProject(root));
    if (!editor.start()) {
        expect(false, "window: the editor session starts");
        return;
    }
    editor.frame();
    const fs::path file = root / "assets" / "lamp.prefab";
    const std::string original = readText(file);

    editor.press(editLampKey);
    editor.frame();
    expect(inLevel(editor.scene) && editor.windowOpen(lampWindow),
           "window: the prefab opens in a window and the level stays open");
    expect(editor.windowOpen("Hierarchy##prefab.1.hierarchy") && editor.windowOpen("Inspector##prefab.1.inspector") &&
               editor.windowOpen("Viewport##prefab.1.viewport"),
           "window: the window shows its own Hierarchy, Inspector, and viewport");
    editor.frame();
    const RenderSceneId prefabScene = boundScene(editor.rendering());
    expect(prefabScene && editor.rendering().sceneAlive(prefabScene),
           "window: the window's viewport draws a render scene of its own");
    editor.press(addLightKey);
    expect(litLamps(editor.scene) == 0 && readText(file) == original,
           "window: an edit in the focused window changes neither the level nor the file");
    editor.press(ImGuiMod_Ctrl | ImGuiKey_S);
    expect(readText(file) != original && litLamps(editor.scene) == 2,
           "window: Ctrl S saves the prefab and refreshes the level's lamps");
    editor.press(ImGuiMod_Ctrl | ImGuiKey_Z);
    expect(litLamps(editor.scene) == 2 && readText(file) != original,
           "window: Ctrl Z undoes in the window, not the level");
    editor.press(editLampKey);
    editor.frame();
    expect(editor.windowOpen(lampWindow) && !editor.windowOpen("###prefab.2"),
           "window: editing an open prefab brings its window forward instead of opening another");

    // Closing a window with changes asks first.
    editor.press(closeKey);
    expect(editor.windowOpen("Unsaved Prefab"), "close: a window with changes asks before closing");
    expect(editor.clickButton("Unsaved Prefab", "Cancel") && editor.windowOpen(lampWindow),
           "close: Cancel keeps the window");
    editor.press(closeKey);
    expect(editor.clickButton("Unsaved Prefab", "Discard"), "close: Discard answers the prompt");
    editor.frame();
    expect(!editor.windowOpen(lampWindow) && inLevel(editor.scene) && litLamps(editor.scene) == 2,
           "close: Discard closes the window and leaves the saved prefab in the level");
    expect(!editor.rendering().sceneAlive(prefabScene) && !boundScene(editor.rendering()),
           "close: the window's render scene and its view go with it");
}

// prefab.apply writes the prefab from one document; the other open documents
// catch up, each with a refresh step of its own, and the writer is never
// refreshed by its own write.
void testApplyReachesOtherDocuments(const fs::path& root) {
    EditorHarness editor(root, prefabProject(root));
    if (!editor.start()) {
        expect(false, "apply: the editor session starts");
        return;
    }
    editor.frame();
    const fs::path file = root / "assets" / "lamp.prefab";
    const std::string original = readText(file);
    constexpr const char* shelfWindow = "###prefab.1";
    editor.press(editShelfKey);
    editor.frame();
    expect(editor.windowOpen(shelfWindow), "apply: the shelf opens in a window");

    // From the level: the shelf's nested lamp catches up.
    editor.focusLevel();
    editor.press(selectLampsKey);
    editor.press(addLightKey);
    editor.press(applyKey);
    editor.frame();
    expect(readText(file) != original && litLamps(editor.scene) == 2,
           "apply: the level applies its lamp to the prefab");
    editor.press(ImGuiMod_Ctrl | ImGuiKey_Z);
    editor.frame();
    expect(readText(file) == original,
           "apply: the level's first undo undoes the apply, so the level was not refreshed by its own write");
    editor.press(ImGuiMod_Ctrl | ImGuiKey_Z);
    expect(litLamps(editor.scene) == 0, "apply: the level's light is undone");
    // The shelf was refreshed (and refreshed back): steps of its own, which
    // leave it with changes to ask about.
    editor.press(editShelfKey);
    editor.press(closeKey);
    expect(editor.windowOpen("Unsaved Prefab") && editor.clickButton("Unsaved Prefab", "Cancel"),
           "apply: the shelf window refreshed its lamp as steps of its own");

    // From the window: the level catches up, as a step it can undo.
    editor.press(selectLampsKey);
    editor.press(addLightKey);
    editor.press(applyKey);
    expect(readText(file) != original && litLamps(editor.scene) == 0,
           "apply: the window applies its lamp, and the level waits for the frame's catch-up");
    editor.frame();
    expect(litLamps(editor.scene) == 2, "apply: the level's lamps catch up with the window's apply");
    editor.focusLevel();
    editor.press(ImGuiMod_Ctrl | ImGuiKey_Z);
    expect(litLamps(editor.scene) == 0 && readText(file) != original,
           "apply: undo in the level undoes its refresh, not the window's apply");
}

// A gesture's later phases (here a Click, run on release) run on the document
// its press started in, and a gesture whose window closed runs nothing.
void testGesturesStayInTheirWindow(const fs::path& root) {
    EditorHarness editor(root, prefabProject(root));
    if (!editor.start()) {
        expect(false, "gesture: the editor session starts");
        return;
    }
    editor.frame();
    editor.press(editLampKey);
    editor.frame();
    editor.hold(clickLightKey);
    expect(editor.clickButton(lampWindow, "Close##prefab_window_close"), "gesture: the window's Close is pressed");
    editor.frame();
    expect(!editor.windowOpen(lampWindow), "gesture: the clean window closes mid-gesture");
    editor.release(clickLightKey);
    editor.frame();
    expect(litLamps(editor.scene) == 0, "gesture: a gesture whose window closed runs nothing on the level");

    editor.press(editLampKey);
    editor.frame();
    editor.press(clickLightKey);
    expect(litLamps(editor.scene) == 0, "gesture: the click edits the window, not the level");
    editor.press(ImGuiMod_Ctrl | ImGuiKey_S);
    expect(litLamps(editor.scene) == 2, "gesture: the window's click added the light that its save then applies");
}

// A file dropped on a prefab window's viewport floating over the level's
// viewport goes into the prefab, not the level under it.
void testDropOnAWindowOverTheLevel(const fs::path& root) {
    EditorHarness editor(root, prefabProject(root));
    if (!editor.start()) {
        expect(false, "drop: the editor session starts");
        return;
    }
    editor.frame();
    editor.press(editShelfKey);
    editor.frame();
    editor.frame();
    // Headless, the window's panels float undocked; place its viewport over
    // the level's as the floating window opens in the editor.
    editor.placeOver("Viewport##prefab.1.viewport", "Viewport##viewport.");
    editor.frame();
    const std::optional<glm::vec2> point = editor.pointOnTop("Viewport##prefab.1.viewport", "Viewport##viewport.");
    expect(point.has_value(), "drop: the window's viewport lies over the level's");
    if (!point) {
        return;
    }
    InputState input;
    input.droppedFiles.push_back({.path = root / "assets" / "lamp.prefab", .position = *point});
    editor.frame(input);
    editor.frame();
    expect(instancesOf(editor.scene, "lamp.prefab").size() == 2, "drop: the level under the window gains nothing");
    editor.press(closeKey);
    expect(editor.windowOpen("Unsaved Prefab") && editor.clickButton("Unsaved Prefab", "Cancel"),
           "drop: the shelf gained the dropped lamp, so closing it asks about the change");
}

void testPlayWithAWindowOpen(const fs::path& root) {
    EditorHarness editor(root, prefabProject(root));
    if (!editor.start()) {
        expect(false, "play: the editor session starts");
        return;
    }
    editor.frame();
    ApplicationSession& session = editor.session();
    editor.press(editLampKey);
    editor.frame();
    editor.press(ImGuiKey_F5);
    expect(session.fixedUpdatesEnabled() && editor.windowOpen(lampWindow), "play: F5 plays with a prefab window open");
    editor.press(ImGuiKey_F5);
    expect(session.fixedUpdatesEnabled(), "play: F5 while playing does not start Play again");
    editor.press(addLightKey);
    editor.press(ImGuiMod_Ctrl | ImGuiKey_S);
    expect(litLamps(editor.scene) == 0, "play: saving the prefab during Play leaves the running scene's copies");
    editor.press(ImGuiMod_Shift | ImGuiKey_F5);
    editor.frame();
    expect(!session.fixedUpdatesEnabled() && inLevel(editor.scene) && litLamps(editor.scene) == 2,
           "play: Stop restores the level with its instances refreshed by the save");
}

void testModuleReloadWaitsForWindows(const fs::path& root) {
    EditorHarness editor(root, prefabProject(root, true));
    if (!editor.start()) {
        expect(false, "reload: the editor session starts with the test module");
        return;
    }
    editor.frame();
    editor.press(editLampKey);
    editor.frame();
    editor.press(reloadKey);
    const uint64_t generation = editor.scene.contentGeneration();
    editor.frame();
    editor.frame();
    expect(editor.scene.contentGeneration() == generation, "reload: a requested reload waits while a window is open");
    editor.press(closeKey);
    editor.frame();
    expect(!editor.windowOpen(lampWindow), "reload: the clean window closes");
    editor.frame();
    expect(editor.scene.contentGeneration() != generation && inLevel(editor.scene),
           "reload: the reload runs once the windows are closed");
}

void testQuitAsksAboutWindows(const fs::path& root) {
    EditorHarness editor(root, prefabProject(root));
    if (!editor.start()) {
        expect(false, "quit: the editor session starts");
        return;
    }
    editor.frame();
    editor.press(editLampKey);
    editor.frame();
    editor.press(addLightKey);
    editor.press(ImGuiMod_Ctrl | ImGuiKey_Q);
    expect(editor.windowOpen("Unsaved Prefab"), "quit: quitting asks about a prefab window with changes");
    expect(editor.clickButton("Unsaved Prefab", "Cancel") && editor.windowOpen(lampWindow),
           "quit: Cancel keeps the editor and the window");
    editor.press(ImGuiMod_Ctrl | ImGuiKey_Q);
    const fs::path file = root / "assets" / "lamp.prefab";
    const std::string original = readText(file);
    expect(editor.clickButton("Unsaved Prefab", "Save") && readText(file) != original,
           "quit: Save writes the prefab before quitting");
}

void testRecoverAWindow(const fs::path& root) {
    ProjectConfig project = prefabProject(root, false, "autosave.interval=0.01\n");
    const fs::path recovery = root / "settings" / "recovery-prefab-windows";
    {
        EditorHarness editor(root, project);
        if (!editor.start()) {
            expect(false, "recovery: the editor session starts");
            return;
        }
        editor.frame();
        editor.press(editLampKey);
        editor.frame();
        editor.press(addLightKey);
        editor.frame();
        editor.frame();
        expect(fs::is_regular_file(recovery / "1.prefab-edit") && fs::is_regular_file(recovery / "1.prefab-edit.json"),
               "recovery: autosave writes the window's prefab and its note");
        expect(!fs::exists(root / "settings" / "recovery.zenith-scene"), "recovery: the clean level is not autosaved");
        editor.shutdown();
    }
    EditorHarness editor(root, project);
    if (!editor.start()) {
        expect(false, "recovery: the editor session starts again");
        return;
    }
    editor.frame();
    expect(editor.windowOpen("Recover Autosave"), "recovery: the next session offers the autosave");
    expect(editor.clickButton("Recover Autosave", "Recover") && inLevel(editor.scene),
           "recovery: Recover keeps the level");
    editor.frame();
    expect(editor.windowOpen(lampWindow), "recovery: the prefab window opens again");
    editor.press(ImGuiMod_Ctrl | ImGuiKey_S);
    expect(litLamps(editor.scene) == 2, "recovery: the recovered window saves and refreshes the level");
    editor.frame();
    editor.frame();
    expect(!fs::exists(recovery / "1.prefab-edit"), "recovery: a saved window leaves no autosave behind");
}
} // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "zenith_prefab_window_tests";
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root);

    testWindowsHoldTheirOwnDocuments(root);
    testSaveRefreshesOtherWindows(root);
    testWindowsFollowMovedFiles(root);
    testWindowEditsThePrefabBesideTheLevel(root / "window");
    testApplyReachesOtherDocuments(root / "apply");
    testGesturesStayInTheirWindow(root / "gesture");
    testDropOnAWindowOverTheLevel(root / "drop");
    testPlayWithAWindowOpen(root / "play");
    testModuleReloadWaitsForWindows(root / "reload");
    testQuitAsksAboutWindows(root / "quit");
    testRecoverAWindow(root / "recovery");

    fs::remove_all(root, error);
    if (failures != 0) {
        std::cerr << failures << " prefab window test(s) failed\n";
        return 1;
    }
    std::cout << "Prefab window tests passed\n";
    return 0;
}
