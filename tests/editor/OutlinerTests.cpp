#include "EditorTestFixture.hpp"
#include "HeadlessImGui.hpp"

#include "editor/interaction/EntityReferenceField.hpp"
#include "editor/operators/ObjectOperators.hpp"
#include "editor/panels/HierarchyPanel.hpp"
#include "editor/panels/outliner/OrphanData.hpp"
#include "editor/panels/outliner/OutlinerFilter.hpp"
#include "editor/panels/outliner/OutlinerRowWidgets.hpp"
#include "editor/panels/outliner/OutlinerRows.hpp"
#include "editor/panels/outliner/OutlinerTree.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/Prefab.hpp"
#include "engine/scene/SceneDocument.hpp"
#include "engine/scene/Tags.hpp"

#include "support/Doctest.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

TEST_SUITE_BEGIN("Outliner");

// The outliner: its display modes and filter (with the c:, t:, and p: search
// syntax) as pure trees, the flattened row cache, orphan data, the
// operators behind its gestures (reorder, collection rename, select
// matching), and the panel driven headlessly (area state, Ctrl/Shift
// clicks, restriction toggles, drags onto and between rows, search, the
// clipped rows and reveal, and in-place collection rename).

namespace {

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
struct Fixture : EditorTestFixture {
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
        CHECK_MESSAGE(registerObjectOperators(operators).success, "the object operators should register");
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
            CHECK_MESSAGE((static_cast<bool>(linkToCollection(scene, entity, collection))), "linking should work");
        }
        return scene.id(entity);
    }

    SceneEntityId addInstance(const char* name, const char* prefab) {
        const SceneEntityId id = add(name);
        scene.registry().emplace<PrefabInstanceComponent>(scene.findEntity(id),
                                                          PrefabInstanceComponent{.prefab = prefab});
        return id;
    }

    OperatorResult invoke(std::string_view operatorId, const nlohmann::json& properties) {
        return EditorTestFixture::invoke(operatorId, properties.dump());
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

    std::vector<SceneEntityId> children(SceneEntityId id) const {
        std::vector<SceneEntityId> ids;
        for (const Entity child : scene.registry().get<RelationshipComponent>(scene.findEntity(id)).children) {
            ids.push_back(scene.id(child));
        }
        return ids;
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

TEST_CASE("Outliner: display modes") {
    Fixture fixture;
    const Scene& scene = fixture.scene;
    CHECK_MESSAGE(
        tree(scene, OutlinerDisplayMode::ViewLayer) ==
            "Props[Crates[Bulb,Barrel],Crate[Lid],Barrel],Empty,Lamp[Bulb],Tree,Tree 2,Rock",
        ("View Layer should group hierarchies by collection: " + tree(scene, OutlinerDisplayMode::ViewLayer)));
    CHECK_MESSAGE(tree(scene, OutlinerDisplayMode::Collections) ==
                      "Props[Crates[Bulb,Barrel],Crate,Barrel],Empty,Lamp,Lid,Tree,Tree 2,Rock",
                  ("Collections should list members flat: " + tree(scene, OutlinerDisplayMode::Collections)));
    CHECK_MESSAGE(tree(scene, OutlinerDisplayMode::Prefabs) ==
                      "prefabs/rock.prefab[Rock],prefabs/tree.prefab[Tree,Tree 2]",
                  ("Prefabs should group instances by prefab: " + tree(scene, OutlinerDisplayMode::Prefabs)));
    const std::vector<OrphanAsset> orphans{{.path = "textures/unused.png", .kind = EditorAssetKind::Texture}};
    CHECK_MESSAGE(tree(scene, OutlinerDisplayMode::OrphanData, {}, orphans) ==
                      "Unused Assets[textures/unused.png],Empty Collections[Empty]",
                  ("Orphan Data should list unused assets and empty collections: " +
                   tree(scene, OutlinerDisplayMode::OrphanData, {}, orphans)));

    // Each listing of an entity has its own scope, so its rows get distinct ids.
    const std::vector<OutlinerNode> viewLayer = buildOutlinerTree(scene, {.mode = OutlinerDisplayMode::ViewLayer});
    const OutlinerNode& bulbInCrates = viewLayer[0].children[0].children[0];
    const OutlinerNode& bulbUnderLamp = viewLayer[2].children[0];
    CHECK_MESSAGE((bulbInCrates.entity == fixture.bulb && bulbUnderLamp.entity == fixture.bulb &&
                   bulbInCrates.scope == fixture.crates.value && bulbUnderLamp.scope == 0),
                  "an entity listed in a collection and under its parent should have two scopes");
    const std::vector<OutlinerNode> prefabs = buildOutlinerTree(scene, {.mode = OutlinerDisplayMode::Prefabs});
    CHECK_MESSAGE((prefabs[0].children[0].scope != 0 && prefabs[0].children[0].scope != prefabs[1].children[0].scope),
                  "prefab groups should scope their rows apart from the top level and each other");

    for (const OutlinerDisplayMode mode : outlinerDisplayModes) {
        CHECK_MESSAGE(parseOutlinerDisplayMode(outlinerDisplayModeKey(mode)) == mode,
                      "display mode keys should round trip");
    }
    CHECK_MESSAGE(!parseOutlinerDisplayMode("blender_file"), "unknown display mode keys should be rejected");
}

TEST_CASE("Outliner: filter") {
    Fixture fixture;
    const Scene& scene = fixture.scene;
    CHECK_MESSAGE((outlinerFilterMatches("Lamp Post", "") && outlinerFilterMatches("Lamp Post", "mP p") &&
                   !outlinerFilterMatches("Lamp", "lamps")),
                  "the filter should match substrings ignoring case");
    // A matching child shows with its non-matching ancestors, whatever the case.
    CHECK_MESSAGE(
        tree(scene, OutlinerDisplayMode::ViewLayer, "BULB") == "*Props[*Crates[Bulb]],*Lamp[Bulb]",
        ("matches should appear with their ancestors: " + tree(scene, OutlinerDisplayMode::ViewLayer, "BULB")));
    CHECK_MESSAGE(
        tree(scene, OutlinerDisplayMode::ViewLayer, "lid") == "*Props[*Crate[Lid]]",
        ("a child row should match case-insensitively: " + tree(scene, OutlinerDisplayMode::ViewLayer, "lid")));
    CHECK_MESSAGE(tree(scene, OutlinerDisplayMode::Prefabs, "tree 2") == "*prefabs/tree.prefab[Tree 2]",
                  "filters should apply in every mode");
    CHECK_MESSAGE(tree(scene, OutlinerDisplayMode::ViewLayer, "nothing").empty(),
                  "a filter matching nothing lists nothing");
}

TEST_CASE("Outliner: orphan data") {
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
    CHECK_MESSAGE((collectSceneAssetUsage(document.dump(), usage, error) && usage.paths.contains("models/crate.gltf") &&
                   usage.guids.contains(moved)),
                  ("asset references should be found at any depth: " + error));
    CHECK_MESSAGE((!collectSceneAssetUsage("{\"version\":3}", usage, error) && !error.empty()),
                  "a document without entities should be rejected");
    collectSceneAssetUsage(document.dump(), usage, error);

    const testsupport::TempDirectory projectDirectory("outliner-orphans");
    const std::filesystem::path root = projectDirectory / "assets";
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
    CHECK_MESSAGE((orphans.size() == 2 && orphans[0].path == "audio/hit.wav" &&
                   orphans[1].path == "textures/unused.png" && orphans[1].kind == EditorAssetKind::Texture),
                  "assets referenced by path or GUID, scenes, and files outside the root should not be orphans");

    Fixture fixture;
    CHECK_MESSAGE((findEmptyCollections(fixture.scene) == std::vector<SceneCollectionId>{fixture.empty}),
                  "a collection with members in a nested collection should not be empty");
}

TEST_CASE("Outliner: filter syntax") {
    const OutlinerFilter parsed =
        parseOutlinerFilter(R"(  crate c:"Point Light" T:enemy prefab:door http:x "big box" )");
    const std::vector<OutlinerFilterTerm> expected{
        {.field = OutlinerFilterField::Name, .text = "crate"},
        {.field = OutlinerFilterField::Component, .text = "Point Light"},
        {.field = OutlinerFilterField::Tag, .text = "enemy"},
        {.field = OutlinerFilterField::Prefab, .text = "door"},
        {.field = OutlinerFilterField::Name, .text = "http:x"},
        {.field = OutlinerFilterField::Name, .text = "big box"},
    };
    CHECK_MESSAGE(parsed.terms == expected, "the filter should split terms, read prefixes, and keep quoted spaces");
    CHECK_MESSAGE((!parsed.namesOnly() && parseOutlinerFilter("a b").namesOnly() && parseOutlinerFilter("  ").empty()),
                  "a filter should know whether it names labels only");

    Fixture fixture;
    Scene& scene = fixture.scene;
    scene.registry().emplace<PointLightComponent>(scene.findEntity(fixture.bulb));
    scene.registry().emplace<TagsComponent>(scene.findEntity(fixture.crate), TagsComponent{{"enemy", "boss"}});
    CHECK_MESSAGE(
        tree(scene, OutlinerDisplayMode::ViewLayer, "c:point") == "*Props[*Crates[Bulb]],*Lamp[Bulb]",
        ("c: should match a component's display name: " + tree(scene, OutlinerDisplayMode::ViewLayer, "c:point")));
    CHECK_MESSAGE(tree(scene, OutlinerDisplayMode::ViewLayer, "component:point_light") ==
                      tree(scene, OutlinerDisplayMode::ViewLayer, "c:point"),
                  "component: should match a component's key");
    CHECK_MESSAGE(tree(scene, OutlinerDisplayMode::ViewLayer, "t:ENE") == "*Props[Crate]",
                  ("t: should match tags ignoring case: " + tree(scene, OutlinerDisplayMode::ViewLayer, "t:ENE")));
    CHECK_MESSAGE(tree(scene, OutlinerDisplayMode::Prefabs, "p:rock") == "*prefabs/rock.prefab[Rock]",
                  "p: should match a prefab instance's path");
    CHECK_MESSAGE((tree(scene, OutlinerDisplayMode::ViewLayer, "c:point lamp").empty() &&
                   tree(scene, OutlinerDisplayMode::ViewLayer, "c:point bulb") ==
                       tree(scene, OutlinerDisplayMode::ViewLayer, "c:point")),
                  "terms should combine with AND");
    CHECK_MESSAGE(tree(scene, OutlinerDisplayMode::ViewLayer, "c:no_such_component").empty(),
                  "a component no type names should match nothing");
    CHECK_MESSAGE(tree(scene, OutlinerDisplayMode::ViewLayer, "props") == "Props",
                  "name terms should match collection rows too");
    CHECK_MESSAGE((tree(scene, OutlinerDisplayMode::ViewLayer, "t:").find("Crate") != std::string::npos &&
                   tree(scene, OutlinerDisplayMode::ViewLayer, "t:").find("Lamp") == std::string::npos),
                  "an empty tag term should match any tagged object");
    CHECK_MESSAGE((findOutlinerMatches(scene, parseOutlinerFilter("t:enemy"), *builtInComponentTypes()) ==
                   std::vector<SceneEntityId>{fixture.crate}),
                  "matches should be found across the scene");

    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("outliner.select_matching", {{"filter", "p:tree"}})) &&
                   fixture.selection.ids() == std::vector<SceneEntityId>{fixture.tree, fixture.tree2}),
                  "Select All Matching should select every match");
    CHECK_MESSAGE((fixture.invoke("outliner.select_matching", {{"filter", ""}}).status == OperatorStatus::Cancelled &&
                   fixture.invoke("outliner.select_matching", {{"filter", "zzz"}}).status == OperatorStatus::Cancelled),
                  "an empty search or one without matches should select nothing");
    CHECK_MESSAGE(fixture.history.size() == 0, "selecting matches should record no undo step");
}

