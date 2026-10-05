// The editor's operator family table, run headless as the session runs it:
// every family registers, and no operator id is claimed twice.

#include "EditorTestFixture.hpp"
#include "editor/application/EditorPlayMode.hpp"
#include "editor/application/EditorSessionCommands.hpp"
#include "editor/areas/AreaRegistry.hpp"
#include "editor/areas/Workspace.hpp"
#include "editor/interaction/WatchList.hpp"
#include "editor/modules/GameModuleHost.hpp"
#include "editor/operators/EditorOperatorFamilies.hpp"
#include "editor/panels/PreferencesWindow.hpp"
#include "editor/project/PackageRunner.hpp"
#include "editor/project/ProjectBuild.hpp"
#include "editor/settings/EditorSettings.hpp"
#include "engine/navigation/NavigationWorld.hpp"

#include <iostream>
#include <set>
#include <string>
#include <string_view>

namespace {
int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

// The session's workflows, doing nothing: registration only stores them.
class NoSessionCommands final : public EditorDocumentCommands,
                                public EditorPlayCommands,
                                public EditorObjectModeCommands,
                                public EditorWindowCommands,
                                public EditorPrefabModeCommands {
  public:
    bool documentCommandsAvailable(const OperatorContext&) const override {
        return false;
    }

    void requestNew(ApplicationContext&) override {}

    void requestOpen(ApplicationContext&) override {}

    bool saveDocument(ApplicationContext&) override {
        return false;
    }

    void requestSaveAs() override {}

    void undo(ApplicationContext&) override {}

    void redo(ApplicationContext&) override {}

    OperatorResult copySelection(ApplicationContext&) override {
        return OperatorResult::cancelled();
    }

    OperatorResult pasteClipboard(ApplicationContext&) override {
        return OperatorResult::cancelled();
    }

    bool projectHubAvailable() const override {
        return false;
    }

    void requestProjectHub(ApplicationContext&, ProjectHubEntry) override {}

    void requestClose(ApplicationContext&) override {}

    bool standaloneRunning() const override {
        return false;
    }

    PlayTarget lastPlayTarget() const override {
        return PlayTarget::ActiveViewport;
    }

    OperatorResult startPlay(ApplicationContext&, PlayTarget) override {
        return OperatorResult::cancelled();
    }

    void startSimulate(ApplicationContext&) override {}

    bool playActive() const override {
        return false;
    }

    void stopPlay(ApplicationContext&) override {}

    ViewportPanel* gameViewport() const override {
        return nullptr;
    }

    bool modalTransformRunning() const override {
        return false;
    }

    bool beginModalTransform(ApplicationContext&, TransformOperation, std::string, std::string, bool) override {
        return false;
    }

    void openDeleteMenu() override {}

    void openRenamePopup(ApplicationContext&) override {}

    void openOperatorSearch() override {}

    void openKeymapEditor() override {}

    void workspacesChanged() override {}

    void resetActiveWorkspace() override {}

    void requestModuleReload() override {}

    void openNewCodeDialog(GeneratedCodeKind) override {}

    void openProjectSettings(ApplicationContext&) override {}

    bool prefabEditActive() const override {
        return false;
    }

    bool prefabEditAvailable(const OperatorContext&) const override {
        return false;
    }

    OperatorResult editPrefab(ApplicationContext&, const std::filesystem::path&) override {
        return OperatorResult::cancelled();
    }

    void requestLeavePrefabEdit(ApplicationContext&) override {}
};

// What the session owns for its operator families, beyond the shared
// fixture, without a window or GPU.
struct OperatorFamilyFixture : EditorTestFixture {
    EditorPlayMode playMode;
    GameModuleHost modules{{.operators = operators,
                            .runner = runner,
                            .menus = menus.registry(),
                            .viewports = viewports,
                            .componentEditors = componentEditors}};
    PreferencesWindow preferences;
    PackageRunner packageRunner{makeProcessPackageStepRunner()};
    NavigationWorld navigationPreview;
    EditorToolSettings tools;
    ProjectBuild build{tools, diagnostics};
    WatchList watches;
    WorkspaceSet workspaces;
    AreaRegistry areaTypes;
    NoSessionCommands session;

    EditorOperatorServices services() {
        return {.document = document,
                .history = history,
                .eyedropper = eyedropper,
                .componentClipboard = clipboard,
                .componentLayout = componentLayout,
                .menus = menus,
                .viewports = viewports,
                .workspaces = workspaces,
                .areaTypes = areaTypes,
                .playMode = playMode,
                .modules = modules,
                .preferences = preferences,
                .packageRunner = packageRunner,
                .build = build,
                .watches = watches,
                .navigationPreview = navigationPreview,
                .tools = tools,
                .projectFile = {},
                .revealAsset = {},
                .simulateMaskChanged = {},
                .documentCommands = session,
                .playCommands = session,
                .objectModeCommands = session,
                .windowCommands = session,
                .prefabModeCommands = session};
    }
};

void testEveryFamilyRegisters() {
    OperatorFamilyFixture fixture;
    const EditorOperatorServices services = fixture.services();
    std::set<std::string_view> names;
    for (const OperatorFamily& family : editorOperatorFamilies()) {
        const std::string name(family.name);
        expect(!family.name.empty(), "every family has a name");
        expect(names.insert(family.name).second, "family name '" + name + "' is unique");
        expect(family.registerOperators != nullptr, "family '" + name + "' has a register function");
        if (family.registerOperators == nullptr) {
            continue;
        }
        const size_t before = fixture.operators.size();
        // A duplicate id across families fails here, as the registry
        // rejects it.
        const OperatorRegistrationResult result = family.registerOperators(fixture.operators, services);
        expect(result.success, "family '" + name + "' registers: " + result.error);
        expect(fixture.operators.size() > before, "family '" + name + "' registers at least one operator");
    }

    std::set<std::string> ids;
    for (const Operator* op : fixture.operators.operators()) {
        expect(ids.insert(op->id).second, "operator '" + op->id + "' is registered once");
    }
    // A sample from each kind of family, as menus and keymaps name them.
    for (const char* id : {"file.save",
                           "play.start",
                           "wm.search_menu",
                           "transform.translate",
                           "view3d.view_axis",
                           "view3d.cursor3d_place",
                           "module.reload",
                           "wm.toggle_panel",
                           "file.build_package",
                           "object.duplicate",
                           "ui.eyedropper_cancel",
                           "object.component_copy",
                           "wm.call_menu",
                           "prefab.apply",
                           "play.set_time_scale",
                           "build.run",
                           "build.cancel",
                           "module.new_behaviour",
                           "module.generate_code",
                           "debug.attach",
                           "debug.watch_field"}) {
        expect(fixture.operators.find(id) != nullptr, std::string("operator '") + id + "' is registered");
    }
}

void testDuplicateFamilyFails() {
    OperatorFamilyFixture fixture;
    const EditorOperatorServices services = fixture.services();
    const OperatorFamily& first = editorOperatorFamilies().front();
    expect(first.registerOperators(fixture.operators, services).success, "the first family registers");
    const OperatorRegistrationResult again = first.registerOperators(fixture.operators, services);
    expect(!again.success && again.error.find("already registered") != std::string::npos,
           "registering a family twice reports the duplicate id: " + again.error);
}
} // namespace

int main() {
    testEveryFamilyRegisters();
    testDuplicateFamilyFails();
    if (failures != 0) {
        std::cerr << failures << " operator family test(s) failed\n";
        return 1;
    }
    return 0;
}
