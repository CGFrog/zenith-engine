#include "HeadlessImGui.hpp"

#include "editor/application/EditorAreaTypes.hpp"
#include "editor/application/EditorDiagnostics.hpp"
#include "editor/areas/AreaRegistry.hpp"
#include "editor/areas/AreaWindows.hpp"
#include "editor/areas/DefaultWorkspaces.hpp"
#include "editor/areas/Workspace.hpp"
#include "editor/areas/WorkspaceFile.hpp"
#include "editor/document/CommandHistory.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/menus/EditorMenus.hpp"
#include "editor/operators/OperatorRegistry.hpp"
#include "editor/operators/OperatorRunner.hpp"
#include "editor/panels/AssetBrowserPanel.hpp"
#include "editor/panels/BuildPanel.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/panels/InfoPanel.hpp"
#include "editor/panels/PropertiesPanel.hpp"
#include "editor/project/ProjectAssets.hpp"
#include "editor/settings/EditorSettings.hpp"
#include "editor/settings/RenderSettingsController.hpp"
#include "editor/theme/ThemeChrome.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/audio/AudioSystem.hpp"
#include "engine/diagnostics/Logger.hpp"
#include "engine/input/InputManager.hpp"
#include "engine/physics/PhysicsWorld.hpp"
#include "engine/ui/GameUi.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void writeFile(const std::filesystem::path& path, std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
}

// The session workflows a panel may trigger, as no-ops.
class NoSessionActions final : public EditorSessionActions {
  public:
    OperatorResult invokeOperator(ApplicationContext&, std::string_view, std::string_view, KeymapRegion) override {
        return OperatorResult::cancelled();
    }

    bool operatorAvailable(ApplicationContext&, std::string_view) override {
        return false;
    }

    std::string operatorShortcut(std::string_view, std::string_view) const override {
        return {};
    }

    void requestOpenScene(ApplicationContext&, std::filesystem::path) override {}

    void projectFilesMoved(ApplicationContext&, std::span<const ProjectFileMove>) override {}

    void projectFilesRemoved(ApplicationContext&, std::span<const std::filesystem::path>) override {}

    void handleObjectKeys(ApplicationContext&, KeymapRegion) override {}

    bool modalTransformActive() const override {
        return false;
    }

    void updateModalTransform(ApplicationContext&, ViewportId) override {}
};

// The Viewport needs a GPU, so the tests stand in a plain panel for it.
class ViewportStandIn final : public EditorPanel {
  public:
    const char* title() const override {
        return "Viewport";
    }

    void draw(EditorContext&) override {}
};

// What an editor session owns for its areas, without a window or GPU, over a
// small project in a temporary directory.
struct SessionFixture {
    std::filesystem::path directory;
    Scene scene;
    AssetManager assets;
    PhysicsWorld physics;
    AudioSystem audio;
    ApplicationControl control;
    RenderRuntime rendering;
    GameUiContext gameUi;
    Logger logger;
    ApplicationContext app{scene, assets, physics, audio, control, rendering, gameUi, logger};
    EditorDocument document{std::filesystem::temp_directory_path()};
    CommandHistory history;
    EditorDiagnostics diagnostics;
    Selection selection;
    NoSessionActions actions;
    EditorMenus menus;
    OperatorRegistry operators;
    OperatorRunner runner{operators, history};
    EditorToolSettings tools;
    RenderSettingsController renderSettings;
    // Without watchers, so rescans happen exactly when a test advances time.
    ProjectAssets projectAssets;
    BuildPanel build{tools};
    InfoPanel info{runner.history()};
    ViewportStandIn viewport;
    AreaRegistry registry;

