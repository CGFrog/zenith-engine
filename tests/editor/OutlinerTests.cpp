#include "HeadlessImGui.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/CommandHistory.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/interaction/EntityEyedropper.hpp"
#include "editor/interaction/EntityReferenceField.hpp"
#include "editor/menus/EditorMenus.hpp"
#include "editor/operators/ObjectOperators.hpp"
#include "editor/operators/OperatorRegistry.hpp"
#include "editor/operators/OperatorRunner.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/panels/HierarchyPanel.hpp"
#include "editor/panels/outliner/OrphanData.hpp"
#include "editor/panels/outliner/OutlinerTree.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/audio/AudioSystem.hpp"
#include "engine/diagnostics/Logger.hpp"
#include "engine/physics/PhysicsWorld.hpp"
#include "engine/scene/Prefab.hpp"
#include "engine/ui/GameUi.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The outliner: its display modes and filter as pure trees, orphan data, the
// operators behind its gestures, and the panel driven headlessly (area
// state, Ctrl/Shift clicks, restriction toggles, drags, and search).

namespace {
int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
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

// A scene exercising every display mode:
//
//   collections   Props (1) > Crates (2), and Empty (3)
//   Lamp          root, no collection
//     Bulb        in Crates
//   Crate         in Props
//     Lid         no collection
//   Barrel        in Props and Crates
//   Tree, Tree 2  instances of prefabs/tree.prefab
//   Rock          instance of prefabs/rock.prefab
struct Fixture {
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
    EntityEyedropper eyedropper;
    EditorMenus menus;
    OperatorRegistry operators;
    OperatorRunner runner{operators, history};
    OperatorActions actions{runner, [this] { return operatorContext(); }};
    HierarchyPanel panel;

    SceneCollectionId props;
    SceneCollectionId crates;
    SceneCollectionId empty;
    SceneEntityId lamp;
    SceneEntityId bulb;
    SceneEntityId crate;
    SceneEntityId lid;
    SceneEntityId barrel;
    SceneEntityId tree;
    SceneEntityId tree2;
    SceneEntityId rock;

    Fixture() {
        expect(registerObjectOperators(operators).success, "the object operators should register");
        SceneCollections collections;
        collections.create("Props", {}, props);
        collections.create("Crates", props, crates);
        collections.create("Empty", {}, empty);
        scene.setCollections(std::move(collections));
        lamp = add("Lamp");
        bulb = add("Bulb", lamp, {crates});
        crate = add("Crate", {}, {props});
        lid = add("Lid", crate);
        barrel = add("Barrel", {}, {props, crates});
        tree = addInstance("Tree", "prefabs/tree.prefab");
        tree2 = addInstance("Tree 2", "prefabs/tree.prefab");
        rock = addInstance("Rock", "prefabs/rock.prefab");
    }

    SceneEntityId add(const char* name, SceneEntityId parent = {}, std::vector<SceneCollectionId> linked = {}) {
        const Entity entity = scene.createEntity(name);
        if (parent) {
            scene.setParent(entity, scene.findEntity(parent));
        }
        for (const SceneCollectionId collection : linked) {
            expect(static_cast<bool>(linkToCollection(scene, entity, collection)), "linking should work");
        }
        return scene.id(entity);
    }

    SceneEntityId addInstance(const char* name, const char* prefab) {
        const SceneEntityId id = add(name);
        scene.registry().emplace<PrefabInstanceComponent>(scene.findEntity(id),
                                                          PrefabInstanceComponent{.prefab = prefab});
        return id;
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
                .eyedropper = &eyedropper,
                .menus = menus};
    }

    OperatorResult invoke(std::string_view operatorId, const nlohmann::json& properties) {
        OperatorContext operatorContext = this->operatorContext();
        return runner.invokeWithJson(operatorId, operatorContext, properties.dump());
    }

    void draw() {
        HeadlessImGuiFrame frame(panel.title());
        EditorContext editor = context();
        panel.draw(editor);
    }

    std::string name(SceneEntityId id) const {
        return scene.registry().get<NameComponent>(scene.findEntity(id)).name;
    }

    Entity parent(SceneEntityId id) const {
        return scene.registry().get<RelationshipComponent>(scene.findEntity(id)).parent;
    }

    bool member(SceneEntityId id, SceneCollectionId collection) const {
        const auto* membership = scene.registry().try_get<CollectionMembershipComponent>(scene.findEntity(id));
        return membership != nullptr && membership->contains(collection);
    }
};

