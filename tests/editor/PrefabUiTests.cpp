// Headless tests of the prefab UI: override highlighting and Revert to Prefab
// in the inspector, the instance header's actions, Add > Prefab, asset drops,
// and Select Prefab Asset. Every flow runs through the prefab operators.

#include "EditorTestFixture.hpp"
#include "HeadlessImGui.hpp"

#include "editor/document/PrefabAuthoring.hpp"
#include "editor/interaction/ReflectedFieldEditor.hpp"
#include "editor/menus/DefaultMenus.hpp"
#include "editor/operators/ObjectOperators.hpp"
#include "editor/operators/PrefabOperators.hpp"
#include "editor/panels/InspectorPanel.hpp"
#include "editor/project/ProjectAssets.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/Prefab.hpp"

#include "support/Doctest.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using testsupport::readText;

TEST_SUITE_BEGIN("PrefabUi");

namespace {
struct SpringSettings {
    float stiffness{1.0f};
    float damping{0.5f};
};

// A module component with a nested struct, to address nested fields.
struct SpringComponent {
    float length{2.0f};
    SpringSettings settings;
};

std::shared_ptr<const ComponentTypeRegistry> componentTypes() {
    auto types = std::make_shared<ComponentTypeRegistry>();
    CHECK_MESSAGE(registerBuiltInComponentTypes(*types).success, "built-in component types should register");
    CHECK_MESSAGE(types
                      ->add(ComponentTypeBuilder<SpringComponent>("test_spring", "Spring")
                                .fields(Fields<SpringComponent>()
                                            .add("length", &SpringComponent::length)
                                            .structure("settings", &SpringComponent::settings,
                                                       Fields<SpringSettings>()
                                                           .add("stiffness", &SpringSettings::stiffness)
                                                           .add("damping", &SpringSettings::damping)))
                                .build())
                      .success,
                  "the spring component should register");
    return types;
}

// The objects an editor session owns, without a window or GPU, editing a
// project under a fresh asset root (the fixture's scratch directory).
struct Fixture : EditorTestFixture {
    std::filesystem::path root;
    std::vector<std::filesystem::path> revealed;
    std::vector<std::filesystem::path> prefabFiles;

    Fixture()
        : root(document.options().assetRoot) {
        document.setComponentTypes(componentTypes());
        CHECK_MESSAGE(registerObjectOperators(operators).success, "the object operators should register");
        CHECK_MESSAGE(
            registerPrefabOperators(
                operators, {.revealAsset = [this](const std::filesystem::path& path) { revealed.push_back(path); }})
                .success,
            "the prefab operators should register");
        CHECK_MESSAGE(
            registerDefaultMenus(menus.registry(), {.projectPrefabs = [this] { return prefabFiles; }}).success,
            "the default menus should register");
    }

    void edit(const std::function<void()>& change) {
        editing().perform("Edit", {}, [&change] {
            change();
            return SceneResult{.success = true};
        });
    }

    Entity find(SceneEntityId id) const {
        return scene.findEntity(id);
    }

    // A lamp with a light and a spring, made into props/lamp.prefab, and a
    // second instance of it, which is selected.
    SceneEntityId authorInstance() {
        const Entity lamp = scene.createEntity("Lamp");
        scene.registry().emplace<PointLightComponent>(lamp, glm::vec3(1.0f), 10.0f, 4.0f);
        scene.registry().emplace<SpringComponent>(lamp);
        selection.selectOnly(scene.id(lamp));
        CHECK_MESSAGE(invoke("prefab.create_from_selection", R"({"path":"props/lamp.prefab"})").status ==
                          OperatorStatus::Finished,
                      "the lamp prefab should be created");
        prefabFiles.push_back(root / "props" / "lamp.prefab");
        selection.clear();
        CHECK_MESSAGE(invoke("prefab.instantiate", R"({"asset":"props/lamp.prefab"})").status ==
                          OperatorStatus::Finished,
                      "the lamp prefab should instantiate");
        return *selection.active();
    }
};

// Override bars drawn last frame: rectangles in the override accent.
size_t overrideBars() {
    const ImU32 bar = ImGui::GetColorU32(toImVec4(palette::prefabOverride));
    size_t vertices = 0;
    const ImDrawData* data = ImGui::GetDrawData();
    for (int list = 0; list < data->CmdListsCount; ++list) {
        for (const ImDrawVert& vertex : data->CmdLists[list]->VtxBuffer) {
            vertices += vertex.col == bar ? 1 : 0;
        }
    }
    return vertices / 4;
}

const std::vector<FieldDescriptor>& springFields() {
    static const std::vector<FieldDescriptor> fields =
        std::move(Fields<SpringComponent>()
                      .add("length", &SpringComponent::length)
                      .structure("settings", &SpringComponent::settings,
                                 Fields<SpringSettings>()
                                     .add("stiffness", &SpringSettings::stiffness)
                                     .add("damping", &SpringSettings::damping)))
            .build();
    return fields;
}

bool contains(const std::vector<std::string>& values, std::string_view value) {
    return std::ranges::find(values, value) != values.end();
}

// Activates the item labelled label in the open popup menu next frame.
void activateMenuItem(const char* label) {
    // The open popup: an earlier menu's window stays in the list, closed.
    const ImGuiWindow* popup = nullptr;
    for (const ImGuiWindow* window : ImGui::GetCurrentContext()->Windows) {
        if (std::string_view(window->Name).find("##Popup_") != std::string_view::npos && window->Active) {
            popup = window;
        }
    }
    CHECK_MESSAGE(popup != nullptr, "a popup menu should be open");
    if (popup != nullptr) {
        HeadlessImGuiContext::activateNextFrame(HeadlessImGuiContext::itemId(*popup, label));
    }
}

} // namespace