TEST_CASE("Outliner: row index") {
    Fixture fixture;
    const std::vector<OutlinerNode> rows = buildOutlinerTree(fixture.scene, {.mode = OutlinerDisplayMode::ViewLayer});
    constexpr ImGuiID seed = 1234;
    const std::vector<OutlinerRowEntry> entries = indexOutlinerRows(rows, seed);
    // Props, Crates, Bulb, Barrel, Crate, Lid, Barrel, Empty, Lamp, Bulb, Tree, Tree 2, Rock.
    std::unordered_set<ImGuiID> ids;
    for (const OutlinerRowEntry& entry : entries) {
        ids.insert(entry.id);
    }
    CHECK_MESSAGE((entries.size() == 13 && ids.size() == 13), "every node should have an entry with a unique id");
    CHECK_MESSAGE((entries[0].subtreeEnd == 7 && entries[1].parent == 0 && entries[2].depth == 2 &&
                   entries[5].parent == 4 && entries[8].parent == OutlinerRowEntry::noParent),
                  "entries should record their subtree, parent, and depth");
    CHECK_MESSAGE((entries[4].id == HierarchyPanel::entityRowId(seed, fixture.crate, fixture.props.value) &&
                   entries[0].id == HierarchyPanel::collectionRowId(seed, fixture.props)),
                  "entry ids should be the panel's row ids");

    const auto closed = [](const OutlinerRowEntry&) { return false; };
    const auto open = [](const OutlinerRowEntry&) { return true; };
    CHECK_MESSAGE((visibleOutlinerRows(entries, closed) == std::vector<uint32_t>{0, 7, 8, 10, 11, 12}),
                  "closed rows should hide their subtrees");
    CHECK_MESSAGE(visibleOutlinerRows(entries, open).size() == entries.size(), "open rows should show everything");

    // An entity listed twice in one scope still gets two ids.
    const std::vector<OutlinerNode> twice{{.kind = OutlinerNodeKind::Entity, .entity = fixture.lamp},
                                          {.kind = OutlinerNodeKind::Entity, .entity = fixture.lamp}};
    const std::vector<OutlinerRowEntry> repeated = indexOutlinerRows(twice, seed);
    CHECK_MESSAGE(
        (repeated[0].id == HierarchyPanel::entityRowId(seed, fixture.lamp) && repeated[1].id != repeated[0].id),
        "a repeated row should get a distinct id");

    OutlinerRowCache cache;
    int builds = 0;
    const auto build = [&] {
        ++builds;
        return buildOutlinerTree(fixture.scene, {.mode = OutlinerDisplayMode::ViewLayer});
    };
    const OutlinerRowSource source{.seed = seed, .historyRevision = 1, .entityCount = 8};
    CHECK_MESSAGE((cache.update(source, fixture.scene.collections(), build) &&
                   !cache.update(source, fixture.scene.collections(), build) && builds == 1),
                  "an unchanged source should not rebuild");
    CHECK_MESSAGE((cache.updateVisible(0, closed) && !cache.updateVisible(0, closed) && cache.visible().size() == 6),
                  "visible rows should follow the open revision");
    OutlinerRowSource edited = source;
    edited.historyRevision = 2;
    CHECK_MESSAGE((cache.update(edited, fixture.scene.collections(), build) && cache.updateVisible(0, closed)),
                  "a new history revision should rebuild the tree and its visible rows");
    SceneCollections renamed = fixture.scene.collections();
    renamed.rename(fixture.props, "Things");
    CHECK_MESSAGE((cache.update(edited, renamed, build) && builds == 3), "changed collections should rebuild");
    CHECK_MESSAGE((cache.visibleIndex(entries[7].id) == 1u && cache.findEntity(fixture.lamp) == 8u),
                  "the cache should find rows by id and entity");
}

