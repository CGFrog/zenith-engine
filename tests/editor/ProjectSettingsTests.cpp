// Project Settings: reading and saving the project's layers, input actions,
// tags, and navigation settings (keeping what other windows saved),
// validation, the window drawn headless, the render settings no longer
// overwriting them, the inspector's physics layer names and tags editor, and
// navigation.bake baking with the project's settings.

#include "EditorTestFixture.hpp"
#include "HeadlessImGui.hpp"

#include "editor/interaction/ComponentEditors.hpp"
#include "editor/interaction/ProjectNames.hpp"
#include "editor/interaction/ReflectedFieldEditor.hpp"
#include "editor/operators/NavigationOperators.hpp"
#include "editor/panels/ProjectSettingsWindow.hpp"
#include "editor/settings/EditorRenderSettings.hpp"
#include "engine/input/InputActionsJson.hpp"
#include "engine/navigation/NavigationWorld.hpp"
#include "engine/physics/PhysicsComponents.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include "support/Doctest.hpp"

#include <imgui_internal.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

fs::path makeProject(const fs::path& directory) {
    fs::create_directories(directory / "assets");
    fs::create_directories(directory / "scenes");
    const fs::path projectFile = directory / "game.zenith-project";
    ProjectConfig project{.projectFile = projectFile,
                          .displayName = "Game",
                          .assetRoot = directory / "assets",
                          .sceneRoot = directory / "scenes"};
    CHECK_MESSAGE(static_cast<bool>(saveProjectConfig(projectFile, project)), "the project saves");
    return projectFile;
}

} // namespace

TEST_SUITE_BEGIN("ProjectSettings");

TEST_CASE("ProjectSettings: save and load") {
    const testsupport::TempDirectory root("project-settings-save");
    const fs::path projectFile = makeProject(root / "save");
    ProjectSettingsValues values;
    CHECK_MESSAGE((loadProjectSettingsValues(projectFile, values) && values.layers == PhysicsLayerTable::defaults() &&
                   values.tags.empty() && values.inputActions.actionNames().empty()),
                  "a new project has the default layers, no tags, and no actions");

    // Another window saves the render settings in between: they are kept.
    ProjectConfig opened;
    loadProjectConfig(projectFile, opened);
    EditorRenderSettings render(opened, root / "unused.json");
    RenderSettings changed = render.requested();
    changed.maximumLocalLights = 77;
    render.request(changed);
    CHECK_MESSAGE(static_cast<bool>(render.save()), "the render settings save");

    const std::optional<PhysicsLayer> pickups = values.layers.add("Pickups");
    values.layers.setCollides(*pickups, PhysicsLayer::Player, true);
    values.tags = {"enemy", "pickup"};
    values.inputActions.addAction("Fire", InputActionValueType::Button);
    values.inputActions.addBinding("Fire", {.source = MouseButton::Left});
    CHECK_MESSAGE(static_cast<bool>(saveProjectSettingsValues(projectFile, values)), "the settings save");
    CHECK_MESSAGE(fs::is_regular_file(projectFile.parent_path() / "input_actions.json"),
                  "actions go to input_actions.json beside the project");

    ProjectConfig saved;
    CHECK_MESSAGE((loadProjectConfig(projectFile, saved) && saved.renderSettings.maximumLocalLights == 77 &&
                   saved.tags == values.tags && saved.physicsLayers == values.layers &&
                   saved.inputActions == projectFile.parent_path() / "input_actions.json"),
                  "saving keeps the render settings and names the action file");
    ProjectSettingsValues reloaded;
    CHECK_MESSAGE((loadProjectSettingsValues(projectFile, reloaded) && reloaded.layers == values.layers &&
                   reloaded.tags == values.tags &&
                   serializeInputActionMap(reloaded.inputActions) == serializeInputActionMap(values.inputActions)),
                  "the values read back");

    // The render settings no longer write back the old layers and tags.
    changed.maximumLocalLights = 78;
    render.request(changed);
    render.save();
    ProjectConfig afterRender;
    loadProjectConfig(projectFile, afterRender);
    CHECK_MESSAGE((afterRender.tags == values.tags && afterRender.physicsLayers == values.layers),
                  "saving render settings keeps the project settings saved since");

    ProjectSettingsValues invalid = values;
    invalid.tags = {"enemy", ""};
    CHECK_MESSAGE((!validateProjectSettings(invalid).empty() && !saveProjectSettingsValues(projectFile, invalid)),
                  "an empty tag is refused before anything is written");

    // Navigation settings: the defaults are not written until changed;
    // changed ones are saved after reading the file again, and read back.
    CHECK_MESSAGE((values.navigation == NavMeshBuildSettings{} && !saved.navigation),
                  "unchanged navigation settings are not written to the project");
    changed.maximumLocalLights = 79;
    render.request(changed);
    render.save();
    values.navigation.agentRadius = 0.9f;
    values.navigation.layerMask = 0b1u;
    CHECK_MESSAGE(static_cast<bool>(saveProjectSettingsValues(projectFile, values)), "navigation settings save");
    ProjectConfig withNavigation;
    CHECK_MESSAGE((loadProjectConfig(projectFile, withNavigation) && withNavigation.navigation &&
                   *withNavigation.navigation == values.navigation &&
                   withNavigation.renderSettings.maximumLocalLights == 79),
                  "navigation settings are saved after reading the project again");
    ProjectSettingsValues navigationReloaded;
    CHECK_MESSAGE((loadProjectSettingsValues(projectFile, navigationReloaded) &&
                   navigationReloaded.navigation == values.navigation),
                  "navigation settings read back");
    ProjectSettingsValues invalidNavigation = values;
    invalidNavigation.navigation.cellSize = 0.0f;
    CHECK_MESSAGE((!validateProjectSettings(invalidNavigation).empty() &&
                   !saveProjectSettingsValues(projectFile, invalidNavigation)),
                  "invalid navigation settings are refused");
    CHECK_MESSAGE((projectSettingsTab("navigation") == ProjectSettingsTab::Navigation && !projectSettingsTab("nav")),
                  "project.settings names the Navigation tab");
}

