// Runs the whole editor session headlessly: EditorApplication composed by an
// ApplicationSession, as zenith_editor composes it, with ImGui frames driven
// by HeadlessImGui instead of a window and renderer. Shortcuts are pressed
// through ImGui's input queue, so they travel the real keymap and operator
// path; checks observe the scene, the fixed-update gates, control requests,
// and the files the session persists.

#include "HeadlessImGui.hpp"

#include "editor/application/EditorApplication.hpp"
#include "engine/application/Application.hpp"
#include "engine/scene/Scene.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

size_t entityCount(const Scene& scene) {
    return scene.registry().view<const SceneEntityIdComponent>().size();
}

bool hasEntityNamed(const Scene& scene, std::string_view name) {
    const auto view = scene.registry().view<const NameComponent>();
    for (const Entity entity : view) {
        if (view.get<const NameComponent>(entity).name == name) {
            return true;
        }
    }
    return false;
}

ProjectConfig testProject(const fs::path& root) {
    ProjectConfig project;
    project.displayName = "Editor session test";
    project.assetRoot = ZENITH_ASSET_DIR;
    project.sceneRoot = root / "scenes";
    fs::create_directories(project.sceneRoot);
    return project;
}

// Declaration order is teardown order in reverse: the session (and the
// editor's unload, which still uses ImGui) ends before the ImGui context.
class EditorHarness {
  public:
    explicit EditorHarness(const fs::path& root)
        : m_application(root / "settings", ZENITH_EDITOR_FONT, testProject(root))
        , m_session(m_application, scene, assets, m_physics, m_audio) {}

    SceneResult start() {
        return m_session.start();
    }

    // One engine frame: update between ImGui frames, then the UI.
    ApplicationFrameResult frame() {
        m_session.update(InputState{}, 1.0f / 60.0f);
        buildFontAtlas();
        m_imgui.frame([this] { m_session.drawUi(); });
        return m_session.finishFrame();
    }

    // Presses and releases chord over two frames, then lets the session act on
    // any request the shortcut made.
    ApplicationFrameResult press(ImGuiKeyChord chord) {
        m_session.update(InputState{}, 1.0f / 60.0f);
        buildFontAtlas();
        m_imgui.press(chord, [this] { m_session.drawUi(); });
        return m_session.finishFrame();
    }

    ApplicationSession& session() {
        return m_session;
    }

    void shutdown() {
        m_session.shutdown();
    }

  private:
    // The editor loads its font into the atlas; a renderer backend rebuilds
    // the atlas before the next frame, and the harness stands in for it.
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

  private:
    PhysicsWorld m_physics;
    AudioSystem m_audio;
    EditorApplication m_application;
    ApplicationSession m_session;
};

void testStartupAndPersistence(const fs::path& root) {
    {
        EditorHarness editor(root);
        expect(static_cast<bool>(editor.start()), "startup: the editor session starts without a GPU");
        expect(entityCount(editor.scene) > 0, "startup: the default scene is populated");
        bool running = true;
        for (int frame = 0; frame < 5; ++frame) {
            const ApplicationFrameResult result = editor.frame();
            running = running && result.success && result.keepRunning;
        }
        expect(running, "startup: idle frames keep the editor running");
        expect(!editor.session().fixedUpdatesEnabled(), "startup: Edit mode runs no fixed updates");
        editor.shutdown();
    }
    const fs::path settings = root / "settings";
    expect(fs::is_regular_file(settings / "workspaces.json"), "persistence: unloading saves the workspaces");
    expect(fs::is_regular_file(settings / "settings.ini"), "persistence: unloading saves the editor settings");

    EditorHarness reopened(root);
    expect(static_cast<bool>(reopened.start()) && reopened.frame().keepRunning,
           "persistence: a second session starts from the saved workspaces and settings");
}

void testPlayModeRestoresTheScene(const fs::path& root) {
    EditorHarness editor(root);
    if (!editor.start()) {
        expect(false, "play: the editor session starts");
        return;
    }
    editor.frame();
    const size_t authored = entityCount(editor.scene);

    editor.press(ImGuiKey_F5);
    ApplicationSession& session = editor.session();
    expect(session.fixedUpdatesEnabled() && session.beginFixedUpdate(), "play: F5 enters Play mode");
    editor.scene.createEntity("Spawned during play");

    editor.press(ImGuiKey_F6);
    expect(!session.beginFixedUpdate(), "play: F6 pauses fixed updates");
    editor.press(ImGuiKey_F7);
    expect(session.beginFixedUpdate() && !session.beginFixedUpdate(), "play: F7 allows exactly one fixed update");
    editor.press(ImGuiKey_F6);
    expect(session.beginFixedUpdate(), "play: F6 again resumes");

    editor.press(ImGuiMod_Shift | ImGuiKey_F5);
    expect(!session.fixedUpdatesEnabled(), "play: Shift F5 returns to Edit mode");
    expect(entityCount(editor.scene) == authored && !hasEntityNamed(editor.scene, "Spawned during play"),
           "play: stopping restores the authored scene");
}

void testNewDocumentAndQuit(const fs::path& root) {
    EditorHarness editor(root);
    if (!editor.start()) {
        expect(false, "file: the editor session starts");
        return;
    }
    editor.frame();
    const size_t defaultCount = entityCount(editor.scene);
    // Not recorded in history, so the document stays clean and New does not
    // stop for the unsaved-changes dialog.
    editor.scene.createEntity("Scratch");
    const ApplicationFrameResult created = editor.press(ImGuiMod_Ctrl | ImGuiKey_N);
    expect(created.keepRunning && !hasEntityNamed(editor.scene, "Scratch") && entityCount(editor.scene) == defaultCount,
           "file: Ctrl N replaces a clean document with the default scene");

    const ApplicationFrameResult quit = editor.press(ImGuiMod_Ctrl | ImGuiKey_Q);
    expect(quit.success && !quit.keepRunning, "file: Ctrl Q on a clean document quits");
}

void testHostQuitRequest(const fs::path& root) {
    EditorHarness editor(root);
    if (!editor.start()) {
        expect(false, "quit: the editor session starts");
        return;
    }
    editor.frame();
    editor.press(ImGuiKey_F5);
    // Closing the window while playing stops Play mode and then quits.
    editor.session().requestQuit();
    expect(!editor.session().fixedUpdatesEnabled(), "quit: a host quit request leaves Play mode");
    const ApplicationFrameResult result = editor.frame();
    expect(result.success && !result.keepRunning, "quit: a host quit request on a clean document quits");
}
} // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "zenith_editor_session_tests";
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root);

    testStartupAndPersistence(root / "persistence");
    testPlayModeRestoresTheScene(root / "play");
    testNewDocumentAndQuit(root / "file");
    testHostQuitRequest(root / "quit");

    fs::remove_all(root, error);
    if (failures != 0) {
        std::cerr << failures << " editor session test(s) failed\n";
        return 1;
    }
    std::cout << "Editor session tests passed\n";
    return 0;
}