TEST_CASE("Outliner: reorder operator") {
    Fixture fixture;
    const SceneEntityId wick = fixture.add("Wick", fixture.lamp);
    const SceneEntityId shade = fixture.add("Shade", fixture.lamp);
    const auto reorder = [&fixture](SceneEntityId entity, SceneEntityId parent, uint32_t index) {
        return fixture.invoke("outliner.reorder",
                              {{"entity", entity.value}, {"parent", parent.value}, {"index", index}});
    };
    CHECK_MESSAGE((static_cast<bool>(reorder(shade, fixture.lamp, 0)) &&
                   fixture.children(fixture.lamp) == std::vector<SceneEntityId>{shade, fixture.bulb, wick}),
                  "reordering should move the object among its siblings");
    CHECK_MESSAGE(reorder(shade, fixture.lamp, 0).status == OperatorStatus::Cancelled,
                  "an unmoved object changes nothing");
    CHECK_MESSAGE(
        (static_cast<bool>(reorder(fixture.lid, fixture.lamp, 1)) &&
         fixture.parent(fixture.lid) == fixture.scene.findEntity(fixture.lamp) &&
         fixture.children(fixture.lamp) == std::vector<SceneEntityId>{shade, fixture.lid, fixture.bulb, wick}),
        "reordering under another parent should reparent at the index");
    CHECK_MESSAGE(fixture.history.size() == 2, "each reorder should be one undo step");

    std::string saved;
    CHECK_MESSAGE(fixture.editing().capture(saved), "the scene should serialize");
    Scene loaded;
    const SceneDocumentResult load =
        SceneDocument::deserialize(saved, fixture.assets, fixture.document.options(), loaded);
    std::vector<SceneEntityId> loadedOrder;
    for (const Entity child : loaded.registry().get<RelationshipComponent>(loaded.findEntity(fixture.lamp)).children) {
        loadedOrder.push_back(loaded.id(child));
    }
    CHECK_MESSAGE((load.success && loadedOrder == fixture.children(fixture.lamp)),
                  ("the sibling order should survive saving and loading: " + load.error));

    fixture.editing().undo();
    CHECK_MESSAGE((fixture.parent(fixture.lid) == fixture.scene.findEntity(fixture.crate) &&
                   fixture.children(fixture.lamp) == std::vector<SceneEntityId>{shade, fixture.bulb, wick}),
                  "undo should put the object back under its parent");
    fixture.editing().undo();
    CHECK_MESSAGE((fixture.children(fixture.lamp) == std::vector<SceneEntityId>{fixture.bulb, wick, shade}),
                  "undo should restore the id order");
    fixture.editing().redo();
    CHECK_MESSAGE((fixture.children(fixture.lamp) == std::vector<SceneEntityId>{shade, fixture.bulb, wick}),
                  "redo should reorder again");
    CHECK_MESSAGE((reorder(shade, {}, 0).status == OperatorStatus::Cancelled &&
                   reorder(fixture.lamp, fixture.bulb, 0).status == OperatorStatus::Cancelled),
                  "root order and cycles should be refused");
}