// "Label[children]" per row, comma separated; "*" marks filter context rows.
std::string summary(std::span<const OutlinerNode> rows) {
    std::string text;
    for (const OutlinerNode& row : rows) {
        if (!text.empty()) {
            text += ',';
        }
        text += (row.filterContext ? "*" : "") + row.label;
        if (!row.children.empty()) {
            text += '[' + summary(row.children) + ']';
        }
    }
    return text;
}

std::string tree(const Scene& scene, OutlinerDisplayMode mode, std::string_view filter = {},
                 std::span<const OrphanAsset> orphans = {}) {
    return summary(buildOutlinerTree(scene, {.mode = mode, .filter = filter, .orphanAssets = orphans}));
}

void testDisplayModes() {
    Fixture fixture;
    const Scene& scene = fixture.scene;
    expect(tree(scene, OutlinerDisplayMode::ViewLayer) ==
               "Props[Crates[Bulb,Barrel],Crate[Lid],Barrel],Empty,Lamp[Bulb],Tree,Tree 2,Rock",
           "View Layer should group hierarchies by collection: " + tree(scene, OutlinerDisplayMode::ViewLayer));
    expect(tree(scene, OutlinerDisplayMode::Collections) ==
               "Props[Crates[Bulb,Barrel],Crate,Barrel],Empty,Lamp,Lid,Tree,Tree 2,Rock",
           "Collections should list members flat: " + tree(scene, OutlinerDisplayMode::Collections));
    expect(tree(scene, OutlinerDisplayMode::Prefabs) == "prefabs/rock.prefab[Rock],prefabs/tree.prefab[Tree,Tree 2]",
           "Prefabs should group instances by prefab: " + tree(scene, OutlinerDisplayMode::Prefabs));
    const std::vector<OrphanAsset> orphans{{.path = "textures/unused.png", .kind = EditorAssetKind::Texture}};
    expect(tree(scene, OutlinerDisplayMode::OrphanData, {}, orphans) ==
               "Unused Assets[textures/unused.png],Empty Collections[Empty]",
           "Orphan Data should list unused assets and empty collections: " +
               tree(scene, OutlinerDisplayMode::OrphanData, {}, orphans));

    // Each listing of an entity has its own scope, so its rows get distinct ids.
    const std::vector<OutlinerNode> viewLayer = buildOutlinerTree(scene, {.mode = OutlinerDisplayMode::ViewLayer});
    const OutlinerNode& bulbInCrates = viewLayer[0].children[0].children[0];
    const OutlinerNode& bulbUnderLamp = viewLayer[2].children[0];
    expect(bulbInCrates.entity == fixture.bulb && bulbUnderLamp.entity == fixture.bulb &&
               bulbInCrates.scope == fixture.crates.value && bulbUnderLamp.scope == 0,
           "an entity listed in a collection and under its parent should have two scopes");
    const std::vector<OutlinerNode> prefabs = buildOutlinerTree(scene, {.mode = OutlinerDisplayMode::Prefabs});
    expect(prefabs[0].children[0].scope != 0 && prefabs[0].children[0].scope != prefabs[1].children[0].scope,
           "prefab groups should scope their rows apart from the top level and each other");

    for (const OutlinerDisplayMode mode : outlinerDisplayModes) {
        expect(parseOutlinerDisplayMode(outlinerDisplayModeKey(mode)) == mode, "display mode keys should round trip");
    }
    expect(!parseOutlinerDisplayMode("blender_file"), "unknown display mode keys should be rejected");
}

void testFilter() {
    Fixture fixture;
    const Scene& scene = fixture.scene;
    expect(outlinerFilterMatches("Lamp Post", "") && outlinerFilterMatches("Lamp Post", "mP p") &&
               !outlinerFilterMatches("Lamp", "lamps"),
           "the filter should match substrings ignoring case");
    // A matching child shows with its non-matching ancestors, whatever the case.
    expect(tree(scene, OutlinerDisplayMode::ViewLayer, "BULB") == "*Props[*Crates[Bulb]],*Lamp[Bulb]",
           "matches should appear with their ancestors: " + tree(scene, OutlinerDisplayMode::ViewLayer, "BULB"));
    expect(tree(scene, OutlinerDisplayMode::ViewLayer, "lid") == "*Props[*Crate[Lid]]",
           "a child row should match case-insensitively: " + tree(scene, OutlinerDisplayMode::ViewLayer, "lid"));
    expect(tree(scene, OutlinerDisplayMode::Prefabs, "tree 2") == "*prefabs/tree.prefab[Tree 2]",
           "filters should apply in every mode");
    expect(tree(scene, OutlinerDisplayMode::ViewLayer, "nothing").empty(), "a filter matching nothing lists nothing");
}

