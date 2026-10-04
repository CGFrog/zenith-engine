// Runs the whole editor session headlessly: EditorApplication composed by an
// ApplicationSession, as zenith_editor composes it, with ImGui frames driven
// by HeadlessImGui instead of a window and renderer. Shortcuts are pressed
// through ImGui's input queue, so they travel the real keymap and operator
// path; checks observe the scene, the fixed-update gates, control requests,
// and the files the session persists.

#include "HeadlessImGui.hpp"

#include "editor/application/EditorApplication.hpp"
#include "editor/project/ProjectTemplate.hpp"
#include "editor/project/RecentProjects.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "engine/application/Application.hpp"
#include "engine/scene/Scene.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
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
    explicit EditorHarness(const fs::path& root, std::optional<EditorProjectHubOptions> projectHub = std::nullopt)
        : m_application(root / "settings", ZENITH_EDITOR_FONT, testProject(root), std::move(projectHub))
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

    const EditorApplication& application() const {
        return m_application;
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

// The id of an item in the Project Hub's window, reached through path's ids.
ImGuiID projectHubItem(std::initializer_list<const char*> path) {
    const ImGuiWindow* window = HeadlessImGuiContext::findWindow("Project Hub");
    if (window == nullptr) {
        return 0;
    }
    ImGuiID id = window->ID;
    for (const char* part : path) {
        id = ImHashStr(part, 0, id);
    }
    return id;
}

// Starts in the hub, opens a template project from the recent list, and goes
// back to the hub through File > Project Hub (run from operator search).
void testProjectHubSwitchesProjects(const fs::path& root) {
    AssetManager generatorAssets;
    const ProjectTemplateResult generated =
        generateProjectFromTemplate({.displayName = "Hub Game",
                                     .parentDirectory = root / "projects",
                                     .templateDirectory = fs::path(ZENITH_TEMPLATE_DIR) / cppGameTemplateName,
                                     .engineRoot = root / "engine",
                                     .engineAssetRoot = ZENITH_ASSET_DIR},
                                    generatorAssets);
    if (!generated) {
        expect(false, "hub: the template project generates: " + generated.error);
        return;
    }
    {
        RecentProjects recent(root / "settings" / "recent-projects.json");
        recent.recordOpened(generated.projectFile, "Hub Game", std::chrono::system_clock::time_point{});
        expect(static_cast<bool>(recent.save()), "hub: the recent list is prepared");
    }

    EditorHarness editor(
        root, EditorProjectHubOptions{.hub = {.defaultProjectLocation = root / "projects"}, .startInHub = true});
    if (!editor.start()) {
        expect(false, "hub: the editor starts in the Project Hub");
        return;
    }
    const ApplicationFrameResult hubFrame = editor.frame();
    expect(hubFrame.keepRunning && entityCount(editor.scene) == 0, "hub: the hub shows no scene");

    const std::string projectKey = fs::absolute(generated.projectFile).lexically_normal().string();
    HeadlessImGuiContext::activateNextFrame(
        projectHubItem({"##hub_columns", "##recent_projects", projectKey.c_str(), "Open"}));
    const ApplicationFrameResult opened = editor.frame();
    expect(opened.success && opened.keepRunning, "hub: opening a recent project keeps the editor running");
    expect(hasEntityNamed(editor.scene, "Ground") && hasEntityNamed(editor.scene, "Camera"),
           "hub: the chosen project's session opens its startup scene");
    expect(editor.application().project().displayName == "Hub Game", "hub: the chosen project is current");
    const std::span<const RecentProject> recent = editor.application().recentProjects().entries();
    expect(!recent.empty() && recent.front().lastOpened > std::chrono::system_clock::time_point{},
           "hub: opening a project records when it was opened");
    editor.frame();

    // F3, then the operator's name and Enter.
    editor.press(ImGuiKey_F3);
    HeadlessImGuiContext::typeNextFrame("Project Hub");
    editor.frame();
    const ApplicationFrameResult left = editor.press(ImGuiKey_Enter);
    expect(left.success && left.keepRunning && entityCount(editor.scene) == 0,
           "hub: File > Project Hub closes the clean project and shows the hub");
    expect(fs::is_regular_file(root / "settings" / "workspaces.json"),
           "hub: leaving the project unloads its session, which saves the workspaces");
    expect(editor.frame().keepRunning, "hub: the hub keeps running after a project closed");
}

void testProjectHubUsesThePreferredLook(const fs::path& root) {
    fs::create_directories(root / "settings");
    {
        std::ofstream settings(root / "settings" / "settings.ini");
        settings << "theme.accent=violet\nui.scale=2\n";
    }
    EditorHarness editor(
        root, EditorProjectHubOptions{.hub = {.defaultProjectLocation = root / "projects"}, .startInHub = true});
    if (!editor.start()) {
        expect(false, "hub look: the editor starts in the Project Hub");
        return;
    }
    const ApplicationFrameResult frame = editor.frame();
    expect(frame.success && frame.keepRunning, "hub look: the hub draws");
    expect(editorArtPixel() == 2.0f, "hub look: the hub uses the UI scale from settings.ini");
    expect(editorAccent() == themeAccentInfo(ThemeAccent::Violet).color.withAlpha(1.0f),
           "hub look: the hub uses the accent from settings.ini");
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
    testProjectHubSwitchesProjects(root / "hub");
    testProjectHubUsesThePreferredLook(root / "hub_look");

    fs::remove_all(root, error);
    if (failures != 0) {
        std::cerr << failures << " editor session test(s) failed\n";
        return 1;
    }
    std::cout << "Editor session tests passed\n";
    return 0;
}
