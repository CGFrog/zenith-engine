#include "HeadlessImGui.hpp"

#include "editor/areas/AreaRegistry.hpp"
#include "editor/areas/AreaWindows.hpp"
#include "editor/areas/DefaultWorkspaces.hpp"
#include "editor/areas/Workspace.hpp"
#include "editor/areas/WorkspaceFile.hpp"
#include "editor/panels/EditorPanel.hpp"
#include "editor/settings/EditorSettings.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

class TestPanel final : public EditorPanel {
  public:
    explicit TestPanel(const char* title)
        : m_title(title) {}

    const char* title() const override {
        return m_title;
    }

    void draw(EditorContext&) override {}

  private:
    const char* m_title;
};

AreaType multipleType(std::string id, std::string title) {
    return {.id = std::move(id), .title = std::move(title), .instancing = AreaInstancing::Multiple, .create = [] {
                return std::make_unique<TestPanel>("Test");
            }};
}

// The registry the area tests use: two multiple-instance types and one
// single-instance type showing a shared panel.
struct RegistryFixture {
    TestPanel solo{"Solo"};
    AreaRegistry registry;

    RegistryFixture() {
        registry.add(multipleType("alpha", "Alpha"));
        registry.add(multipleType("beta", "Beta"));
        registry.add(AreaType::shared("solo", solo));
    }
};

size_t countAreas(const Workspace& workspace, std::string_view typeId) {
    return static_cast<size_t>(
        std::ranges::count_if(workspace.areas(), [typeId](const AreaInstance& area) { return area.typeId == typeId; }));
}

void testRegistry() {
    AreaRegistry registry;
    expect(registry.add(multipleType("alpha", "Alpha")).success, "a valid type should register");
    expect(!registry.add(multipleType("alpha", "Other")).success, "a duplicate id should be refused");
    expect(!registry.add(multipleType("", "Empty")).success, "an empty id should be refused");
    expect(!registry.add(multipleType("a.b", "Dotted")).success, "an id with '.' should be refused");
    expect(!registry.add(multipleType("a#b", "Hashed")).success, "an id with '#' should be refused");
    expect(!registry.add({.id = "untitled", .create = [] { return std::make_unique<TestPanel>("x"); }}).success,
           "a type without a title should be refused");
    expect(!registry.add({.id = "nofactory", .title = "No Factory"}).success,
           "a type without a factory should be refused");

    TestPanel shared("Shared Title");
    expect(registry.add(AreaType::shared("shared", shared)).success, "a shared panel type should register");
    const AreaType* sharedType = registry.find("shared");
    expect(sharedType != nullptr && sharedType->title == "Shared Title" &&
               sharedType->instancing == AreaInstancing::Single,
           "a shared type should be single-instance and titled after its panel");
    if (sharedType != nullptr) {
        const std::unique_ptr<EditorPanel> view = sharedType->create();
        expect(view != nullptr && std::string_view(view->title()) == "Shared Title",
               "a shared type's panel should forward to the shared panel");
    }
    expect(registry.types().size() == 2 && registry.types()[0].id == "alpha" && registry.types()[1].id == "shared",
           "types should keep registration order");
    expect(registry.remove("alpha") && registry.find("alpha") == nullptr && !registry.remove("alpha"),
           "removing a type should report whether it existed");
}