void testOrphanData() {
    const AssetGuid moved(1, 2);
    const nlohmann::json document{
        {"version", 3},
        {"entities",
         {{{"id", 1},
           {"components",
            {{"mesh_renderer", {{"model", {{"path", "models/crate.gltf"}}}}},
             {"material", {{"layers", {{{"albedo", {{"guid", moved.toString()}, {"path", "models/old.gltf"}}}}}}}},
             {"name_tag", {{"text", "path"}}}}}}}}};
    SceneAssetUsage usage;
    std::string error;
    expect(collectSceneAssetUsage(document.dump(), usage, error) && usage.paths.contains("models/crate.gltf") &&
               usage.guids.contains(moved),
           "asset references should be found at any depth: " + error);
    expect(!collectSceneAssetUsage("{\"version\":3}", usage, error) && !error.empty(),
           "a document without entities should be rejected");
    collectSceneAssetUsage(document.dump(), usage, error);

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "zenith-outliner-tests" / "assets";
    const auto record = [&root](const char* path, EditorAssetKind kind, AssetGuid guid = {}) {
        return EditorAssetRecord{.relativePath = path, .absolutePath = root / path, .kind = kind, .guid = guid};
    };
    const std::vector<EditorAssetRecord> assets{
        record("models/crate.gltf", EditorAssetKind::Model),
        record("models/renamed.gltf", EditorAssetKind::Model, moved),
        record("textures/unused.png", EditorAssetKind::Texture),
        record("audio/hit.wav", EditorAssetKind::Audio),
        record("scenes/main.zenith-scene", EditorAssetKind::Scene),
        {.relativePath = "other.png",
         .absolutePath = root.parent_path() / "elsewhere" / "other.png",
         .kind = EditorAssetKind::Texture},
    };
    const std::vector<OrphanAsset> orphans = findOrphanAssets(assets, root, usage);
    expect(orphans.size() == 2 && orphans[0].path == "audio/hit.wav" && orphans[1].path == "textures/unused.png" &&
               orphans[1].kind == EditorAssetKind::Texture,
           "assets referenced by path or GUID, scenes, and files outside the root should not be orphans");

    Fixture fixture;
    expect(findEmptyCollections(fixture.scene) == std::vector<SceneCollectionId>{fixture.empty},
           "a collection with members in a nested collection should not be empty");
}

void testSetVisibilityOperator() {
    Fixture fixture;
    const auto visibility = [&fixture] {
        return fixture.scene.registry().try_get<VisibilityComponent>(fixture.scene.findEntity(fixture.rock));
    };
    fixture.selection.assign({fixture.rock, fixture.tree});
    const uint64_t renderable = fixture.scene.revisions().renderable;
    expect(static_cast<bool>(fixture.invoke("object.set_visibility",
                                            {{"entity", fixture.rock.value}, {"flag", "visible"}, {"value", false}})) &&
               visibility() != nullptr && visibility()->hidden,
           "hiding through the column should set the viewport hide");
    expect(fixture.selection.ids() == std::vector<SceneEntityId>{fixture.tree},
           "a hidden object should leave the selection");
    expect(fixture.scene.revisions().renderable != renderable, "a visibility change should invalidate render caches");
    expect(
        fixture.invoke("object.set_visibility", {{"entity", fixture.rock.value}, {"flag", "visible"}, {"value", false}})
                .status == OperatorStatus::Cancelled,
        "setting a flag to its value should change nothing");
    fixture.invoke("object.set_visibility", {{"entity", fixture.rock.value}, {"flag", "renderable"}, {"value", false}});
    expect(visibility() != nullptr && !visibility()->renderable, "the renderable column should set renderable");
    fixture.editing().undo();
    expect(visibility() != nullptr && visibility()->renderable && visibility()->hidden,
           "undo should restore the renderable flag only");
    fixture.editing().undo();
    expect(visibility() == nullptr, "undoing the hide should leave no visibility component");
    fixture.editing().redo();
    fixture.invoke("object.set_visibility", {{"entity", fixture.rock.value}, {"flag", "visible"}, {"value", true}});
    expect(visibility() == nullptr, "returning every flag to its default should remove the component");
    fixture.invoke("object.set_visibility", {{"entity", fixture.rock.value}, {"flag", "selectable"}, {"value", false}});
    expect(visibility() != nullptr && !visibility()->selectable && !visibility()->hidden,
           "the selectable column should set selectable");
    expect(fixture.invoke("object.set_visibility", {{"entity", 4242}, {"flag", "visible"}, {"value", false}}).status ==
               OperatorStatus::Failed,
           "an unknown object should fail");
    fixture.diagnostics.acknowledgeErrors();
}