TEST_CASE("PrefabUi: field highlight") {
    HeadlessImGuiContext imgui;
    const PrefabEntityOverrides overrides{
        .linked = true, .components = {{.component = "test_spring", .fields = {"/settings/damping"}}}};
    SpringComponent spring;
    std::vector<std::string> queried;
    std::vector<std::string> reverted;
    ReflectedFieldEditorOptions options;
    options.overridden = [&](std::string_view pointer) {
        queried.emplace_back(pointer);
        return overrides.overridden("test_spring", pointer);
    };
    options.revertOverride = [&](std::string_view pointer) { reverted.emplace_back(pointer); };
    std::optional<ImGuiID> openPopup;
    const auto draw = [&] {
        HeadlessImGuiFrame frame;
        const ImGuiWindow& window = *ImGui::GetCurrentWindow();
        // Struct fields are tree nodes; open the one the override is in.
        ImGui::GetStateStorage()->SetInt(HeadlessImGuiContext::itemId(window, "settings", "Settings"), 1);
        if (openPopup) {
            // As a right click on the item does.
            ImGui::OpenPopupEx(*openPopup);
            openPopup.reset();
        }
        drawReflectedFields(&spring, springFields(), options);
    };
    draw();
    CHECK_MESSAGE((contains(queried, "/length") && contains(queried, "/settings") &&
                   contains(queried, "/settings/stiffness") && contains(queried, "/settings/damping")),
                  "fields, including nested struct fields, should be queried by their full JSON pointers");
    CHECK_MESSAGE(overrideBars() == 2, "the overridden nested field and its struct should be marked, and nothing else");

    openPopup = HeadlessImGuiContext::itemId(*HeadlessImGuiContext::findWindow("Headless test"), "settings", "Settings",
                                             "damping", "##FieldMenu");
    draw();
    activateMenuItem("Revert to Prefab");
    draw();
    CHECK_MESSAGE((reverted.size() == 1 && reverted.front() == "/settings/damping"),
                  "Revert to Prefab in a field's menu should revert that field's pointer");

    options.overridden = [](std::string_view) { return false; };
    draw();
    CHECK_MESSAGE(overrideBars() == 0, "fields that are not overridden should not be marked");

    // The same menu offers Watch Field when the view watches.
    std::vector<std::string> watched;
    options.watch = [&watched](std::string_view pointer, std::string_view label) {
        watched.push_back(std::string(pointer) + " " + std::string(label));
    };
    openPopup =
        HeadlessImGuiContext::itemId(*HeadlessImGuiContext::findWindow("Headless test"), "length", "##FieldMenu");
    draw();
    activateMenuItem("Watch Field");
    draw();
    CHECK_MESSAGE((watched.size() == 1 && watched.front().starts_with("/length ")),
                  "Watch Field in a field's menu should watch that field's pointer");
}

