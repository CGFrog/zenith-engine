// Headless tests of the prefab UI: override highlighting and Revert to Prefab
// in the inspector, the instance header's actions, Add > Prefab, asset drops,
// and Select Prefab Asset. Every flow runs through the prefab operators.

#include "HeadlessImGui.hpp"
#include "engine/diagnostics/Logger.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/CommandHistory.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/document/PrefabAuthoring.hpp"
#include "editor/interaction/ReflectedFieldEditor.hpp"
#include "editor/menus/DefaultMenus.hpp"
#include "editor/menus/EditorMenus.hpp"
#include "editor/operators/ObjectOperators.hpp"
#include "editor/operators/OperatorRegistry.hpp"
#include "editor/operators/OperatorRunner.hpp"
#include "editor/operators/PrefabOperators.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/panels/InspectorPanel.hpp"
#include "editor/project/ProjectAssets.hpp"
#include "editor/settings/ComponentInspectorLayout.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/audio/AudioSystem.hpp"
#include "engine/physics/PhysicsWorld.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/Prefab.hpp"
#include "engine/ui/GameUi.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

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
    expect(registerBuiltInComponentTypes(*types).success, "built-in component types should register");
    expect(types
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

std::filesystem::path freshDirectory(const std::string& name) {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / ("zenith-prefab-ui-" + name);
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory);
    return directory;
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

// The session workflows a panel triggers, reduced to running operators.
class OperatorActions final : public EditorSessionActions {
  public:
    OperatorActions(OperatorRunner& runner, std::function<OperatorContext()> context)
        : m_runner(runner)
        , m_context(std::move(context)) {}

    OperatorResult invokeOperator(ApplicationContext&, std::string_view operatorId, std::string_view properties,
                                  KeymapRegion) override {
        OperatorContext context = m_context();
        return m_runner.invokeWithJson(operatorId, context, properties);
    }

    bool operatorAvailable(ApplicationContext&, std::string_view) override {
        return true;
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

  private:
    OperatorRunner& m_runner;
    std::function<OperatorContext()> m_context;
};

// The objects an editor session owns, without a window or GPU, editing a
// project under a fresh asset root.
struct Fixture {
    std::filesystem::path root;
    Scene scene;
    AssetManager assets;
    PhysicsWorld physics;
    AudioSystem audio;
    ApplicationControl control;
    RenderRuntime rendering;
    GameUiContext gameUi;
    Logger logger;
    ApplicationContext app{scene, assets, physics, audio, control, rendering, gameUi, logger};
    EditorDocument document;
    CommandHistory history;
    EditorDiagnostics diagnostics;
    Selection selection;
    ComponentInspectorLayout componentLayout;
    OperatorRegistry operators;
    OperatorRunner runner{operators, history};
    OperatorActions actions{runner, [this] { return operatorContext(); }};
    EditorMenus menus;
    std::vector<std::filesystem::path> revealed;
    std::vector<std::filesystem::path> prefabFiles;

    explicit Fixture(const std::string& name)
        : root(freshDirectory(name))
        , document(root) {
        document.setComponentTypes(componentTypes());
        expect(registerObjectOperators(operators).success, "the object operators should register");
        expect(registerPrefabOperators(
                   operators, {.revealAsset = [this](const std::filesystem::path& path) { revealed.push_back(path); }})
                   .success,
               "the prefab operators should register");
        expect(registerDefaultMenus(menus.registry(), {.projectPrefabs = [this] { return prefabFiles; }}).success,
               "the default menus should register");
    }

    SceneEditing editing() {
        return {scene, assets, document, history, diagnostics};
    }

    OperatorContext operatorContext() {
        return {.editing = editing(), .selection = selection, .mode = EditorMode::Edit};
    }

    EditorContext context() {
        return {.app = app,
                .editing = editing(),
                .selection = selection,
                .diagnostics = diagnostics,
                .mode = EditorMode::Edit,
                .actions = actions,
                .menus = menus};
    }

    OperatorResult invoke(std::string_view id, std::string_view properties = {}) {
        OperatorContext context = operatorContext();
        return runner.invokeWithJson(id, context, properties);
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
        expect(invoke("prefab.create_from_selection", R"({"path":"props/lamp.prefab"})").status ==
                   OperatorStatus::Finished,
               "the lamp prefab should be created");
        prefabFiles.push_back(root / "props" / "lamp.prefab");
        selection.clear();
        expect(invoke("prefab.instantiate", R"({"asset":"props/lamp.prefab"})").status == OperatorStatus::Finished,
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
    const ImGuiWindow* popup = HeadlessImGuiContext::findWindow("##Popup_");
    expect(popup != nullptr, "a popup menu should be open");
    if (popup != nullptr) {
        HeadlessImGuiContext::activateNextFrame(HeadlessImGuiContext::itemId(*popup, label));
    }
}

void testFieldHighlight() {
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
    expect(contains(queried, "/length") && contains(queried, "/settings") && contains(queried, "/settings/stiffness") &&
               contains(queried, "/settings/damping"),
           "fields, including nested struct fields, should be queried by their full JSON pointers");
    expect(overrideBars() == 2, "the overridden nested field and its struct should be marked, and nothing else");

    openPopup = HeadlessImGuiContext::itemId(*HeadlessImGuiContext::findWindow("Headless test"), "settings", "Settings",
                                             "damping", "##PrefabOverride");
    draw();
    activateMenuItem("Revert to Prefab");
    draw();
    expect(reverted.size() == 1 && reverted.front() == "/settings/damping",
           "Revert to Prefab in a field's menu should revert that field's pointer");

    options.overridden = [](std::string_view) { return false; };
    draw();
    expect(overrideBars() == 0, "fields that are not overridden should not be marked");
}

void testInspector() {
    HeadlessImGuiContext imgui;
    Fixture fixture("inspector");
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
    expect(overrideBars() == 4,
           "the light and spring headers, the intensity field, and the spring's settings struct should be marked");
    const ImGuiWindow& window = *HeadlessImGuiContext::findWindow("Headless test");

    // Revert to Prefab from the intensity field's menu.
    openPopup = HeadlessImGuiContext::itemId(window, "point_light", "intensity", "##PrefabOverride");
    draw();
    activateMenuItem("Revert to Prefab");
    draw();
    const auto instance = [&] { return fixture.find(instanceId); };
    expect(fixture.scene.registry().get<PointLightComponent>(instance()).intensity == 10.0f &&
               fixture.scene.registry().get<SpringComponent>(instance()).settings.damping == 0.9f,
           "Revert to Prefab should reset only the field it was chosen on");
    draw();
    expect(overrideBars() == 2, "after the revert only the spring header and its settings should be marked");

    // The instance header's actions.
    const auto press = [&](const char* label) {
        HeadlessImGuiContext::activateNextFrame(HeadlessImGuiContext::itemId(window, "##PrefabHeader", label));
        draw();
    };
    press("Revert");
    expect(fixture.scene.registry().get<SpringComponent>(instance()).settings.damping == 0.5f,
           "the header's Revert should reset the instance");
    press("Select Prefab Asset");
    expect(fixture.revealed.size() == 1 && fixture.revealed.front() == (fixture.root / "props" / "lamp.prefab"),
           "Select Prefab Asset should reveal the prefab file");
    fixture.edit([&] { fixture.scene.registry().get<SpringComponent>(instance()).length = 5.0f; });
    press("Apply");
    const std::vector<Entity> instances = prefabInstancesOf(fixture.scene, "props/lamp.prefab");
    expect(instances.size() == 2 &&
               std::ranges::all_of(
                   instances,
                   [&](Entity entity) { return fixture.scene.registry().get<SpringComponent>(entity).length == 5.0f; }),
           "the header's Apply should write the prefab and update the other instance");
    press("Unpack");
    expect(!fixture.scene.registry().all_of<PrefabInstanceComponent>(instance()),
           "the header's Unpack should remove the link");
    draw();
    expect(overrideBars() == 0, "an unpacked entity should show no overrides");
}

void testAddMenuAndDrops() {
    Fixture fixture("menu");
    fixture.authorInstance();
    fixture.prefabFiles.push_back(std::filesystem::temp_directory_path() / "elsewhere.prefab");
    const std::optional<ResolvedMenu> menu = fixture.menus.resolve({.menu = addMenuId}, fixture.operatorContext());
    const MenuItem* group = nullptr;
    if (menu) {
        const auto found = std::ranges::find(menu->items, std::string("Prefab"), &MenuItem::label);
        group = found == menu->items.end() ? nullptr : &*found;
    }
    expect(group != nullptr && group->children.size() == 1 && group->children.front().label == "props/Lamp" &&
               group->children.front().operatorId == "prefab.instantiate",
           "Add > Prefab should list the project's prefabs by display name and skip files outside the asset root");
    if (group != nullptr && !group->children.empty()) {
        const size_t before = prefabInstancesOf(fixture.scene, "props/lamp.prefab").size();
        expect(fixture.invoke(group->children.front().operatorId, group->children.front().properties).status ==
                       OperatorStatus::Finished &&
                   prefabInstancesOf(fixture.scene, "props/lamp.prefab").size() == before + 1,
               "the Add > Prefab entry should instantiate its prefab");
    }

    // An Asset Browser drop carries the absolute path.
    const Entity holder = fixture.scene.createEntity("Holder");
    const SceneEntityId holderId = fixture.scene.id(holder);
    EditorContext context = fixture.context();
    context.placePrefab(fixture.root / "props" / "lamp.prefab", holder);
    const Entity dropped = fixture.find(*fixture.selection.active());
    expect(dropped != entt::null && fixture.scene.registry().all_of<PrefabInstanceComponent>(dropped) &&
               fixture.scene.registry().get<RelationshipComponent>(dropped).parent == fixture.find(holderId),
           "dropping a prefab on an outliner row should instantiate it under that entity");
    context.placePrefab(fixture.root / "props" / "lamp.prefab", entt::null);
    expect(fixture.scene.registry().get<RelationshipComponent>(fixture.find(*fixture.selection.active())).parent ==
               entt::null,
           "dropping a prefab in a viewport should instantiate it at the scene root");

    // Create from Selection (Object menu, F3) needs no path.
    const Entity crate = fixture.scene.createEntity("My:Crate");
    fixture.selection.selectOnly(fixture.scene.id(crate));
    expect(fixture.invoke("prefab.create_from_selection").status == OperatorStatus::Finished &&
               std::filesystem::is_regular_file(fixture.root / "prefabs" / "My_Crate.prefab"),
           "Create from Selection should pick a prefab path from the entity name");
    const Entity second = fixture.scene.createEntity("My:Crate");
    fixture.selection.selectOnly(fixture.scene.id(second));
    expect(fixture.invoke("prefab.create_from_selection").status == OperatorStatus::Finished &&
               std::filesystem::is_regular_file(fixture.root / "prefabs" / "My_Crate 2.prefab"),
           "a suggested path should not overwrite an existing prefab");
}

// A name that is not portable ASCII keeps its display name beside the
// reduced file name.
void testDisplayName() {
    HeadlessImGuiContext imgui;
    Fixture fixture("display-name");
    const std::string name = "L\xC3\xA4mp: \xE2\x9C\xA8";
    const Entity lamp = fixture.scene.createEntity(name);
    const SceneEntityId lampId = fixture.scene.id(lamp);
    fixture.selection.selectOnly(lampId);
    expect(fixture.invoke("prefab.create_from_selection").status == OperatorStatus::Finished,
           "a prefab should be created from a name that is not portable");
    const std::filesystem::path file = fixture.root / "prefabs" / "L__mp_ ___.prefab";
    PrefabDocument document;
    expect(std::filesystem::is_regular_file(file) && loadPrefab(file, document).success &&
               document.displayName == name && readText(file).find("\"display_name\"") != std::string::npos,
           "the file name should be portable and the document should keep the display name");
    fixture.prefabFiles.push_back(file);

    PrefabAuthoring authoring(fixture.assets, fixture.document.options());
    PrefabEntityOverrides overrides;
    expect(authoring.overrides(fixture.scene, lamp, overrides).success && overrides.displayName == name,
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
    expect(menu && group != menu->items.end() && group->children.size() == 1 &&
               group->children.front().label == "prefabs/" + name,
           "Add > Prefab should show the display name");

    // Applying a renamed instance keeps the prefab's display name.
    fixture.edit([&] { fixture.scene.registry().get<NameComponent>(fixture.find(lampId)).name = "Renamed"; });
    expect(fixture.invoke("prefab.apply").status == OperatorStatus::Finished && loadPrefab(file, document).success &&
               document.displayName == name,
           "apply should keep the display name");
    PrefabDocument legacy{.entities = document.entities};
    std::string encoded;
    expect(serializePrefab(legacy, encoded).success && encoded.find("metadata") == std::string::npos,
           "a prefab without a display name should not write metadata");
}

// Override highlighting on a 200-entity instance while a value is dragged:
// the old cache compared the whole instance on every revision; the cache
// now compares only the shown entity until the edits settle.
void testOverrideCacheCost() {
    Fixture fixture("cache");
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
    expect(authoring.create(scene, root, "big.prefab").success, "the 200-entity prefab should be created");
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
        expect(authoring.overrides(scene, fixture.find(shownId), full).success, "the full comparison should succeed");
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
    expect(cache.partial() && dragged.overridden("point_light", "/intensity") &&
               dragged.instanceOverrides == full.instanceOverrides,
           "the per-entity update should highlight what a full comparison does");

    // Another entity edited in the same step is counted once edits settle.
    scene.registry().get<PointLightComponent>(fixture.find(children[3])).range = 9.0f;
    PrefabOverrideCache settling(std::chrono::milliseconds(0));
    settling.get(authoring, scene, shownId, revision);
    scene.registry().get<PointLightComponent>(fixture.find(children[4])).range = 9.0f;
    expect(settling.get(authoring, scene, shownId, revision + 1).instanceOverrides == 2 && settling.partial(),
           "an edit should update only the shown entity at first");
    expect(settling.get(authoring, scene, shownId, revision + 1).instanceOverrides == 3 && !settling.partial(),
           "once edits settle the whole instance should be compared again");
    PrefabEntityOverrides other;
    expect(settling.get(authoring, scene, children[3], revision + 1).overridden("point_light", "/range") &&
               authoring.overrides(scene, fixture.find(children[3]), other).success && other.instanceOverrides == 3,
           "another entity of the instance should reuse the instance's overrides");
}

void testRevealRequest() {
    const std::filesystem::path root = freshDirectory("reveal");
    std::ofstream(root / "door.prefab") << R"({"version":2,"entities":[],"nested":[]})";
    EditorDiagnostics diagnostics;
    ProjectAssets project(root, root, root / "settings" / "asset-browser.json",
                          AssetCatalogOptions{.watchFiles = false});
    project.refresh(diagnostics);
    expect(std::ranges::any_of(project.catalog().assets(),
                               [](const EditorAssetRecord& asset) { return asset.kind == EditorAssetKind::Prefab; }),
           "the asset catalog should list .prefab files as prefabs");
    const uint64_t primary = project.registerView();
    const uint64_t secondary = project.registerView();
    expect(project.claimPrimaryView(primary), "the first view should become the primary");
    project.requestReveal(root / "door.prefab");
    expect(!project.takeRevealRequest(secondary), "only the primary browser should take a reveal");
    const std::optional<std::filesystem::path> reveal = project.takeRevealRequest(primary);
    expect(reveal && *reveal == root / "door.prefab" && !project.takeRevealRequest(primary),
           "the primary browser should take a reveal once");
}
} // namespace

int main() {
    testFieldHighlight();
    testInspector();
    testAddMenuAndDrops();
    testDisplayName();
    testOverrideCacheCost();
    testRevealRequest();
    for (const char* name : {"inspector", "menu", "reveal", "display-name", "cache"}) {
        std::error_code error;
        std::filesystem::remove_all(std::filesystem::temp_directory_path() / ("zenith-prefab-ui-" + std::string(name)),
                                    error);
    }
    return failures == 0 ? 0 : 1;
}