void testParentDropOperator() {
    Fixture fixture;
    // Crate's child Lid is selected with it, so it stays under Crate.
    fixture.selection.assign({fixture.crate, fixture.lid, fixture.rock});
    const Entity lamp = fixture.scene.findEntity(fixture.lamp);
    expect(static_cast<bool>(fixture.invoke("outliner.parent_drop", {{"parent", fixture.lamp.value}})) &&
               fixture.parent(fixture.crate) == lamp && fixture.parent(fixture.rock) == lamp &&
               fixture.parent(fixture.lid) == fixture.scene.findEntity(fixture.crate),
           "dropping should parent every selected root to the target");
    expect(fixture.history.size() == 1, "a drop should be one undo step");
    fixture.selection.assign({fixture.lamp});
    expect(fixture.invoke("outliner.parent_drop", {{"parent", fixture.crate.value}}).status ==
               OperatorStatus::Cancelled,
           "an object should not become its descendant's child");
    fixture.selection.assign({fixture.crate});
    expect(static_cast<bool>(fixture.invoke("outliner.parent_drop", {{"parent", nullptr}})) &&
               fixture.parent(fixture.crate) == entt::null,
           "dropping on the scene root should unparent");
    fixture.editing().undo();
    fixture.editing().undo();
    expect(fixture.parent(fixture.crate) == entt::null && fixture.parent(fixture.rock) == entt::null,
           "undo should restore the parents");
}

// The editor smoke's outliner step, headless: add, collect, link, undo all.
void testCollectionWorkflowUndo() {
    Fixture fixture;
    const size_t collections = fixture.scene.collections().all().size();
    fixture.invoke("object.add_empty", nlohmann::json::object());
    const SceneEntityId added = *fixture.selection.active();
    fixture.invoke("collection.new", {{"name", "Smoke"}, {"link_selection", false}});
    fixture.invoke("collection.link", {{"collection", collections + 1}});
    expect(fixture.history.size() == 3 && fixture.member(added, SceneCollectionId{collections + 1}),
           "adding, creating, and linking should be three undo steps: " + std::to_string(fixture.history.size()));
    for (int step = 0; step < 3; ++step) {
        fixture.editing().undo();
    }
    expect(fixture.scene.collections().all().size() == collections && fixture.scene.findEntity(added) == entt::null,
           "undoing the three steps should remove the collection and the empty");
}

void testAreaState() {
    HierarchyPanel panel;
    expect(panel.areaState() == "view_layer", "a new outliner should show the View Layer");
    panel.restoreAreaState("orphan_data");
    expect(panel.displayMode() == OutlinerDisplayMode::OrphanData && panel.areaState() == "orphan_data",
           "the display mode should restore from area state");
    panel.restoreAreaState("not a mode");
    expect(panel.displayMode() == OutlinerDisplayMode::OrphanData, "unknown area state should be ignored");
    HierarchyPanel other;
    other.restoreAreaState(panel.areaState());
    expect(other.displayMode() == OutlinerDisplayMode::OrphanData, "area state should round trip between panels");
}

const ImGuiWindow* panelWindow(const Fixture& fixture) {
    return HeadlessImGuiContext::findWindow(fixture.panel.title());
}

ImGuiID entityRow(const Fixture& fixture, SceneEntityId entity, uint64_t scope = 0) {
    const ImGuiWindow* window = panelWindow(fixture);
    return window == nullptr ? 0 : HierarchyPanel::entityRowId(window->ID, entity, scope);
}

ImGuiID collectionRow(const Fixture& fixture, SceneCollectionId collection) {
    const ImGuiWindow* window = panelWindow(fixture);
    return window == nullptr ? 0 : HierarchyPanel::collectionRowId(window->ID, collection);
}

constexpr float rowX = 200.0f;