TEST_CASE("ProjectSettings: window") {
    ProjectSettingsWindow window;
    InputActionMap moduleActions;
    moduleActions.addAction("Jump", InputActionValueType::Button);
    moduleActions.addBinding("Jump", {.source = Key::Space});
    window.open({}, moduleActions);
    HeadlessImGuiContext imgui;
    const auto draw = [&window](bool canApply) {
        HeadlessImGuiFrame frame;
        return window.drawContents(canApply, canApply ? std::string{} : "Stop to apply");
    };
    for (const ProjectSettingsTab tab : {ProjectSettingsTab::PhysicsLayers, ProjectSettingsTab::InputActions,
                                         ProjectSettingsTab::Tags, ProjectSettingsTab::Navigation}) {
        window.setTab(tab);
        draw(true);
        draw(true);
        CHECK_MESSAGE(window.tab() == tab, "each tab draws");
    }
    CHECK_MESSAGE((!window.modified() && !draw(true)), "an unchanged draft offers nothing to apply");

    window.draft().tags.push_back("enemy");
    draw(true);
    CHECK_MESSAGE(window.modified(), "an edit marks the draft modified");
    const ImGuiWindow* frame = HeadlessImGuiContext::findWindow("Headless test");
    CHECK_MESSAGE(frame != nullptr, "the window frame exists");
    if (frame != nullptr) {
        HeadlessImGuiContext::activateNextFrame(HeadlessImGuiContext::itemId(*frame, "Apply"));
        CHECK_MESSAGE(!draw(false), "Apply does nothing while it cannot apply");
        HeadlessImGuiContext::activateNextFrame(HeadlessImGuiContext::itemId(*frame, "Apply"));
        const std::optional<ProjectSettingsValues> applied = draw(true);
        CHECK_MESSAGE((applied && applied->tags == std::vector<std::string>{"enemy"}), "Apply returns the draft");
    }
    window.finish("disk full");
    CHECK_MESSAGE(window.modified(), "a failed save keeps the draft modified");
    window.finish({});
    draw(true);
    CHECK_MESSAGE(!window.modified(), "a saved draft is no longer modified");
}