    explicit SessionFixture(std::filesystem::path root)
        : directory(prepare(std::move(root)))
        , renderSettings(EditorRenderSettings(ProjectConfig{}, directory / "settings" / "render-settings.json"))
        , projectAssets(directory / "content", directory / "scenes", directory / "settings" / "asset-browser.json",
                        AssetCatalogOptions{.watchFiles = false}) {
        expect(registry.add(AreaType::shared("viewport", viewport)).success, "the viewport stand-in should register");
        for (AreaType& type : builtInAreaTypes({.projectAssets = projectAssets,
                                                .renderSettings = renderSettings,
                                                .tools = tools,
                                                .build = build,
                                                .info = info})) {
            const std::string id = type.id;
            if (!registry.add(std::move(type)).success) {
                std::cerr << "FAILED: built-in area type '" << id << "' should register\n";
                ++failures;
            }
        }
    }

    static std::filesystem::path prepare(std::filesystem::path root) {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root / "content" / "models");
        std::filesystem::create_directories(root / "content" / "textures");
        std::filesystem::create_directories(root / "scenes");
        writeFile(root / "content" / "models" / "cube.gltf", "{}");
        writeFile(root / "content" / "textures" / "noise.png", "png");
        return root;
    }

    EditorContext context() {
        return {.app = app,
                .editing = {scene, assets, document, history, diagnostics},
                .selection = selection,
                .diagnostics = diagnostics,
                .mode = EditorMode::Edit,
                .actions = actions,
                .menus = menus};
    }

    // One headless frame updating and drawing panel in its own window, as
    // the session does for a visible area.
    void drawFrame(EditorPanel& panel, const char* window) {
        EditorContext editor = context();
        panel.update(editor, InputState{}, 1.0f / 60.0f);
        HeadlessImGuiFrame frame(window);
        panel.draw(editor);
    }
};

std::vector<const AreaInstance*> areasOfType(const Workspace& workspace, std::string_view typeId) {
    std::vector<const AreaInstance*> areas;
    for (const AreaInstance& area : workspace.areas()) {
        if (area.typeId == typeId) {
            areas.push_back(&area);
        }
    }
    return areas;
}

// Every built-in type registers with its documented instancing, and a panel
// of each instantiates and draws headless frames without reporting errors.
void testEveryTypeDraws(const std::filesystem::path& directory) {
    SessionFixture fixture(directory);
    fixture.projectAssets.refresh(fixture.diagnostics);
    const std::vector<std::pair<std::string_view, AreaInstancing>> expected{
        {"viewport", AreaInstancing::Single},        {"hierarchy", AreaInstancing::Multiple},
        {"inspector", AreaInstancing::Multiple},     {"properties", AreaInstancing::Multiple},
        {"diagnostics", AreaInstancing::Multiple},   {"render_statistics", AreaInstancing::Multiple},
        {"asset_browser", AreaInstancing::Multiple}, {"graphics", AreaInstancing::Multiple},
        {"build", AreaInstancing::Single},           {"info", AreaInstancing::Single},
    };
    const std::span<const AreaType> types = fixture.registry.types();
    expect(types.size() == expected.size(), "the editor should register ten built-in area types");
    for (size_t index = 0; index < std::min(types.size(), expected.size()); ++index) {
        if (types[index].id != expected[index].first || types[index].instancing != expected[index].second) {
            std::cerr << "FAILED: area type " << index << " should be '" << expected[index].first
                      << "' with its documented instancing\n";
            ++failures;
        }
    }

    HeadlessImGuiContext imgui;
    for (const AreaType& type : types) {
        const std::unique_ptr<EditorPanel> panel = type.create();
        if (panel == nullptr) {
            std::cerr << "FAILED: area type '" << type.id << "' should create a panel\n";
            ++failures;
            continue;
        }
        expect(std::string_view(panel->title()) == type.title, "a panel's title should match its type's title");
        fixture.drawFrame(*panel, panel->title());
        fixture.drawFrame(*panel, panel->title());
        EditorContext editor = fixture.context();
        panel->hidden(editor);
    }
    expect(!fixture.diagnostics.hasUnacknowledgedError(), "drawing every area type should report no errors");
    expect(fixture.history.size() == 0, "drawing every area type should not edit the scene");
}