// The height at which the row with id is hovered, scanning down the panel.
std::optional<float> rowY(Fixture& fixture, ImGuiID id) {
    ImGuiIO& io = ImGui::GetIO();
    for (float y = 0.0f; y < io.DisplaySize.y; y += 2.0f) {
        io.AddMousePosEvent(rowX, y);
        fixture.draw();
        if (ImGui::GetCurrentContext()->HoveredId == id) {
            return y;
        }
    }
    return std::nullopt;
}

void setModifiers(bool ctrl, bool shift) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
    io.AddKeyEvent(ImGuiMod_Shift, shift);
}

// Clicks the row with id holding the given modifiers; false when no row has it.
bool clickRow(Fixture& fixture, ImGuiID id, bool ctrl = false, bool shift = false) {
    const std::optional<float> y = rowY(fixture, id);
    if (!y) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    setModifiers(ctrl, shift);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    fixture.draw();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    fixture.draw();
    setModifiers(false, false);
    fixture.draw();
    return true;
}

// Drags the row with id onto the row with target.
bool dragRow(Fixture& fixture, ImGuiID id, ImGuiID target) {
    const std::optional<float> from = rowY(fixture, id);
    const std::optional<float> to = rowY(fixture, target);
    if (!from || !to) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(rowX, *from);
    fixture.draw();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    fixture.draw();
    for (int step = 1; step <= 8; ++step) {
        io.AddMousePosEvent(rowX + 2.0f * static_cast<float>(step),
                            *from + (*to - *from) * static_cast<float>(step) / 8.0f);
        fixture.draw();
    }
    fixture.draw();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    fixture.draw();
    fixture.draw();
    return true;
}

void testClicks() {
    Fixture fixture;
    HeadlessImGuiContext imgui;
    ImGui::GetIO().ConfigInputTrickleEventQueue = false;
    fixture.draw();
    fixture.draw();
    // Collections open by default; entities with children start closed.
    const std::vector<SceneEntityId> order{fixture.bulb, fixture.barrel, fixture.crate, fixture.barrel,
                                           fixture.lamp, fixture.tree,   fixture.tree2, fixture.rock};
    expect(fixture.panel.displayedEntities() == order, "the panel should draw the View Layer rows in order");

    expect(clickRow(fixture, entityRow(fixture, fixture.crate, fixture.props.value)), "Crate's row should be found");
    expect(fixture.selection.ids() == std::vector<SceneEntityId>{fixture.crate}, "a click should select the row");
    clickRow(fixture, entityRow(fixture, fixture.barrel, fixture.props.value), true);
    expect(fixture.selection.ids() == std::vector<SceneEntityId>{fixture.barrel, fixture.crate},
           "Ctrl click should extend the selection");
    clickRow(fixture, entityRow(fixture, fixture.tree), false, true);
    expect(fixture.selection.ids() ==
               std::vector<SceneEntityId>{fixture.barrel, fixture.crate, fixture.lamp, fixture.tree},
           "Shift click should select the displayed range from the last clicked row");
    clickRow(fixture, entityRow(fixture, fixture.rock), true, true);
    expect(fixture.selection.size() == 6 && fixture.selection.active() == fixture.barrel,
           "Ctrl Shift click should add a range and keep the anchor active");
    clickRow(fixture, entityRow(fixture, fixture.tree2));
    expect(fixture.selection.ids() == std::vector<SceneEntityId>{fixture.tree2},
           "a plain click on a selected row should select it alone when released");

    fixture.panel.setDisplayMode(OutlinerDisplayMode::Collections);
    fixture.draw();
    expect(fixture.panel.displayedEntities() == std::vector<SceneEntityId>{fixture.bulb, fixture.barrel, fixture.crate,
                                                                           fixture.barrel, fixture.lamp, fixture.lid,
                                                                           fixture.tree, fixture.tree2, fixture.rock},
           "the Collections mode should draw its rows");
    fixture.panel.setDisplayMode(OutlinerDisplayMode::Prefabs);
    fixture.draw();
    expect(fixture.panel.displayedEntities() == std::vector<SceneEntityId>{fixture.rock, fixture.tree, fixture.tree2},
           "the Prefabs mode should draw its rows");
    expect(fixture.history.size() == 0, "selecting should record no undo steps");
}