TEST_CASE("ProjectSettings: inspector names") {
    // A rigid body's layer is chosen by name from the project's layers.
    RigidBodyComponent body;
    body.layer = PhysicsLayer::Player;
    const ComponentType* type = builtInComponentTypes()->find("rigid_body");
    const EditorProjectNames names{.physicsLayers = {"Static", "Dynamic", "Player", "Pickups"}};
    ReflectedFieldEditorOptions options{.nameList = [&names](std::string_view list) { return names.list(list); }};
    HeadlessImGuiContext imgui;
    std::string text;
    {
        HeadlessImGuiFrame frame;
        ImGui::LogToBuffer();
        drawReflectedFields(&body, type->fields(), options);
        text = ImGui::GetCurrentContext()->LogBuffer.c_str();
        ImGui::LogFinish();
    }
    CHECK_MESSAGE(text.find("Player") != std::string::npos, "the layer field shows the layer's name");
    body.layer = PhysicsLayer{9};
    {
        HeadlessImGuiFrame frame;
        ImGui::LogToBuffer();
        drawReflectedFields(&body, type->fields(), options);
        text = ImGui::GetCurrentContext()->LogBuffer.c_str();
        ImGui::LogFinish();
    }
    CHECK_MESSAGE(text.find("#9 (not in the project)") != std::string::npos,
                  "a layer the project lacks shows its index");

    // The tags editor marks a tag the project does not declare.
    TagsComponent tags{.tags = {"enemy", "enemmy"}};
    const std::vector<std::string> projectTags{"enemy"};
    std::string otherTag;
    {
        HeadlessImGuiFrame frame;
        ImGui::LogToBuffer();
        ComponentEditors::drawTags(tags, projectTags, otherTag);
        text = ImGui::GetCurrentContext()->LogBuffer.c_str();
        ImGui::LogFinish();
    }
    const size_t misspelt = text.find("enemmy");
    CHECK_MESSAGE((misspelt != std::string::npos && text.find("(not a project tag)", misspelt) != std::string::npos &&
                   text.find("(not a project tag)") > misspelt),
                  "only the undeclared tag is marked");
}

// navigation.bake bakes the preview with the project's settings, read at each
// bake, so an applied change reaches the next one.
TEST_CASE("ProjectSettings: navigation bake uses settings") {
    EditorTestFixture fixture;
    NavigationWorld preview;
    NavMeshBuildSettings settings{.agentRadius = 0.7f};
    CHECK_MESSAGE(
        static_cast<bool>(registerNavigationOperators(fixture.operators, {.preview = preview, .settings = settings})),
        "the navigation operators register");
    const Entity floor = fixture.scene.createEntity("Floor");
    fixture.scene.registry().emplace<ColliderComponent>(
        floor, ColliderComponent{.shape = BoxCollider{.halfExtents = {5.0f, 0.5f, 5.0f}}});
    fixture.scene.registry().emplace<RigidBodyComponent>(floor);
    EditorContext editor = fixture.context();
    CHECK_MESSAGE(
        (static_cast<bool>(editor.invoke("navigation.bake")) && preview.baked() && preview.settings() == settings),
        "navigation.bake uses the project's settings");
    settings.agentRadius = 0.3f;
    CHECK_MESSAGE((static_cast<bool>(editor.invoke("navigation.bake")) && preview.settings().agentRadius == 0.3f),
                  "a settings change applies to the next bake");

    // The preview is the level's: an operator context editing another
    // document (a prefab window's) neither bakes nor clears it.
    Scene prefabScene;
    EditorDocument prefabDocument(fixture.document.options().assetRoot);
    CommandHistory prefabHistory;
    Selection prefabSelection;
    OperatorContext prefab{.editing = {prefabScene, fixture.assets, prefabDocument, prefabHistory, fixture.diagnostics},
                           .selection = prefabSelection,
                           .mode = EditorMode::Edit,
                           .app = &fixture.app};
    CHECK_MESSAGE((fixture.runner.invokeWithJson("navigation.bake", prefab, "{}").status != OperatorStatus::Finished &&
                   fixture.runner.invokeWithJson("navigation.clear", prefab, "{}").status != OperatorStatus::Finished &&
                   preview.baked()),
                  "a prefab window's document neither bakes nor clears the level's preview");
}

TEST_SUITE_END();