TEST_CASE("PrefabUi: inspector") {
    HeadlessImGuiContext imgui;
    Fixture fixture;
    const SceneEntityId instanceId = fixture.authorInstance();
    fixture.edit([&] {
        const Entity instance = fixture.find(instanceId);
        fixture.scene.registry().get<PointLightComponent>(instance).intensity = 77.0f;
        fixture.scene.registry().get<SpringComponent>(instance).settings.damping = 0.9f;
    });
    InspectorPanel panel(fixture.componentLayout);
    std::optional<ImGuiID> openPopup;
    const auto draw = [&] {
        HeadlessImGuiFrame frame;
        if (openPopup) {
            ImGui::OpenPopupEx(*openPopup);
            openPopup.reset();
        }
        EditorContext context = fixture.context();
        panel.draw(context);
    };
    draw();
    CHECK_MESSAGE(
        overrideBars() == 4,
        "the light and spring headers, the intensity field, and the spring's settings struct should be marked");
    const ImGuiWindow& window = *HeadlessImGuiContext::findWindow("Headless test");

    // Revert to Prefab from the intensity field's menu.
    openPopup = HeadlessImGuiContext::itemId(window, "point_light", "intensity", "##FieldMenu");
    draw();
    activateMenuItem("Revert to Prefab");
    draw();
    const auto instance = [&] { return fixture.find(instanceId); };
    CHECK_MESSAGE((fixture.scene.registry().get<PointLightComponent>(instance()).intensity == 10.0f &&
                   fixture.scene.registry().get<SpringComponent>(instance()).settings.damping == 0.9f),
                  "Revert to Prefab should reset only the field it was chosen on");
    draw();
    CHECK_MESSAGE(overrideBars() == 2, "after the revert only the spring header and its settings should be marked");

    // The instance header's actions.
    const auto press = [&](const char* label) {
        HeadlessImGuiContext::activateNextFrame(HeadlessImGuiContext::itemId(window, "##PrefabHeader", label));
        draw();
    };
    press("Revert");
    CHECK_MESSAGE(fixture.scene.registry().get<SpringComponent>(instance()).settings.damping == 0.5f,
                  "the header's Revert should reset the instance");
    press("Select Prefab Asset");
    CHECK_MESSAGE(
        (fixture.revealed.size() == 1 && fixture.revealed.front() == (fixture.root / "props" / "lamp.prefab")),
        "Select Prefab Asset should reveal the prefab file");
    fixture.edit([&] { fixture.scene.registry().get<SpringComponent>(instance()).length = 5.0f; });
    press("Apply");
    const std::vector<Entity> instances = prefabInstancesOf(fixture.scene, "props/lamp.prefab");
    CHECK_MESSAGE((instances.size() == 2 &&
                   std::ranges::all_of(instances,
                                       [&](Entity entity) {
                                           return fixture.scene.registry().get<SpringComponent>(entity).length == 5.0f;
                                       })),
                  "the header's Apply should write the prefab and update the other instance");
    press("Unpack");
    CHECK_MESSAGE(!fixture.scene.registry().all_of<PrefabInstanceComponent>(instance()),
                  "the header's Unpack should remove the link");
    draw();
    CHECK_MESSAGE(overrideBars() == 0, "an unpacked entity should show no overrides");
}

TEST_CASE("PrefabUi: add menu and drops") {
    const testsupport::TempDirectory elsewhere("prefab-ui-elsewhere");
    Fixture fixture;
    fixture.authorInstance();
    fixture.prefabFiles.push_back(elsewhere / "elsewhere.prefab");
    const std::optional<ResolvedMenu> menu = fixture.menus.resolve({.menu = addMenuId}, fixture.operatorContext());
    const MenuItem* group = nullptr;
    if (menu) {
        const auto found = std::ranges::find(menu->items, std::string("Prefab"), &MenuItem::label);
        group = found == menu->items.end() ? nullptr : &*found;
    }
    CHECK_MESSAGE(
        (group != nullptr && group->children.size() == 1 && group->children.front().label == "props/Lamp" &&
         group->children.front().operatorId == "prefab.instantiate"),
        "Add > Prefab should list the project's prefabs by display name and skip files outside the asset root");
    if (group != nullptr && !group->children.empty()) {
        const size_t before = prefabInstancesOf(fixture.scene, "props/lamp.prefab").size();
        CHECK_MESSAGE((fixture.invoke(group->children.front().operatorId, group->children.front().properties).status ==
                           OperatorStatus::Finished &&
                       prefabInstancesOf(fixture.scene, "props/lamp.prefab").size() == before + 1),
                      "the Add > Prefab entry should instantiate its prefab");
    }

    // An Asset Browser drop carries the absolute path.
    const Entity holder = fixture.scene.createEntity("Holder");
    const SceneEntityId holderId = fixture.scene.id(holder);
    EditorContext context = fixture.context();
    context.placePrefab(fixture.root / "props" / "lamp.prefab", holder);
    const Entity dropped = fixture.find(*fixture.selection.active());
    CHECK_MESSAGE((dropped != entt::null && fixture.scene.registry().all_of<PrefabInstanceComponent>(dropped) &&
                   fixture.scene.registry().get<RelationshipComponent>(dropped).parent == fixture.find(holderId)),
                  "dropping a prefab on an outliner row should instantiate it under that entity");
    context.placePrefab(fixture.root / "props" / "lamp.prefab", entt::null);
    CHECK_MESSAGE(
        (fixture.scene.registry().get<RelationshipComponent>(fixture.find(*fixture.selection.active())).parent ==
         entt::null),
        "dropping a prefab in a viewport should instantiate it at the scene root");

    // Create from Selection (Object menu, F3) needs no path.
    const Entity crate = fixture.scene.createEntity("My:Crate");
    fixture.selection.selectOnly(fixture.scene.id(crate));
    CHECK_MESSAGE((fixture.invoke("prefab.create_from_selection").status == OperatorStatus::Finished &&
                   std::filesystem::is_regular_file(fixture.root / "prefabs" / "My_Crate.prefab")),
                  "Create from Selection should pick a prefab path from the entity name");
    const Entity second = fixture.scene.createEntity("My:Crate");
    fixture.selection.selectOnly(fixture.scene.id(second));
    CHECK_MESSAGE((fixture.invoke("prefab.create_from_selection").status == OperatorStatus::Finished &&
                   std::filesystem::is_regular_file(fixture.root / "prefabs" / "My_Crate 2.prefab")),
                  "a suggested path should not overwrite an existing prefab");
}