TEST_CASE("Outliner: collection rename operator") {
    Fixture fixture;
    const auto name = [&fixture] { return fixture.scene.collections().find(fixture.props)->name; };
    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("collection.rename",
                                                    {{"collection", fixture.props.value}, {"name", "  Things "}})) &&
                   name() == "Things" && fixture.history.size() == 1),
                  "renaming should trim the name and be one undo step");
    CHECK_MESSAGE(
        (fixture.invoke("collection.rename", {{"collection", fixture.props.value}, {"name", " "}}).status ==
             OperatorStatus::Cancelled &&
         fixture.invoke("collection.rename", {{"collection", fixture.props.value}, {"name", "Things"}}).status ==
             OperatorStatus::Cancelled),
        "an empty or unchanged name should change nothing");
    CHECK_MESSAGE((static_cast<bool>(
                      fixture.invoke("collection.rename", {{"collection", fixture.crates.value}, {"name", "Things"}}))),
                  "collection names need not be unique");
    CHECK_MESSAGE(fixture.invoke("collection.rename", {{"collection", 4242}, {"name", "X"}}).status ==
                      OperatorStatus::Failed,
                  "an unknown collection should fail");
    fixture.diagnostics.acknowledgeErrors();
    fixture.editing().undo();
    fixture.editing().undo();
    CHECK_MESSAGE(name() == "Props", "undo should restore the name");
}