void testRestrictionToggles() {
    Fixture fixture;
    HeadlessImGuiContext imgui;
    fixture.draw();
    const ImGuiID rockRow = entityRow(fixture, fixture.rock);
    HeadlessImGuiContext::activateNextFrame(HierarchyPanel::restrictionToggleId(rockRow, "visible"));
    fixture.draw();
    const auto* visibility =
        fixture.scene.registry().try_get<VisibilityComponent>(fixture.scene.findEntity(fixture.rock));
    expect(visibility != nullptr && visibility->hidden && fixture.history.size() == 1,
           "the eye column should hide the object as one undo step");
    fixture.draw();
    HeadlessImGuiContext::activateNextFrame(HierarchyPanel::restrictionToggleId(rockRow, "visible"));
    fixture.draw();
    expect(!fixture.scene.registry().any_of<VisibilityComponent>(fixture.scene.findEntity(fixture.rock)),
           "the eye column should show a hidden object again");

    const ImGuiID propsRow = collectionRow(fixture, fixture.props);
    fixture.draw();
    HeadlessImGuiContext::activateNextFrame(HierarchyPanel::restrictionToggleId(propsRow, "selectable"));
    fixture.draw();
    expect(!fixture.scene.collections().find(fixture.props)->selectable && fixture.history.size() == 3,
           "the arrow column should make a collection unselectable");
    fixture.draw();
    HeadlessImGuiContext::activateNextFrame(HierarchyPanel::restrictionToggleId(propsRow, "renderable"));
    fixture.draw();
    expect(!fixture.scene.collections().find(fixture.props)->renderable, "the camera column should stop rendering");
    fixture.editing().undo();
    fixture.editing().undo();
    expect(fixture.scene.collections().find(fixture.props)->selectable &&
               fixture.scene.collections().find(fixture.props)->renderable,
           "undo should restore the collection flags");
}

void testDrags() {
    Fixture fixture;
    HeadlessImGuiContext imgui;
    ImGui::GetIO().ConfigInputTrickleEventQueue = false;
    fixture.draw();
    fixture.selection.assign({fixture.tree, fixture.tree2});
    expect(dragRow(fixture, entityRow(fixture, fixture.tree), entityRow(fixture, fixture.lamp)),
           "the rows to drag should be found");
    const Entity lamp = fixture.scene.findEntity(fixture.lamp);
    expect(fixture.parent(fixture.tree) == lamp && fixture.parent(fixture.tree2) == lamp,
           "dragging a selected row should reparent the whole selection");
    expect(fixture.history.size() == 1 && fixture.selection.size() == 2,
           "the drop should be one undo step and keep the selection");

    fixture.selection.assign({fixture.crate});
    expect(dragRow(fixture, entityRow(fixture, fixture.rock), collectionRow(fixture, fixture.empty)),
           "the rock and the empty collection should be found");
    expect(fixture.member(fixture.rock, fixture.empty) && !fixture.member(fixture.crate, fixture.empty),
           "dragging an unselected row onto a collection should link that row alone");
    expect(fixture.selection.ids() == std::vector<SceneEntityId>{fixture.rock} && fixture.history.size() == 2,
           "the dragged row should become the selection");
    fixture.editing().undo();
    fixture.editing().undo();
    expect(!fixture.member(fixture.rock, fixture.empty) && fixture.parent(fixture.tree) == entt::null,
           "undo should unlink and unparent");
}

void testSearch() {
    Fixture fixture;
    HeadlessImGuiContext imgui;
    fixture.draw();
    const ImGuiWindow* window = panelWindow(fixture);
    expect(window != nullptr, "the panel should draw its window");
    if (window == nullptr) {
        return;
    }
    HeadlessImGuiContext::activateNextFrame(HeadlessImGuiContext::itemId(*window, "##HierarchyFilter"));
    fixture.draw();
    HeadlessImGuiContext::typeNextFrame("BULB");
    fixture.draw();
    fixture.draw();
    // Lamp is closed, but searching opens the rows leading to a match.
    expect(fixture.panel.displayedEntities() == std::vector<SceneEntityId>{fixture.bulb, fixture.lamp, fixture.bulb},
           "a search should show every matching row with its ancestors");
}
} // namespace

int main() {
    testDisplayModes();
    testFilter();
    testOrphanData();
    testSetVisibilityOperator();
    testParentDropOperator();
    testCollectionWorkflowUndo();
    testAreaState();
    testClicks();
    testRestrictionToggles();
    testDrags();
    testSearch();
    if (failures != 0) {
        std::cerr << failures << " outliner test(s) failed\n";
        return 1;
    }
    std::cout << "Outliner tests passed\n";
    return 0;
}