// A name that is not portable ASCII keeps its display name beside the
// reduced file name.
TEST_CASE("PrefabUi: display name") {
    HeadlessImGuiContext imgui;
    Fixture fixture;
    const std::string name = "L\xC3\xA4mp: \xE2\x9C\xA8";
    const Entity lamp = fixture.scene.createEntity(name);
    const SceneEntityId lampId = fixture.scene.id(lamp);
    fixture.selection.selectOnly(lampId);
    CHECK_MESSAGE(fixture.invoke("prefab.create_from_selection").status == OperatorStatus::Finished,
                  "a prefab should be created from a name that is not portable");
    const std::filesystem::path file = fixture.root / "prefabs" / "L__mp_ ___.prefab";
    PrefabDocument document;
    CHECK_MESSAGE((std::filesystem::is_regular_file(file) && loadPrefab(file, document).success &&
                   document.displayName == name && readText(file).find("\"display_name\"") != std::string::npos),
                  "the file name should be portable and the document should keep the display name");
    fixture.prefabFiles.push_back(file);

    PrefabAuthoring authoring(fixture.assets, fixture.document.options());
    PrefabEntityOverrides overrides;
    CHECK_MESSAGE((authoring.overrides(fixture.scene, lamp, overrides).success && overrides.displayName == name),
                  "the instance header should show the display name");
    InspectorPanel panel(fixture.componentLayout);
    {
        HeadlessImGuiFrame frame;
        EditorContext context = fixture.context();
        panel.draw(context);
    }
    const std::optional<ResolvedMenu> menu = fixture.menus.resolve({.menu = addMenuId}, fixture.operatorContext());
    const auto group = menu ? std::ranges::find(menu->items, std::string("Prefab"), &MenuItem::label)
                            : std::vector<MenuItem>::const_iterator{};
    CHECK_MESSAGE((menu && group != menu->items.end() && group->children.size() == 1 &&
                   group->children.front().label == "prefabs/" + name),
                  "Add > Prefab should show the display name");

    // Applying a renamed instance keeps the prefab's display name.
    fixture.edit([&] { fixture.scene.registry().get<NameComponent>(fixture.find(lampId)).name = "Renamed"; });
    CHECK_MESSAGE((fixture.invoke("prefab.apply").status == OperatorStatus::Finished &&
                   loadPrefab(file, document).success && document.displayName == name),
                  "apply should keep the display name");
    PrefabDocument legacy{.entities = document.entities};
    std::string encoded;
    CHECK_MESSAGE((serializePrefab(legacy, encoded).success && encoded.find("metadata") == std::string::npos),
                  "a prefab without a display name should not write metadata");
}