TEST_CASE("Outliner: set visibility operator") {
    Fixture fixture;
    const auto visibility = [&fixture] {
        return fixture.scene.registry().try_get<VisibilityComponent>(fixture.scene.findEntity(fixture.rock));
    };
    fixture.selection.assign({fixture.rock, fixture.tree});
    const uint64_t renderable = fixture.scene.revisions().renderable;
    CHECK_MESSAGE(
        (static_cast<bool>(fixture.invoke("object.set_visibility",
                                          {{"entity", fixture.rock.value}, {"flag", "visible"}, {"value", false}})) &&
         visibility() != nullptr && visibility()->hidden),
        "hiding through the column should set the viewport hide");
    CHECK_MESSAGE((fixture.selection.ids() == std::vector<SceneEntityId>{fixture.tree}),
                  "a hidden object should leave the selection");
    CHECK_MESSAGE(fixture.scene.revisions().renderable != renderable,
                  "a visibility change should invalidate render caches");
    CHECK_MESSAGE(
        fixture.invoke("object.set_visibility", {{"entity", fixture.rock.value}, {"flag", "visible"}, {"value", false}})
                .status == OperatorStatus::Cancelled,
        "setting a flag to its value should change nothing");
    fixture.invoke("object.set_visibility", {{"entity", fixture.rock.value}, {"flag", "renderable"}, {"value", false}});
    CHECK_MESSAGE((visibility() != nullptr && !visibility()->renderable),
                  "the renderable column should set renderable");
    fixture.editing().undo();
    CHECK_MESSAGE((visibility() != nullptr && visibility()->renderable && visibility()->hidden),
                  "undo should restore the renderable flag only");
    fixture.editing().undo();
    CHECK_MESSAGE(visibility() == nullptr, "undoing the hide should leave no visibility component");
    fixture.editing().redo();
    fixture.invoke("object.set_visibility", {{"entity", fixture.rock.value}, {"flag", "visible"}, {"value", true}});
    CHECK_MESSAGE(visibility() == nullptr, "returning every flag to its default should remove the component");
    fixture.invoke("object.set_visibility", {{"entity", fixture.rock.value}, {"flag", "selectable"}, {"value", false}});
    CHECK_MESSAGE((visibility() != nullptr && !visibility()->selectable && !visibility()->hidden),
                  "the selectable column should set selectable");
    CHECK_MESSAGE(
        fixture.invoke("object.set_visibility", {{"entity", 4242}, {"flag", "visible"}, {"value", false}}).status ==
            OperatorStatus::Failed,
        "an unknown object should fail");
    fixture.diagnostics.acknowledgeErrors();
}

TEST_CASE("Outliner: parent drop operator") {
    Fixture fixture;
    // Crate's child Lid is selected with it, so it stays under Crate.
    fixture.selection.assign({fixture.crate, fixture.lid, fixture.rock});
    const Entity lamp = fixture.scene.findEntity(fixture.lamp);
    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("outliner.parent_drop", {{"parent", fixture.lamp.value}})) &&
                   fixture.parent(fixture.crate) == lamp && fixture.parent(fixture.rock) == lamp &&
                   fixture.parent(fixture.lid) == fixture.scene.findEntity(fixture.crate)),
                  "dropping should parent every selected root to the target");
    CHECK_MESSAGE(fixture.history.size() == 1, "a drop should be one undo step");
    fixture.selection.assign({fixture.lamp});
    CHECK_MESSAGE(fixture.invoke("outliner.parent_drop", {{"parent", fixture.crate.value}}).status ==
                      OperatorStatus::Cancelled,
                  "an object should not become its descendant's child");
    fixture.selection.assign({fixture.crate});
    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("outliner.parent_drop", {{"parent", nullptr}})) &&
                   fixture.parent(fixture.crate) == entt::null),
                  "dropping on the scene root should unparent");
    fixture.editing().undo();
    fixture.editing().undo();
    CHECK_MESSAGE((fixture.parent(fixture.crate) == entt::null && fixture.parent(fixture.rock) == entt::null),
                  "undo should restore the parents");
}

// The editor smoke's outliner step, headless: add, collect, link, undo all.
TEST_CASE("Outliner: collection workflow undo") {
    Fixture fixture;
    const size_t collections = fixture.scene.collections().all().size();
    fixture.invoke("object.add_empty", nlohmann::json::object());
    const SceneEntityId added = *fixture.selection.active();
    fixture.invoke("collection.new", {{"name", "Smoke"}, {"link_selection", false}});
    fixture.invoke("collection.link", {{"collection", collections + 1}});
    CHECK_MESSAGE(
        (fixture.history.size() == 3 && fixture.member(added, SceneCollectionId{collections + 1})),
        ("adding, creating, and linking should be three undo steps: " + std::to_string(fixture.history.size())));
    for (int step = 0; step < 3; ++step) {
        fixture.editing().undo();
    }
    CHECK_MESSAGE(
        (fixture.scene.collections().all().size() == collections && fixture.scene.findEntity(added) == entt::null),
        "undoing the three steps should remove the collection and the empty");
}