void testWorkspaceAreas() {
    RegistryFixture fixture;
    Workspace workspace("Test");
    workspace.sync(fixture.registry);
    expect(workspace.areas().size() == 3 && std::ranges::none_of(workspace.areas(), &AreaInstance::visible),
           "sync should give every type one hidden area");
    expect(std::ranges::all_of(workspace.areas(), [](const AreaInstance& area) { return area.panel != nullptr; }),
           "synced areas of registered types should have panels");

    AreaResult opened = workspace.open(fixture.registry, "alpha");
    expect(opened.success && opened.areaId == "alpha.1" && workspace.anyVisible("alpha"),
           "open should show the type's hidden area");
    workspace.find("alpha.1")->dockId = 42;
    AreaResult second = workspace.openNew(fixture.registry, "alpha");
    const AreaInstance* secondArea = workspace.find(second.areaId);
    expect(second.success && second.areaId == "alpha.2" && secondArea != nullptr && secondArea->visible &&
               secondArea->dockRequest == 42,
           "openNew should add a numbered area docked beside the visible one");
    expect(secondArea != nullptr && secondArea->windowName("Alpha") == "Alpha##alpha.2",
           "area windows should be named <title>##<type>.<number>");
    expect(workspace.openNew(fixture.registry, "solo").areaId == "solo.1" && countAreas(workspace, "solo") == 1,
           "openNew on a single-instance type should show its one area");
    expect(!workspace.open(fixture.registry, "missing").success, "opening an unknown type should fail");

    expect(workspace.close("alpha.2").success && workspace.find("alpha.2") == nullptr,
           "closing one of several areas of a type should remove it");
    expect(workspace.close("alpha.1").success && workspace.find("alpha.1") != nullptr &&
               !workspace.find("alpha.1")->visible,
           "closing the last area of a type should only hide it");
    expect(!workspace.close("alpha.9").success, "closing an unknown area should fail");

    workspace.openNew(fixture.registry, "beta");
    workspace.openNew(fixture.registry, "beta");
    expect(countAreas(workspace, "beta") == 2 && workspace.anyVisible("beta"), "two Beta areas should be open");
    expect(workspace.toggle(fixture.registry, "beta").success && !workspace.anyVisible("beta") &&
               countAreas(workspace, "beta") == 1,
           "toggling an open type should close its areas, keeping one hidden");
    expect(workspace.toggle(fixture.registry, "beta").success && workspace.anyVisible("beta"),
           "toggling a hidden type should show it");

    fixture.registry.remove("beta");
    workspace.sync(fixture.registry);
    expect(workspace.find("beta.2") != nullptr && workspace.find("beta.2")->panel == nullptr,
           "an area whose type is unregistered should keep its place without a panel");
    fixture.registry.add(multipleType("beta", "Beta"));
    workspace.sync(fixture.registry);
    expect(workspace.find("beta.2") != nullptr && workspace.find("beta.2")->panel != nullptr,
           "an area should get its panel back when its type returns");

    expect(!workspace.addStoredArea("beta", 2, true).success, "a duplicate stored area should be refused");
    expect(!workspace.addStoredArea("bad.type", 1, true).success, "a malformed stored area should be refused");

    workspace.close("solo.1");
    expect(workspace.addStoredArea("solo", 2, true).success, "a stored second Solo area should be accepted");
    workspace.sync(fixture.registry);
    expect(countAreas(workspace, "solo") == 1 && workspace.find("solo.2") != nullptr &&
               workspace.find("solo.2")->visible,
           "sync should keep one area of a single-instance type, preferring a visible one");
}

void testChangeType() {
    RegistryFixture fixture;
    Workspace workspace("Test");
    workspace.sync(fixture.registry);
    workspace.open(fixture.registry, "alpha");
    workspace.find("alpha.1")->dockId = 7;

    AreaResult changed = workspace.changeType(fixture.registry, "alpha.1", "beta");
    const AreaInstance* beta = workspace.find("beta.1");
    expect(changed.success && changed.areaId == "beta.1" && beta != nullptr && beta->visible && beta->dockRequest == 7,
           "changing type should show the new type in the area's dock node");
    expect(workspace.find("alpha.1") != nullptr && !workspace.find("alpha.1")->visible &&
               countAreas(workspace, "beta") == 1,
           "the replaced area should be hidden and the hidden Beta area reused");

    workspace.open(fixture.registry, "solo");
    workspace.find("beta.1")->dockId = 9;
    changed = workspace.changeType(fixture.registry, "beta.1", "solo");
    expect(changed.success && changed.areaId == "solo.1" && countAreas(workspace, "solo") == 1 &&
               workspace.find("solo.1")->dockRequest == 9,
           "a single-instance type open elsewhere should move into the area");
    expect(!workspace.changeType(fixture.registry, "solo.1", "missing").success,
           "changing to an unknown type should fail");
    expect(workspace.changeType(fixture.registry, "solo.1", "solo").success && workspace.find("solo.1")->visible,
           "changing to the same type should do nothing");
}

void testWorkspaceSet() {
    WorkspaceSet workspaces;
    std::vector<Workspace> list;
    list.emplace_back("One");
    list.emplace_back("Two");
    workspaces.assign(std::move(list), "Two");
    expect(workspaces.active().name() == "Two", "assign should activate the named workspace");
    expect(!workspaces.requestActivation("Three").success, "an unknown workspace cannot be requested");
    expect(workspaces.requestActivation("One").success && workspaces.requestedActivation() == 0u,
           "a known workspace can be requested");
    expect(workspaces.active().name() == "Two", "a request should not switch until completed");
    workspaces.completeActivation();
    expect(workspaces.active().name() == "One" && !workspaces.requestedActivation(),
           "completing should switch and clear the request");
}