// Override highlighting on a 200-entity instance while a value is dragged:
// the old cache compared the whole instance on every revision; the cache
// now compares only the shown entity until the edits settle.
TEST_CASE("PrefabUi: override cache cost") {
    Fixture fixture;
    Scene& scene = fixture.scene;
    const Entity root = scene.createEntity("Big");
    std::vector<SceneEntityId> children;
    for (int index = 1; index < 200; ++index) {
        const Entity child = scene.createEntity("Part " + std::to_string(index));
        scene.registry().emplace<PointLightComponent>(child, glm::vec3(1.0f), 10.0f, 4.0f);
        scene.registry().emplace<SpringComponent>(child);
        scene.setParent(child, root);
        children.push_back(scene.id(child));
    }
    PrefabAuthoring authoring(fixture.assets, fixture.document.options());
    CHECK_MESSAGE(authoring.create(scene, root, "big.prefab").success, "the 200-entity prefab should be created");
    const SceneEntityId shownId = children[100];
    const auto drag = [&](int frame) {
        scene.registry().get<PointLightComponent>(fixture.find(shownId)).intensity = 10.0f + static_cast<float>(frame);
    };
    constexpr int frames = 20;
    using Clock = std::chrono::steady_clock;
    const auto millisecondsPerFrame = [](Clock::duration total) {
        return std::chrono::duration<double, std::milli>(total).count() / frames;
    };

    // Before: every revision compared the whole instance.
    const Clock::time_point fullStart = Clock::now();
    PrefabEntityOverrides full;
    for (int frame = 0; frame < frames; ++frame) {
        drag(frame);
        CHECK_MESSAGE(authoring.overrides(scene, fixture.find(shownId), full).success,
                      "the full comparison should succeed");
    }
    const double fullCost = millisecondsPerFrame(Clock::now() - fullStart);

    // After: one full comparison, then one entity per revision.
    PrefabOverrideCache cache(std::chrono::hours(1));
    uint64_t revision = 1;
    cache.get(authoring, scene, shownId, revision);
    const Clock::time_point cachedStart = Clock::now();
    for (int frame = 0; frame < frames; ++frame) {
        drag(frame);
        cache.get(authoring, scene, shownId, ++revision);
    }
    const double cachedCost = millisecondsPerFrame(Clock::now() - cachedStart);
    std::cout << "Override highlighting, 200-entity instance: " << fullCost << " ms per drag frame before, "
              << cachedCost << " ms after\n";
    const PrefabEntityOverrides& dragged = cache.get(authoring, scene, shownId, revision);
    CHECK_MESSAGE((cache.partial() && dragged.overridden("point_light", "/intensity") &&
                   dragged.instanceOverrides == full.instanceOverrides),
                  "the per-entity update should highlight what a full comparison does");

    // Another entity edited in the same step is counted once edits settle.
    scene.registry().get<PointLightComponent>(fixture.find(children[3])).range = 9.0f;
    PrefabOverrideCache settling(std::chrono::milliseconds(0));
    settling.get(authoring, scene, shownId, revision);
    scene.registry().get<PointLightComponent>(fixture.find(children[4])).range = 9.0f;
    CHECK_MESSAGE((settling.get(authoring, scene, shownId, revision + 1).instanceOverrides == 2 && settling.partial()),
                  "an edit should update only the shown entity at first");
    CHECK_MESSAGE((settling.get(authoring, scene, shownId, revision + 1).instanceOverrides == 3 && !settling.partial()),
                  "once edits settle the whole instance should be compared again");
    PrefabEntityOverrides other;
    CHECK_MESSAGE((settling.get(authoring, scene, children[3], revision + 1).overridden("point_light", "/range") &&
                   authoring.overrides(scene, fixture.find(children[3]), other).success &&
                   other.instanceOverrides == 3),
                  "another entity of the instance should reuse the instance's overrides");
}

TEST_CASE("PrefabUi: reveal request") {
    const testsupport::TempDirectory directory("prefab-ui-reveal");
    const std::filesystem::path& root = directory.path();
    std::ofstream(root / "door.prefab") << R"({"version":2,"entities":[],"nested":[]})";
    EditorDiagnostics diagnostics;
    ProjectAssets project(root, root, root / "settings" / "asset-browser.json",
                          AssetCatalogOptions{.watchFiles = false});
    project.refresh(diagnostics);
    CHECK_MESSAGE(
        std::ranges::any_of(project.catalog().assets(),
                            [](const EditorAssetRecord& asset) { return asset.kind == EditorAssetKind::Prefab; }),
        "the asset catalog should list .prefab files as prefabs");
    const uint64_t primary = project.registerView();
    const uint64_t secondary = project.registerView();
    CHECK_MESSAGE(project.claimPrimaryView(primary), "the first view should become the primary");
    project.requestReveal(root / "door.prefab");
    CHECK_MESSAGE(!project.takeRevealRequest(secondary), "only the primary browser should take a reveal");
    const std::optional<std::filesystem::path> reveal = project.takeRevealRequest(primary);
    CHECK_MESSAGE((reveal && *reveal == root / "door.prefab" && !project.takeRevealRequest(primary)),
                  "the primary browser should take a reveal once");
}

TEST_SUITE_END();