TEST_CASE("Outliner: area state") {
    HierarchyPanel panel;
    CHECK_MESSAGE(panel.areaState() == "view_layer", "a new outliner should show the View Layer");
    panel.restoreAreaState("orphan_data");
    CHECK_MESSAGE((panel.displayMode() == OutlinerDisplayMode::OrphanData && panel.areaState() == "orphan_data"),
                  "the display mode should restore from area state");
    panel.restoreAreaState("not a mode");
    CHECK_MESSAGE(panel.displayMode() == OutlinerDisplayMode::OrphanData, "unknown area state should be ignored");
    HierarchyPanel other;
    other.restoreAreaState(panel.areaState());
    CHECK_MESSAGE(other.displayMode() == OutlinerDisplayMode::OrphanData,
                  "area state should round trip between panels");
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

// The heights at which the row with id is hovered (its first and last),
// scanning down the panel.
std::optional<std::pair<float, float>> rowSpan(Fixture& fixture, ImGuiID id) {
    ImGuiIO& io = ImGui::GetIO();
    std::optional<std::pair<float, float>> span;
    for (float y = 0.0f; y < io.DisplaySize.y; y += 2.0f) {
        io.AddMousePosEvent(rowX, y);
        fixture.draw();
        if (ImGui::GetCurrentContext()->HoveredId == id) {
            span = std::pair{span ? span->first : y, y};
        } else if (span) {
            break;
        }
    }
    return span;
}

// A height inside the row with id: fraction 0 is its top, 1 its bottom.
std::optional<float> rowY(Fixture& fixture, ImGuiID id, float fraction = 0.5f) {
    const std::optional<std::pair<float, float>> span = rowSpan(fixture, id);
    if (!span) {
        return std::nullopt;
    }
    // Leave the mouse there, as clicks and drags start from it.
    const float y = span->first + (span->second - span->first) * fraction;
    ImGui::GetIO().AddMousePosEvent(rowX, y);
    fixture.draw();
    return y;
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

// Drags the row with id onto the row with target, dropping at fraction of
// its height (the middle drops onto it, the edges between rows).
bool dragRow(Fixture& fixture, ImGuiID id, ImGuiID target, float fraction = 0.5f) {
    const std::optional<float> from = rowY(fixture, id);
    const std::optional<float> to = rowY(fixture, target, fraction);
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

TEST_CASE("Outliner: clicks") {
    Fixture fixture;
    HeadlessImGuiContext imgui;
    ImGui::GetIO().ConfigInputTrickleEventQueue = false;
    fixture.draw();
    fixture.draw();
    // Collections open by default; entities with children start closed.
    const std::vector<SceneEntityId> order{fixture.bulb, fixture.barrel, fixture.crate, fixture.barrel,
                                           fixture.lamp, fixture.tree,   fixture.tree2, fixture.rock};
    CHECK_MESSAGE(fixture.panel.displayedEntities() == order, "the panel should draw the View Layer rows in order");

    CHECK_MESSAGE(clickRow(fixture, entityRow(fixture, fixture.crate, fixture.props.value)),
                  "Crate's row should be found");
    CHECK_MESSAGE((fixture.selection.ids() == std::vector<SceneEntityId>{fixture.crate}),
                  "a click should select the row");
    clickRow(fixture, entityRow(fixture, fixture.barrel, fixture.props.value), true);
    CHECK_MESSAGE((fixture.selection.ids() == std::vector<SceneEntityId>{fixture.barrel, fixture.crate}),
                  "Ctrl click should extend the selection");
    clickRow(fixture, entityRow(fixture, fixture.tree), false, true);
    CHECK_MESSAGE((fixture.selection.ids() ==
                   std::vector<SceneEntityId>{fixture.barrel, fixture.crate, fixture.lamp, fixture.tree}),
                  "Shift click should select the displayed range from the last clicked row");
    clickRow(fixture, entityRow(fixture, fixture.rock), true, true);
    CHECK_MESSAGE((fixture.selection.size() == 6 && fixture.selection.active() == fixture.barrel),
                  "Ctrl Shift click should add a range and keep the anchor active");
    clickRow(fixture, entityRow(fixture, fixture.tree2));
    CHECK_MESSAGE((fixture.selection.ids() == std::vector<SceneEntityId>{fixture.tree2}),
                  "a plain click on a selected row should select it alone when released");

    fixture.panel.setDisplayMode(OutlinerDisplayMode::Collections);
    fixture.draw();
    CHECK_MESSAGE((fixture.panel.displayedEntities() ==
                   std::vector<SceneEntityId>{fixture.bulb, fixture.barrel, fixture.crate, fixture.barrel, fixture.lamp,
                                              fixture.lid, fixture.tree, fixture.tree2, fixture.rock}),
                  "the Collections mode should draw its rows");
    fixture.panel.setDisplayMode(OutlinerDisplayMode::Prefabs);
    fixture.draw();
    CHECK_MESSAGE(
        (fixture.panel.displayedEntities() == std::vector<SceneEntityId>{fixture.rock, fixture.tree, fixture.tree2}),
        "the Prefabs mode should draw its rows");
    CHECK_MESSAGE(fixture.history.size() == 0, "selecting should record no undo steps");
}

TEST_CASE("Outliner: restriction toggles") {
    Fixture fixture;
    HeadlessImGuiContext imgui;
    fixture.draw();
    const ImGuiID rockRow = entityRow(fixture, fixture.rock);
    HeadlessImGuiContext::activateNextFrame(HierarchyPanel::restrictionToggleId(rockRow, "visible"));
    fixture.draw();
    const auto* visibility =
        fixture.scene.registry().try_get<VisibilityComponent>(fixture.scene.findEntity(fixture.rock));
    CHECK_MESSAGE((visibility != nullptr && visibility->hidden && fixture.history.size() == 1),
                  "the eye column should hide the object as one undo step");
    fixture.draw();
    HeadlessImGuiContext::activateNextFrame(HierarchyPanel::restrictionToggleId(rockRow, "visible"));
    fixture.draw();
    CHECK_MESSAGE((!fixture.scene.registry().any_of<VisibilityComponent>(fixture.scene.findEntity(fixture.rock))),
                  "the eye column should show a hidden object again");

    const ImGuiID propsRow = collectionRow(fixture, fixture.props);
    fixture.draw();
    HeadlessImGuiContext::activateNextFrame(HierarchyPanel::restrictionToggleId(propsRow, "selectable"));
    fixture.draw();
    CHECK_MESSAGE((!fixture.scene.collections().find(fixture.props)->selectable && fixture.history.size() == 3),
                  "the arrow column should make a collection unselectable");
    fixture.draw();
    HeadlessImGuiContext::activateNextFrame(HierarchyPanel::restrictionToggleId(propsRow, "renderable"));
    fixture.draw();
    CHECK_MESSAGE(!fixture.scene.collections().find(fixture.props)->renderable,
                  "the camera column should stop rendering");
    fixture.editing().undo();
    fixture.editing().undo();
    CHECK_MESSAGE((fixture.scene.collections().find(fixture.props)->selectable &&
                   fixture.scene.collections().find(fixture.props)->renderable),
                  "undo should restore the collection flags");
}

TEST_CASE("Outliner: drags") {
    Fixture fixture;
    HeadlessImGuiContext imgui;
    ImGui::GetIO().ConfigInputTrickleEventQueue = false;
    fixture.draw();
    fixture.selection.assign({fixture.tree, fixture.tree2});
    CHECK_MESSAGE(dragRow(fixture, entityRow(fixture, fixture.tree), entityRow(fixture, fixture.lamp)),
                  "the rows to drag should be found");
    const Entity lamp = fixture.scene.findEntity(fixture.lamp);
    CHECK_MESSAGE((fixture.parent(fixture.tree) == lamp && fixture.parent(fixture.tree2) == lamp),
                  "dragging a selected row should reparent the whole selection");
    CHECK_MESSAGE((fixture.history.size() == 1 && fixture.selection.size() == 2),
                  "the drop should be one undo step and keep the selection");

    fixture.selection.assign({fixture.crate});
    CHECK_MESSAGE(dragRow(fixture, entityRow(fixture, fixture.rock), collectionRow(fixture, fixture.empty)),
                  "the rock and the empty collection should be found");
    CHECK_MESSAGE((fixture.member(fixture.rock, fixture.empty) && !fixture.member(fixture.crate, fixture.empty)),
                  "dragging an unselected row onto a collection should link that row alone");
    CHECK_MESSAGE((fixture.selection.ids() == std::vector<SceneEntityId>{fixture.rock} && fixture.history.size() == 2),
                  "the dragged row should become the selection");
    fixture.editing().undo();
    fixture.editing().undo();
    CHECK_MESSAGE((!fixture.member(fixture.rock, fixture.empty) && fixture.parent(fixture.tree) == entt::null),
                  "undo should unlink and unparent");
}

TEST_CASE("Outliner: search") {
    Fixture fixture;
    HeadlessImGuiContext imgui;
    fixture.draw();
    const ImGuiWindow* window = panelWindow(fixture);
    CHECK_MESSAGE(window != nullptr, "the panel should draw its window");
    if (window == nullptr) {
        return;
    }
    HeadlessImGuiContext::activateNextFrame(HeadlessImGuiContext::itemId(*window, "##HierarchyFilter"));
    fixture.draw();
    HeadlessImGuiContext::typeNextFrame("BULB");
    fixture.draw();
    fixture.draw();
    // Lamp is closed, but searching opens the rows leading to a match.
    CHECK_MESSAGE(
        (fixture.panel.displayedEntities() == std::vector<SceneEntityId>{fixture.bulb, fixture.lamp, fixture.bulb}),
        "a search should show every matching row with its ancestors");
}

TEST_CASE("Outliner: row cache invalidation") {
    Fixture fixture;
    HeadlessImGuiContext imgui;
    fixture.draw();
    const OutlinerRowCache::Stats first = fixture.panel.rowCacheStats();
    for (int frame = 0; frame < 10; ++frame) {
        fixture.draw();
    }
    const OutlinerRowCache::Stats steady = fixture.panel.rowCacheStats();
    CHECK_MESSAGE((first.treeBuilds == 1 && steady.treeBuilds == 1 && steady.visibleBuilds == first.visibleBuilds),
                  "frames without changes should rebuild nothing");
    fixture.invoke("collection.rename", {{"collection", fixture.empty.value}, {"name", "Spare"}});
    fixture.draw();
    CHECK_MESSAGE(fixture.panel.rowCacheStats().treeBuilds == 2, "a recorded edit should rebuild the rows");
    fixture.scene.createEntity("Loose");
    fixture.draw();
    CHECK_MESSAGE(fixture.panel.rowCacheStats().treeBuilds == 3, "an entity added outside the history should rebuild");
    fixture.panel.setDisplayMode(OutlinerDisplayMode::Prefabs);
    fixture.draw();
    fixture.panel.setDisplayMode(OutlinerDisplayMode::ViewLayer);
    fixture.draw();
    CHECK_MESSAGE(fixture.panel.rowCacheStats().treeBuilds == 5, "switching display modes should rebuild");

    // Making Lid active from outside the outliner opens Crate to show it.
    const uint64_t trees = fixture.panel.rowCacheStats().treeBuilds;
    const uint64_t visible = fixture.panel.rowCacheStats().visibleBuilds;
    fixture.selection.assign({fixture.lid});
    fixture.draw();
    const std::vector<SceneEntityId>& shown = fixture.panel.displayedEntities();
    CHECK_MESSAGE(std::ranges::find(shown, fixture.lid) != shown.end(),
                  "revealing should open the active object's parents");
    CHECK_MESSAGE((fixture.panel.rowCacheStats().treeBuilds == trees &&
                   fixture.panel.rowCacheStats().visibleBuilds == visible + 1),
                  "opening a row should recompute only the visible rows");
}

TEST_CASE("Outliner: clipped rows and reveal") {
    Fixture fixture;
    for (int index = 0; index < 300; ++index) {
        fixture.add(("Filler " + std::to_string(index)).c_str());
    }
    const SceneEntityId deep = fixture.add("Deep");
    const SceneEntityId needle = fixture.add("Needle", deep);
    HeadlessImGuiContext imgui;
    ImGui::GetIO().ConfigInputTrickleEventQueue = false;
    fixture.draw();
    fixture.draw();
    CHECK_MESSAGE(
        fixture.panel.displayedEntities().size() == 309,
        ("every open row should count as displayed: " + std::to_string(fixture.panel.displayedEntities().size())));
    CHECK_MESSAGE(!rowY(fixture, entityRow(fixture, deep)), "a row far below the view should not be drawn");

    fixture.selection.assign({needle});
    for (int frame = 0; frame < 3; ++frame) {
        fixture.draw();
    }
    CHECK_MESSAGE(rowY(fixture, entityRow(fixture, needle)).has_value(),
                  "an object made active elsewhere should be opened and scrolled into view");
    CHECK_MESSAGE(
        (clickRow(fixture, entityRow(fixture, deep)) && fixture.selection.ids() == std::vector<SceneEntityId>{deep}),
        "rows scrolled into view should take clicks");
}

TEST_CASE("Outliner: reorder drag") {
    Fixture fixture;
    const SceneEntityId wick = fixture.add("Wick", fixture.lamp);
    const SceneEntityId shade = fixture.add("Shade", fixture.lamp);
    HeadlessImGuiContext imgui;
    ImGui::GetIO().ConfigInputTrickleEventQueue = false;
    // Revealing Wick opens Lamp.
    fixture.selection.assign({wick});
    fixture.draw();
    fixture.draw();
    CHECK_MESSAGE(dragRow(fixture, entityRow(fixture, shade), entityRow(fixture, fixture.bulb), 0.05f),
                  "the child rows should be found");
    CHECK_MESSAGE((fixture.children(fixture.lamp) == std::vector<SceneEntityId>{shade, fixture.bulb, wick} &&
                   fixture.history.size() == 1),
                  "dropping at a row's top edge should move the dragged object before it as one undo step");
    CHECK_MESSAGE((dragRow(fixture, entityRow(fixture, wick), entityRow(fixture, shade), 0.95f) &&
                   fixture.children(fixture.lamp) == std::vector<SceneEntityId>{shade, wick, fixture.bulb}),
                  "dropping at a row's bottom edge should move the dragged object after it");
    CHECK_MESSAGE((dragRow(fixture, entityRow(fixture, wick), entityRow(fixture, shade)) &&
                   fixture.parent(wick) == fixture.scene.findEntity(shade)),
                  "dropping on a row's middle should still reparent");
    CHECK_MESSAGE(fixture.history.size() == 3, "each drop should be one undo step");
}

TEST_CASE("Outliner: collection rename in place") {
    Fixture fixture;
    HeadlessImGuiContext imgui;
    ImGui::GetIO().ConfigInputTrickleEventQueue = false;
    fixture.draw();
    const std::optional<float> y = rowY(fixture, collectionRow(fixture, fixture.props));
    CHECK_MESSAGE(y.has_value(), "the collection row should be found");
    if (!y) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(rowX, *y);
    for (int click = 0; click < 2; ++click) {
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        fixture.draw();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        fixture.draw();
    }
    CHECK_MESSAGE(fixture.panel.renamingCollection() == fixture.props, "double-clicking a collection should rename it");
    fixture.draw();
    HeadlessImGuiContext::typeNextFrame("Shelves");
    fixture.draw();
    io.AddKeyEvent(ImGuiKey_Enter, true);
    fixture.draw();
    io.AddKeyEvent(ImGuiKey_Enter, false);
    fixture.draw();
    CHECK_MESSAGE((fixture.scene.collections().find(fixture.props)->name == "Shelves" &&
                   !fixture.panel.renamingCollection() && fixture.history.size() == 1),
                  ("Enter should rename the collection as one undo step: " +
                   fixture.scene.collections().find(fixture.props)->name));
}

TEST_CASE("Outliner: selected row cursor follows the accent") {
    HeadlessImGuiContext imgui;
    applyEditorColors(ImGui::GetStyle(), {.accent = ThemeAccent::Violet});
    bool accentDrawn = false;
    bool defaultDrawn = false;
    {
        HeadlessImGuiFrame frame("Accent");
        ImGui::TreeNodeEx("Row", ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const int before = drawList->VtxBuffer.Size;
        decorateOutlinerRow(EditorIcon::Empty, true, true);
        const ImU32 accent = widgets::color(editorAccent());
        const ImU32 defaultAccent = widgets::color(palette::accentBlue);
        for (int index = before; index < drawList->VtxBuffer.Size; ++index) {
            accentDrawn = accentDrawn || drawList->VtxBuffer[index].col == accent;
            defaultDrawn = defaultDrawn || drawList->VtxBuffer[index].col == defaultAccent;
        }
    }
    CHECK_MESSAGE((accentDrawn && !defaultDrawn),
                  "a selected row's cursor should use the chosen accent, not the default accent");
}
} // namespace

TEST_SUITE_END();