// Asset Browser areas: each has its own view over the one ProjectAssets, the
// first drawn becomes the primary, and closing or hiding it promotes another.
void testAssetBrowserAreas(const std::filesystem::path& directory) {
    SessionFixture fixture(directory);
    const std::filesystem::path models = normalizedCatalogPath(directory / "content" / "models");
    const std::filesystem::path textures = normalizedCatalogPath(directory / "content" / "textures");
    int reloads = 0;
    const ProjectAssetActions actions{.reloadModel = [&reloads](const std::filesystem::path&) { ++reloads; }};

    Workspace workspace("Test");
    workspace.sync(fixture.registry);
    const AreaResult first = workspace.open(fixture.registry, "asset_browser");
    const AreaResult second = workspace.openNew(fixture.registry, "asset_browser");
    expect(first.success && second.success && first.areaId != second.areaId,
           "the asset browser should open in two areas");
    auto& firstBrowser = dynamic_cast<AssetBrowserPanel&>(*workspace.find(first.areaId)->panel);
    auto& secondBrowser = dynamic_cast<AssetBrowserPanel&>(*workspace.find(second.areaId)->panel);
    expect(&firstBrowser != &secondBrowser && &firstBrowser.catalog() == &secondBrowser.catalog(),
           "each area should have its own browser over the shared project");
    expect(firstBrowser.role() == AssetBrowserRole::Secondary && secondBrowser.role() == AssetBrowserRole::Secondary &&
               !fixture.projectAssets.hasPrimaryView(),
           "browsers created by areas should not be primary before they are shown");

    HeadlessImGuiContext imgui;
    // One frame as the session runs it: poll once, then every area. An area
    // named by notDrawn is skipped, as ImGui skips a background dock tab.
    const auto frame = [&](float deltaSeconds, std::string_view notDrawn = {}) {
        fixture.projectAssets.pollChanges(fixture.diagnostics, actions, deltaSeconds);
        for (const AreaInstance& area : workspace.areas()) {
            if (area.panel && area.typeId == "asset_browser" && area.id() != notDrawn) {
                if (area.visible) {
                    fixture.drawFrame(*area.panel, area.id().c_str());
                } else {
                    EditorContext editor = fixture.context();
                    area.panel->hidden(editor);
                }
            }
        }
    };
    frame(0.0f);
    expect(firstBrowser.role() == AssetBrowserRole::Primary && secondBrowser.role() == AssetBrowserRole::Secondary,
           "the first browser drawn should become the primary");

    firstBrowser.navigateTo(models, fixture.diagnostics);
    secondBrowser.navigateTo(textures, fixture.diagnostics);
    expect(firstBrowser.currentFolder() == models && secondBrowser.currentFolder() == textures,
           "each area should keep its own folder");
    expect(fixture.projectAssets.settings().lastFolder == "models",
           "only the primary browser's folder should be remembered");

    // A change on disk is scanned and dispatched once however many areas draw.
    writeFile(directory / "content" / "models" / "cube.gltf", R"({"asset":{"version":"2.0"}})");
    frame(1.5f);
    frame(1.5f);
    expect(reloads == 1, "a changed model should be reloaded exactly once with two browsers open");

    // A primary that stops being drawn (a background tab, another workspace)
    // loses the role to a browser that is drawn, after one grace frame.
    frame(0.0f, first.areaId);
    expect(firstBrowser.role() == AssetBrowserRole::Primary, "one undrawn frame should not move the primary role");
    frame(0.0f, first.areaId);
    expect(secondBrowser.role() == AssetBrowserRole::Primary && firstBrowser.role() == AssetBrowserRole::Secondary &&
               fixture.projectAssets.settings().lastFolder == "textures",
           "a drawn browser should replace a primary that is no longer drawn");
    frame(0.0f);
    frame(0.0f);
    expect(secondBrowser.role() == AssetBrowserRole::Primary, "a drawn primary should keep the role");
    frame(0.0f, second.areaId);
    frame(0.0f, second.areaId);
    expect(firstBrowser.role() == AssetBrowserRole::Primary && fixture.projectAssets.settings().lastFolder == "models",
           "the role should move back once only the first browser is drawn");

    // Closing the primary's area destroys its browser; the other is promoted
    // the next time it is drawn and its folder becomes the remembered one.
    expect(workspace.close(first.areaId).success && workspace.find(first.areaId) == nullptr,
           "closing one of two browser areas should remove it");
    expect(!fixture.projectAssets.hasPrimaryView(), "a destroyed primary should give up the role");
    frame(0.0f);
    expect(secondBrowser.role() == AssetBrowserRole::Primary, "the remaining browser should be promoted");
    expect(fixture.projectAssets.settings().lastFolder == "textures",
           "a promoted browser's folder should become the remembered one");

    // Hiding the last browser area gives up the role; showing it takes it back.
    workspace.close(second.areaId);
    frame(0.0f);
    expect(!fixture.projectAssets.hasPrimaryView(), "a hidden browser should give up the primary role");
    workspace.open(fixture.registry, "asset_browser");
    frame(0.0f);
    expect(secondBrowser.role() == AssetBrowserRole::Primary, "a browser shown again should become primary");
    expect(!fixture.diagnostics.hasUnacknowledgedError(), "the browser areas should report no errors");
}