std::filesystem::path testDirectory() {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "zenith_workspace_tests";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    return directory;
}

void writeText(const std::filesystem::path& path, std::string_view text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

void testWorkspaceFileRoundTrip(const std::filesystem::path& directory) {
    RegistryFixture fixture;
    WorkspaceSet workspaces;
    std::vector<std::string> warnings;
    restoreWorkspaces(defaultWorkspaceFile(), workspaces, warnings);
    expect(warnings.empty() && workspaces.workspaces().size() == 3 && workspaces.active().name() == "Layout",
           "the default workspaces should be Layout, Lighting, and Debug");
    workspaces.sync(fixture.registry);
    workspaces.active().openNew(fixture.registry, "alpha");
    workspaces.active().openNew(fixture.registry, "alpha");
    workspaces.active().setLayout("[Window][Alpha##alpha.2]\nPos=1,2\n");
    workspaces.requestActivation("Debug");
    workspaces.completeActivation();

    const WorkspaceFileContents saved = captureWorkspaces(workspaces);
    const std::filesystem::path path = directory / "workspaces.json";
    expect(saveWorkspaceFile(path, saved).success, "workspaces should save");
    WorkspaceFileContents loaded;
    const WorkspaceFileResult result = loadWorkspaceFile(path, loaded);
    expect(result.success && result.found && loaded == saved, "workspaces should round trip through the file");
    expect(loaded.activeWorkspace == "Debug", "the active workspace should round trip");

    WorkspaceSet restored;
    restoreWorkspaces(loaded, restored, warnings);
    expect(warnings.empty() && captureWorkspaces(restored) == saved, "restored workspaces should capture the same");
    const Workspace* layout = restored.find("Layout");
    expect(layout != nullptr && layout->find("alpha.2") != nullptr && layout->find("alpha.2")->visible &&
               layout->layout() == "[Window][Alpha##alpha.2]\nPos=1,2\n",
           "areas and the ImGui layout should be restored per workspace");

    WorkspaceFileContents untouched = saved;
    expect(loadWorkspaceFile(directory / "missing.json", untouched).success &&
               !loadWorkspaceFile(directory / "missing.json", untouched).found && untouched == saved,
           "a missing file should load as not found");
    writeText(path, R"({"version":2,"workspaces":[{"name":"A"}]})");
    expect(!loadWorkspaceFile(path, untouched).success && untouched == saved, "an unknown version should be refused");
    writeText(path, R"({"version":1,"workspaces":[{"name":"A"},{"name":"A"}]})");
    expect(!loadWorkspaceFile(path, untouched).success, "duplicate workspace names should be refused");
    writeText(path, R"({"version":1,"workspaces":[{"name":"A","areas":[{"type":"alpha","instance":0}]}]})");
    expect(!loadWorkspaceFile(path, untouched).success, "an area instance of 0 should be refused");
    writeText(path, R"({"version":1,"activeWorkspace":"B","workspaces":[{"name":"A","areas":[{"type":"x.y"}]}]})");
    expect(loadWorkspaceFile(path, untouched).success, "a structurally valid file should load");
    WorkspaceSet lenient;
    std::vector<std::string> lenientWarnings;
    restoreWorkspaces(untouched, lenient, lenientWarnings);
    expect(lenientWarnings.size() == 1 && lenient.active().name() == "A" && lenient.active().areas().empty(),
           "an invalid area should be skipped with a warning and an unknown active name fall back to the first");
}

void testMigration(const std::filesystem::path& directory) {
    const std::filesystem::path settingsPath = directory / "settings.ini";
    writeText(settingsPath, "layout.version=4\npanel.hierarchy=false\npanel.build=false\nbuild.command=make\n");
    const std::string legacyLayout = "[Window][Hierarchy]\r\nPos=0,0\r\nSize=200,600\r\nDockId=0x00000001,0\r\n\r\n"
                                     "[Window][Render Statistics]\nPos=10,10\nDockId=0x00000002,1\n\n"
                                     "[Window][Hierarchy Extra]\nPos=5,5\n\n"
                                     "[Docking][Data]\nDockSpace ID=0x00000003 Window=0x1 Pos=0,0 Size=1280,720 "
                                     "Split=X\n  DockNode ID=0x00000001 Parent=0x00000003 SizeRef=200,720\n"
                                     "  DockNode ID=0x00000002 Parent=0x00000003 SizeRef=1080,720\n";
    EditorSettings settings(settingsPath);
    expect(settings.load().success, "legacy settings should load");
    const WorkspaceFileContents migrated = migrateLegacyLayout(
        {.panels = settings.panels(), .layoutVersion = settings.layoutVersion(), .imguiLayout = legacyLayout});
    expect(migrated.activeWorkspace == "Layout" && migrated.workspaces.size() == 3,
           "migration should produce the default workspaces");
    const WorkspaceRecord& layout = migrated.workspaces.front();
    const auto visible = [&layout](std::string_view type) {
        const auto area = std::ranges::find(layout.areas, type, &AreaRecord::type);
        return area != layout.areas.end() && area->visible;
    };
    expect(!visible("hierarchy") && !visible("build") && visible("viewport") && visible("inspector"),
           "legacy panel flags should become Layout area visibility");
    expect(layout.imguiLayout.find("[Window][Hierarchy##hierarchy.1]\r\n") != std::string::npos &&
               layout.imguiLayout.find("[Window][Render Statistics##render_statistics.1]\n") != std::string::npos &&
               layout.imguiLayout.find("[Window][Hierarchy]") == std::string::npos,
           "legacy window sections should be renamed to their areas' windows");
    expect(layout.imguiLayout.find("[Window][Hierarchy Extra]") != std::string::npos &&
               layout.imguiLayout.find("[Docking][Data]") != std::string::npos,
           "other windows and the docking tree should be kept");

    {
        HeadlessImGui imgui;
        ImGui::LoadIniSettingsFromMemory(layout.imguiLayout.c_str(), layout.imguiLayout.size());
        const ImGuiWindowSettings* hierarchy = ImGui::FindWindowSettingsByID(ImHashStr("Hierarchy##hierarchy.1"));
        expect(hierarchy != nullptr && hierarchy->DockId == 1u, "ImGui should read the migrated window's dock slot");
    }

    const WorkspaceFileContents stale =
        migrateLegacyLayout({.panels = settings.panels(), .layoutVersion = 3, .imguiLayout = legacyLayout});
    expect(stale == defaultWorkspaceFile(), "a stale legacy layout should give the default workspaces");

    expect(settings.save().success, "settings should save");
    std::ifstream input(settingsPath);
    std::stringstream text;
    text << input.rdbuf();
    expect(text.str().find("panel.") == std::string::npos && text.str().find("layout.version") == std::string::npos,
           "settings.ini should no longer store panel or layout keys");
    expect(text.str().find("build.command=make") != std::string::npos, "tool settings should still be saved");
}

// Draws every visible area of the workspace inside a dockspace, recording the
// dock nodes as the editor does.
struct HeadlessAreaHost {
    AreaRegistry& registry;
    std::map<std::string, int> draws;
    ImGuiID dockspace{0};

    void draw(Workspace& workspace) {
        dockspace = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
        for (AreaInstance& area : workspace.areas()) {
            const AreaType* type = registry.find(area.typeId);
            if (!area.visible || type == nullptr) {
                continue;
            }
            const std::string id = area.id();
            const AreaWindowResult result = drawAreaWindow(area, *type, registry, 0, [this, &id] { ++draws[id]; });
            area.dockId = result.dockId;
        }
    }
};

ImGuiWindow* window(const char* name) {
    return ImGui::FindWindowByName(name);
}

void testHeadlessTypeSwitch() {
    RegistryFixture fixture;
    HeadlessImGui imgui;
    HeadlessAreaHost host{fixture.registry};
    Workspace workspace("Test");
    workspace.sync(fixture.registry);
    workspace.open(fixture.registry, "alpha");
    workspace.openNew(fixture.registry, "beta");

    ImGuiID left = 0;
    imgui.frame([&] {
        host.draw(workspace);
        ImGui::DockBuilderRemoveNode(host.dockspace);
        ImGui::DockBuilderAddNode(host.dockspace, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(host.dockspace, ImGui::GetMainViewport()->WorkSize);
        ImGuiID center = host.dockspace;
        left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.3f, nullptr, &center);
        ImGui::DockBuilderDockWindow("Alpha##alpha.1", left);
        ImGui::DockBuilderDockWindow("Beta##beta.1", center);
        ImGui::DockBuilderFinish(host.dockspace);
    });
    for (int frame = 0; frame < 3; ++frame) {
        imgui.frame([&] { host.draw(workspace); });
    }
    expect(host.draws["alpha.1"] > 0 && workspace.find("alpha.1")->dockId == left,
           "the area should draw docked in its node");

    // A second Alpha joins the first one's node as a tab.
    const AreaResult second = workspace.openNew(fixture.registry, "alpha");
    for (int frame = 0; frame < 3; ++frame) {
        imgui.frame([&] { host.draw(workspace); });
    }
    expect(second.areaId == "alpha.2" && window("Alpha##alpha.2") != nullptr &&
               window("Alpha##alpha.2")->DockId == left,
           "a new area should open as a tab beside its type's area");

    // Switching alpha.2 to Solo in place: Solo takes alpha.2's dock node.
    const AreaResult changed = workspace.changeType(fixture.registry, "alpha.2", "solo");
    for (int frame = 0; frame < 3; ++frame) {
        imgui.frame([&] { host.draw(workspace); });
    }
    expect(changed.success && workspace.find("alpha.2") == nullptr, "the retyped extra area should be removed");
    expect(window("Solo##solo.1") != nullptr && window("Solo##solo.1")->DockId == left &&
               window("Solo##solo.1")->Active,
           "the new type should be shown in the retyped area's dock node");
    expect(window("Alpha##alpha.2") != nullptr && !window("Alpha##alpha.2")->Active,
           "the retyped area's window should no longer be drawn");
}

void testHeadlessWorkspaceSwitch() {
    RegistryFixture fixture;
    HeadlessImGui imgui;
    HeadlessAreaHost host{fixture.registry};
    WorkspaceSet workspaces;
    std::vector<Workspace> list;
    list.emplace_back("First");
    list.emplace_back("Second");
    workspaces.assign(std::move(list), "First");
    workspaces.sync(fixture.registry);
    workspaces.workspaces()[0].open(fixture.registry, "alpha");
    workspaces.workspaces()[1].open(fixture.registry, "alpha");
    workspaces.workspaces()[1].open(fixture.registry, "beta");

    ImGuiID left = 0;
    imgui.frame([&] {
        host.draw(workspaces.active());
        ImGui::DockBuilderRemoveNode(host.dockspace);
        ImGui::DockBuilderAddNode(host.dockspace, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(host.dockspace, ImGui::GetMainViewport()->WorkSize);
        ImGuiID center = host.dockspace;
        left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.3f, nullptr, &center);
        ImGui::DockBuilderDockWindow("Alpha##alpha.1", left);
        ImGui::DockBuilderFinish(host.dockspace);
    });
    for (int frame = 0; frame < 3; ++frame) {
        imgui.frame([&] { host.draw(workspaces.active()); });
    }
    expect(window("Alpha##alpha.1")->DockId == left, "the first workspace should dock Alpha on the left");

    // Switch between frames, as the editor does.
    workspaces.requestActivation("Second");
    expect(applyRequestedWorkspace(workspaces), "a requested switch should apply");
    expect(workspaces.find("First")->layout().find("[Window][Alpha##alpha.1]") != std::string::npos,
           "switching should capture the first workspace's layout");
    expect(workspaces.active().name() == "Second" && workspaces.active().layout().empty(),
           "the second workspace should have no layout yet");
    for (int frame = 0; frame < 3; ++frame) {
        imgui.frame([&] { host.draw(workspaces.active()); });
    }
    expect(window("Alpha##alpha.1")->DockId == 0 && window("Beta##beta.1")->Active,
           "a workspace without a layout should start from a cleared layout with its own areas");
    expect(!applyRequestedWorkspace(workspaces), "nothing should apply without a request");

    workspaces.requestActivation("First");
    expect(applyRequestedWorkspace(workspaces), "switching back should apply");
    for (int frame = 0; frame < 3; ++frame) {
        imgui.frame([&] { host.draw(workspaces.active()); });
    }
    expect(window("Alpha##alpha.1")->DockId == left && !window("Beta##beta.1")->Active,
           "switching back should restore the first workspace's dock layout and areas");
}
} // namespace

int main() {
    testRegistry();
    testWorkspaceAreas();
    testChangeType();
    testWorkspaceSet();
    const std::filesystem::path directory = testDirectory();
    testWorkspaceFileRoundTrip(directory);
    testMigration(directory);
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    testHeadlessTypeSwitch();
    testHeadlessWorkspaceSwitch();
    if (failures != 0) {
        std::cerr << failures << " workspace test(s) failed\n";
        return 1;
    }
    std::cout << "Workspace tests passed\n";
    return 0;
}