// Each default workspace syncs against the real area types, keeps its
// Properties tabs, opens headless in its default dock layout, and round trips
// through workspaces.json.
void testDefaultWorkspaces(const std::filesystem::path& directory) {
    SessionFixture fixture(directory);
    fixture.projectAssets.refresh(fixture.diagnostics);
    for (const DefaultWorkspace& defaults : defaultWorkspaces()) {
        for (const DefaultArea& area : defaults.areas) {
            if (fixture.registry.find(area.type) == nullptr) {
                std::cerr << "FAILED: default workspace " << defaults.name << " lists unknown type '" << area.type
                          << "'\n";
                ++failures;
            }
        }
    }

    WorkspaceSet workspaces;
    std::vector<std::string> warnings;
    restoreWorkspaces(defaultWorkspaceFile(), workspaces, warnings);
    workspaces.sync(fixture.registry);
    expect(warnings.empty(), "the default workspaces should restore without warnings");

    const Workspace* lighting = workspaces.find("Lighting");
    expect(lighting != nullptr, "there should be a Lighting workspace");
    if (lighting != nullptr) {
        const std::vector<const AreaInstance*> properties = areasOfType(*lighting, "properties");
        expect(properties.size() == 2 && properties[0]->visible && properties[1]->visible,
               "Lighting should show two Properties areas");
        if (properties.size() == 2) {
            expect(properties[0]->state() == "world" && properties[1]->state() == "render",
                   "Lighting's Properties areas should open on the World and Render tabs");
            const auto* world = dynamic_cast<const PropertiesPanel*>(properties[0]->panel.get());
            expect(world != nullptr && world->activeTab(fixture.context()).kind == PropertiesTabKind::World,
                   "the first Lighting Properties panel should show the World tab");
        }
        expect(lighting->anyVisible("graphics") && lighting->anyVisible("viewport"),
               "Lighting should show the Viewport and Graphics");
    }
    const Workspace* debug = workspaces.find("Debug");
    expect(debug != nullptr && debug->anyVisible("info") && debug->anyVisible("diagnostics") &&
               debug->anyVisible("render_statistics") && debug->anyVisible("build") && debug->anyVisible("viewport"),
           "Debug should show the Viewport, Info, Diagnostics, Render Statistics, and Build");
    const Workspace* layout = workspaces.find("Layout");
    expect(layout != nullptr && layout->anyVisible("viewport") && layout->anyVisible("hierarchy") &&
               layout->anyVisible("inspector") && layout->anyVisible("properties") &&
               layout->anyVisible("asset_browser") && layout->anyVisible("info") && !layout->anyVisible("build"),
           "Layout should show the authoring areas and the Asset Browser");

    // Each workspace opens in its default layout with every visible area
    // drawn in the dock slot its defaults give it.
    HeadlessImGui imgui;
    for (Workspace& workspace : workspaces.workspaces()) {
        const DefaultWorkspace& defaults = defaultWorkspaceFor(workspace.name());
        restoreImGuiLayout({});
        const ThemeChrome chrome;
        const auto drawAreas = [&](bool buildLayout) {
            const ImGuiID dockspace = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
            if (buildLayout) {
                buildDefaultDockLayout(dockspace, defaults, workspace, fixture.registry);
            }
            for (AreaInstance& area : workspace.areas()) {
                const AreaType* type = fixture.registry.find(area.typeId);
                if (!area.visible || type == nullptr || !area.panel) {
                    continue;
                }
                EditorContext editor = fixture.context();
                area.panel->update(editor, InputState{}, 1.0f / 60.0f);
                area.dockId = drawAreaWindow(area, *type, fixture.registry, area.panel->windowFlags(), chrome, [&] {
                                  area.panel->draw(editor);
                              }).dockId;
            }
        };
        imgui.frame([&] { drawAreas(true); });
        for (int frame = 0; frame < 2; ++frame) {
            imgui.frame([&] { drawAreas(false); });
        }
        for (const AreaInstance& area : workspace.areas()) {
            if (area.visible && area.dockId == 0) {
                std::cerr << "FAILED: area " << area.id() << " of " << workspace.name()
                          << " should be docked by the default layout\n";
                ++failures;
            }
        }
        if (workspace.name() == "Lighting") {
            const AreaInstance* world = workspace.find("properties.1");
            const AreaInstance* render = workspace.find("properties.2");
            const AreaInstance* graphics = workspace.find("graphics.1");
            expect(world != nullptr && render != nullptr && graphics != nullptr && world->dockId != render->dockId &&
                       render->dockId == graphics->dockId,
                   "Lighting should dock World Properties above Render Properties and Graphics");
        }
    }
    expect(!fixture.diagnostics.hasUnacknowledgedError(), "opening the default workspaces should report no errors");

    // Round trip: what the live workspaces capture saves and loads unchanged,
    // including the per-area Properties tabs, and restores identically.
    const WorkspaceFileContents captured = captureWorkspaces(workspaces);
    const std::filesystem::path path = directory / "settings" / "workspaces.json";
    expect(saveWorkspaceFile(path, captured).success, "the default workspaces should save");
    WorkspaceFileContents loaded;
    const WorkspaceFileResult result = loadWorkspaceFile(path, loaded);
    expect(result.success && result.found && !result.outdated && loaded == captured,
           "the default workspaces should load as saved");
    WorkspaceSet reloaded;
    restoreWorkspaces(loaded, reloaded, warnings);
    reloaded.sync(fixture.registry);
    expect(warnings.empty() && captureWorkspaces(reloaded) == captured,
           "reloaded default workspaces should capture identically");
    const Workspace* reloadedLighting = reloaded.find("Lighting");
    const AreaInstance* reloadedRender = reloadedLighting != nullptr ? reloadedLighting->find("properties.2") : nullptr;
    const auto* renderPanel =
        reloadedRender != nullptr ? dynamic_cast<const PropertiesPanel*>(reloadedRender->panel.get()) : nullptr;
    expect(renderPanel != nullptr && renderPanel->activeTab(fixture.context()).kind == PropertiesTabKind::Render,
           "a reloaded Properties area should reopen on its saved tab");
}
} // namespace

int main() {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "zenith_editor_area_types_tests";
    testEveryTypeDraws(directory / "types");
    testAssetBrowserAreas(directory / "browsers");
    testDefaultWorkspaces(directory / "defaults");
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    if (failures != 0) {
        std::cerr << failures << " editor area type test(s) failed\n";
        return 1;
    }
    std::cout << "Editor area type tests passed\n";
    return 0;
}
