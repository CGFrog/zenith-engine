#include "EditorTestFixture.hpp"
#include "HeadlessImGui.hpp"

#include "editor/interaction/EntityPropertiesView.hpp"
#include "editor/interaction/MeshRendererField.hpp"
#include "editor/operators/ModelOperators.hpp"
#include "editor/panels/AssetDragPayloads.hpp"
#include "editor/project/ProjectAssets.hpp"
#include "engine/assets/ModelImporter.hpp"
#include "engine/physics/PhysicsComponents.hpp"
#include "engine/scene/SceneDocument.hpp"

#include "support/Doctest.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <string>
#include <variant>
#include <vector>

TEST_SUITE_BEGIN("MeshAssign");

// object.mesh_assign and the inspector's Mesh row: assigning a mesh with
// undo, redo, and a save/load round trip; several objects at once and adding
// a renderer; detaching model nodes and the skin and morph rules; the
// collider option; runtime meshes; the model outline the row names meshes
// and materials from; and the row's drop, combo, and runtime display.

namespace {

const std::filesystem::path assetRoot{ZENITH_ASSET_DIR};
constexpr const char* roomModel = "irradiance_room/room.gltf";

nlohmann::json targets(std::initializer_list<SceneEntityId> ids) {
    nlohmann::json list = nlohmann::json::array();
    for (const SceneEntityId id : ids) {
        list.push_back({{"entity", id.value}});
    }
    return list;
}

struct Fixture : EditorTestFixture {
    explicit Fixture(std::filesystem::path root = assetRoot)
        : EditorTestFixture(EditorTestFixtureOptions{.documentRoot = std::move(root)}) {
        CHECK_MESSAGE(registerModelOperators(operators).success, "the model operators should register");
    }

    OperatorResult assign(nlohmann::json entities, const std::string& asset, uint32_t mesh,
                          bool updateCollider = false) {
        return invoke("object.mesh_assign", nlohmann::json{{"entities", std::move(entities)},
                                                           {"asset", asset},
                                                           {"mesh", mesh},
                                                           {"update_collider", updateCollider}}
                                                .dump());
    }

    // Instantiates a model under a new entity and returns the root.
    Entity instance(const char* name, const std::filesystem::path& model) {
        const Entity root = scene.createEntity(name);
        const ModelHandle handle = assets.loadModel(assetRoot / model).handle;
        CHECK_MESSAGE(scene.instantiateModel(root, handle, assets).success,
                      (std::string(name) + " should instantiate"));
        return root;
    }

    // The first node entity of root's instance with the component.
    template <typename Component> Entity nodeWith(Entity root) {
        const SceneEntityId rootId = scene.id(root);
        for (const auto [entity, node] : scene.registry().view<const ModelNodeComponent>().each()) {
            if (node.instanceRoot == rootId && scene.registry().all_of<Component>(entity)) {
                return entity;
            }
        }
        return entt::null;
    }

    MeshHandle meshOf(SceneEntityId id) {
        const auto* renderer = scene.registry().try_get<MeshRendererComponent>(scene.findEntity(id));
        return renderer == nullptr ? MeshHandle{} : renderer->mesh;
    }

    MeshHandle load(const std::filesystem::path& model, uint32_t mesh) {
        return assets.loadMesh(assetRoot / model, mesh).handle;
    }

    template <typename Component> bool has(SceneEntityId id) {
        return scene.registry().all_of<Component>(scene.findEntity(id));
    }

    bool infoMentions(std::string_view text) {
        return std::ranges::any_of(diagnostics.entries(), [&](const EditorDiagnostic& entry) {
            return entry.severity == EditorDiagnosticSeverity::Info && entry.message.find(text) != std::string::npos;
        });
    }
};

TEST_CASE("MeshAssign: assign undo redo and round trip") {
    Fixture fixture;
    const Entity root = fixture.instance("Prop", "primitives/cube.gltf");
    const SceneEntityId node = fixture.scene.id(fixture.nodeWith<MeshRendererComponent>(root));
    const MeshHandle cube = fixture.meshOf(node);
    const MeshHandle wall = fixture.load(roomModel, 2);
    CHECK_MESSAGE((cube && wall), "the cube and room meshes should load");

    const OperatorResult result = fixture.assign(targets({node}), roomModel, 2);
    CHECK_MESSAGE(result.status == OperatorStatus::Finished, ("assigning a mesh should finish: " + result.message));
    CHECK_MESSAGE(fixture.meshOf(node) == wall, "the node should draw the room's third mesh");
    CHECK_MESSAGE((!fixture.has<ModelNodeComponent>(node)), "the node should leave its model instance");
    CHECK_MESSAGE(fixture.infoMentions("left its model instance"), "detaching should be reported in Info");
    CHECK_MESSAGE(fixture.history.size() == 1, "the assignment should be one undo step");

    fixture.editing().undo();
    CHECK_MESSAGE((fixture.meshOf(node) == cube && fixture.has<ModelNodeComponent>(node)),
                  "undo should restore the cube mesh and the model node");
    fixture.editing().redo();
    CHECK_MESSAGE((fixture.meshOf(node) == wall && !fixture.has<ModelNodeComponent>(node)),
                  "redo should assign it again");

    std::string saved;
    const SceneDocumentResult serialized =
        SceneDocument::serialize(fixture.scene, fixture.assets, fixture.document.options(), saved);
    CHECK_MESSAGE(serialized.success, ("the scene should save: " + serialized.error));
    Scene reloaded;
    const SceneDocumentResult loaded =
        SceneDocument::deserialize(saved, fixture.assets, fixture.document.options(), reloaded);
    CHECK_MESSAGE(loaded.success, ("the scene should load: " + loaded.error));
    const Entity reloadedNode = reloaded.findEntity(node);
    const auto* renderer =
        reloadedNode != entt::null ? reloaded.registry().try_get<MeshRendererComponent>(reloadedNode) : nullptr;
    CHECK_MESSAGE((renderer != nullptr && renderer->mesh == wall), "the assigned mesh should survive a save and load");
    CHECK_MESSAGE((reloadedNode != entt::null && !reloaded.registry().all_of<ModelNodeComponent>(reloadedNode)),
                  "the detached node should stay detached after loading");

    CHECK_MESSAGE(fixture.assign(targets({node}), roomModel, 2).status == OperatorStatus::Cancelled,
                  "assigning the mesh an object already draws should change nothing");
}

TEST_CASE("MeshAssign: several objects and adding renderers") {
    Fixture fixture;
    const SceneEntityId first = fixture.scene.id(fixture.scene.createEntity("First"));
    const SceneEntityId second = fixture.scene.id(fixture.scene.createEntity("Second"));
    const OperatorResult result = fixture.assign(targets({first, second}), roomModel, 6);
    CHECK_MESSAGE(result.status == OperatorStatus::Finished,
                  ("assigning to two objects should finish: " + result.message));
    const MeshHandle block = fixture.load(roomModel, 6);
    CHECK_MESSAGE((fixture.meshOf(first) == block && fixture.meshOf(second) == block),
                  "both empty objects should get a Mesh Renderer with the mesh");
    CHECK_MESSAGE(fixture.history.size() == 1, "assigning to several objects should be one undo step");
    fixture.editing().undo();
    CHECK_MESSAGE((!fixture.has<MeshRendererComponent>(first) && !fixture.has<MeshRendererComponent>(second)),
                  "undo should remove the added renderers");

    // Without listed entities, the selection gets the mesh.
    fixture.selection.assign({first, second});
    CHECK_MESSAGE(
        fixture.invoke("object.mesh_assign", nlohmann::json{{"asset", roomModel}, {"mesh", 7}}.dump()).status ==
            OperatorStatus::Finished,
        "invoking without entities should take the selection");
    CHECK_MESSAGE(
        (fixture.meshOf(first) == fixture.load(roomModel, 7) && fixture.meshOf(second) == fixture.load(roomModel, 7)),
        "every selected object should get the mesh");

    CHECK_MESSAGE(fixture.assign(targets({first}), "", 0).status == OperatorStatus::Cancelled,
                  "assigning without a model should change nothing");
    CHECK_MESSAGE(fixture.assign(targets({first}), roomModel, 99).status == OperatorStatus::Failed,
                  "a mesh index the model lacks should fail");
    CHECK_MESSAGE(fixture.assign(targets({first}), "missing.gltf", 0).status == OperatorStatus::Failed,
                  "a missing model should fail");
    const size_t steps = fixture.history.size();
    CHECK_MESSAGE(
        (fixture.assign(targets({first, SceneEntityId{999999}}), roomModel, 0).status == OperatorStatus::Failed &&
         fixture.meshOf(first) == fixture.load(roomModel, 7) && fixture.history.size() == steps),
        "a missing object should fail without changing the others");
    fixture.mode = EditorMode::Play;
    CHECK_MESSAGE(fixture.assign(targets({first}), roomModel, 0).status != OperatorStatus::Finished,
                  "meshes should be assigned in Edit mode only");
}

TEST_CASE("MeshAssign: skin rules") {
    Fixture fixture;
    const Entity root = fixture.instance("Tentacle", "showcase/coverage/tentacle.gltf");
    const Entity skinned = fixture.nodeWith<SkinComponent>(root);
    CHECK_MESSAGE((skinned != entt::null), "the tentacle should have a skinned node");
    if (skinned == entt::null) {
        return;
    }
    const SceneEntityId skinnedId = fixture.scene.id(skinned);
    const SkinComponent skin = fixture.scene.registry().get<SkinComponent>(skinned);
    const MeshHandle tentacle = fixture.meshOf(skinnedId);

    // A static mesh: the skin goes, so no stale palette is drawn with it.
    CHECK_MESSAGE(fixture.assign(targets({skinnedId}), "primitives/cube.gltf", 0).status == OperatorStatus::Finished,
                  "a skinned node should take a static mesh");
    CHECK_MESSAGE((!fixture.has<SkinComponent>(skinnedId) && !fixture.has<ModelNodeComponent>(skinnedId)),
                  "a static mesh should remove the skin and detach the node");
    CHECK_MESSAGE(fixture.infoMentions("Removed the skin"), "removing a skin should be reported in Info");
    fixture.editing().undo();
    CHECK_MESSAGE((fixture.has<SkinComponent>(skinnedId) && fixture.meshOf(skinnedId) == tentacle),
                  "undo should bring the skin back");

    // Another object skinned with the same skin keeps it for a mesh the model
    // draws with that skin.
    const Entity other = fixture.scene.createEntity("Second tentacle");
    fixture.scene.setParent(other, root);
    fixture.scene.attachMesh(other, fixture.load(roomModel, 0));
    fixture.scene.registry().emplace<SkinComponent>(other, skin.model, skin.instanceRoot, skin.skinIndex);
    const SceneEntityId otherId = fixture.scene.id(other);
    CHECK_MESSAGE(fixture.assign(targets({otherId}), "showcase/coverage/tentacle.gltf", 0).status ==
                      OperatorStatus::Finished,
                  "an object should take the skinned mesh");
    CHECK_MESSAGE((fixture.has<SkinComponent>(otherId) && fixture.meshOf(otherId) == tentacle),
                  "a mesh the model draws with the same skin should keep it");

    // A joint of the skeleton cannot leave the instance.
    const SceneEntityId rootId = fixture.scene.id(root);
    Entity joint = entt::null;
    for (const auto [entity, node] : fixture.scene.registry().view<const ModelNodeComponent>().each()) {
        if (node.instanceRoot == rootId && !fixture.scene.registry().all_of<MeshRendererComponent>(entity)) {
            joint = entity;
        }
    }
    CHECK_MESSAGE((joint != entt::null), "the tentacle should have joint nodes");
    const OperatorResult refused = fixture.assign(targets({fixture.scene.id(joint)}), "primitives/cube.gltf", 0);
    CHECK_MESSAGE((refused.status == OperatorStatus::Failed && refused.message.find("joint") != std::string::npos),
                  ("a skeleton joint should be refused: " + refused.message));
}

TEST_CASE("MeshAssign: morph rules") {
    Fixture fixture;
    const Entity root = fixture.instance("Morph", "morph_triangle.gltf");
    const Entity morphed = fixture.nodeWith<MorphWeightsComponent>(root);
    CHECK_MESSAGE((morphed != entt::null), "the morph triangle should have morph weights");
    if (morphed == entt::null) {
        return;
    }
    const SceneEntityId morphedId = fixture.scene.id(morphed);
    CHECK_MESSAGE(fixture.assign(targets({morphedId}), "primitives/cube.gltf", 0).status == OperatorStatus::Finished,
                  "a morphed node should take a mesh without targets");
    CHECK_MESSAGE((!fixture.has<MorphWeightsComponent>(morphedId)),
                  "a mesh without morph targets should drop the weights");

    const SceneEntityId plain = fixture.scene.id(fixture.scene.createEntity("Plain"));
    CHECK_MESSAGE(fixture.assign(targets({plain}), "morph_triangle.gltf", 0).status == OperatorStatus::Finished,
                  "an empty object should take a morphed mesh");
    const auto* weights = fixture.scene.registry().try_get<MorphWeightsComponent>(fixture.scene.findEntity(plain));
    CHECK_MESSAGE((weights != nullptr && weights->weights.size() == 1),
                  "a morphed mesh should bring one weight per target");
}

TEST_CASE("MeshAssign: collider option") {
    Fixture fixture;
    const Entity entity = fixture.scene.createEntity("Collidable");
    const SceneEntityId id = fixture.scene.id(entity);
    const MeshHandle cube = fixture.load("primitives/cube.gltf", 0);
    fixture.scene.attachMesh(entity, cube);
    fixture.scene.registry().emplace<ColliderComponent>(entity, ColliderComponent{.shape = TriangleMeshCollider{cube}});
    const auto colliderMesh = [&] {
        const auto& collider = fixture.scene.registry().get<ColliderComponent>(fixture.scene.findEntity(id));
        const auto* triangles = std::get_if<TriangleMeshCollider>(&collider.shape);
        return triangles == nullptr ? MeshHandle{} : triangles->mesh;
    };
    const MeshHandle wall = fixture.load(roomModel, 2);

    CHECK_MESSAGE(fixture.assign(targets({id}), roomModel, 2).status == OperatorStatus::Finished,
                  "the mesh should change");
    CHECK_MESSAGE(colliderMesh() == cube, "by default the collider should keep its mesh");
    fixture.editing().undo();
    CHECK_MESSAGE(fixture.assign(targets({id}), roomModel, 2, true).status == OperatorStatus::Finished,
                  "the mesh should change with the collider");
    CHECK_MESSAGE(colliderMesh() == wall, "update_collider should point the collider at the new mesh");
    fixture.editing().undo();
    CHECK_MESSAGE((colliderMesh() == cube && fixture.meshOf(id) == cube),
                  "undo should restore the collider and the mesh");
}

TEST_CASE("MeshAssign: runtime mesh") {
    Fixture fixture;
    const MeshAsset* cube = fixture.assets.mesh(fixture.load("primitives/cube.gltf", 0));
    const AssetLoadResult<MeshHandle> runtime = fixture.assets.createMesh(*cube, "runtime cube");
    CHECK_MESSAGE((static_cast<bool>(runtime)), ("a runtime mesh should be created: " + runtime.error));
    const Entity entity = fixture.scene.createEntity("Procedural");
    fixture.scene.attachMesh(entity, runtime.handle);
    const SceneEntityId id = fixture.scene.id(entity);
    // A scene holding a runtime mesh cannot be saved, so it cannot be
    // recorded for undo either: the assignment is refused and changes
    // nothing.
    const OperatorResult result = fixture.assign(targets({id}), roomModel, 0);
    CHECK_MESSAGE((result.status == OperatorStatus::Failed && fixture.meshOf(id) == runtime.handle &&
                   fixture.history.size() == 0),
                  ("replacing a runtime mesh should be refused unchanged: " + result.message));
}

TEST_CASE("MeshAssign: outline") {
    const ModelOutline room = readModelOutline(assetRoot / roomModel);
    CHECK_MESSAGE((static_cast<bool>(room) && room.meshes.size() == 8), "the room should list eight meshes");
    const std::vector<MeshChoice> choices = meshChoices(room);
    CHECK_MESSAGE((choices.size() == 8 && choices.front().label == "Floor" && choices[6].label == "Block" &&
                   choices[6].index == 6),
                  "the room's meshes should be offered by name and index");
    const ModelOutline tentacle = readModelOutline(assetRoot / "showcase" / "coverage" / "tentacle.gltf");
    CHECK_MESSAGE(meshChoices(tentacle).front().label == "Tentacle (skinned)", "a skinned mesh should be marked");
    CHECK_MESSAGE(meshChoices(readModelOutline(assetRoot / "morph_triangle.gltf")).front().label == "Mesh 0 (morph)",
                  "an unnamed morphed mesh should be named by index and marked");
    const ModelOutline gallery = readModelOutline(assetRoot / "showcase" / "materials" / "material_gallery.gltf");
    const std::vector<std::string> slots = materialSlotNames(gallery, 0);
    CHECK_MESSAGE((slots.size() == 16 && slots[0] == "CarPaintRed" && slots[2] == "Velvet"),
                  "every primitive's material should be named");
    CHECK_MESSAGE((materialSlotNames(readModelOutline(assetRoot / "morph_triangle.gltf"), 0) ==
                   std::vector<std::string>{"Default material"}),
                  "a primitive without a material should show the default");
    CHECK_MESSAGE(materialSlotNames(gallery, 5).empty(), "a missing mesh should have no slots");
    CHECK_MESSAGE(!readModelOutline(assetRoot / "missing.gltf"), "a missing file should report an error");
}

// The Mesh row against a project holding the room model.
struct WidgetFixture : Fixture {
    std::filesystem::path root;
    std::unique_ptr<ProjectAssets> project;
    EntityPropertiesView view{componentLayout};

    // The project lives in the fixture's own scratch directory, which is also
    // the document's root.
    WidgetFixture()
        : Fixture(std::filesystem::path{})
        , root(scratch.path()) {
        std::filesystem::create_directories(root / "models");
        std::filesystem::copy_file(assetRoot / roomModel, root / "models" / "room.gltf");
        project = std::make_unique<ProjectAssets>(root, root, root / "asset-browser.json",
                                                  AssetCatalogOptions{.watchFiles = false, .guidSeed = 11});
        project->refresh(diagnostics);
        projectAssets = project.get();
    }

    ~WidgetFixture() {
        projectAssets = nullptr;
        project.reset();
    }

    WidgetFixture(const WidgetFixture&) = delete;
    WidgetFixture& operator=(const WidgetFixture&) = delete;

    void draw(SceneEntityId entity, const std::function<void()>& before = {}) {
        HeadlessImGuiFrame frame("Mesh row test");
        if (before) {
            before();
        }
        EditorContext editor = context();
        view.drawModelField(editor, entity);
    }

    MeshHandle roomMesh(uint32_t mesh) {
        return assets.loadMesh(root / "models" / "room.gltf", mesh).handle;
    }
};

ImGuiID rowItem(const char* item) {
    const ImGuiWindow* window = HeadlessImGuiContext::findWindow("Mesh row test");
    return window == nullptr ? 0 : HeadlessImGuiContext::itemId(*window, "##MeshField", item);
}

bool hoverItem(ImGuiID id, const std::function<void()>& draw) {
    ImGuiIO& io = ImGui::GetIO();
    for (float y = 0.0f; y < io.DisplaySize.y; y += 2.0f) {
        io.AddMousePosEvent(60.0f, y);
        draw();
        if (ImGui::GetCurrentContext()->HoveredId == id) {
            return true;
        }
    }
    return false;
}

TEST_CASE("MeshAssign: row drop combo and runtime") {
    WidgetFixture fixture;
    HeadlessImGuiContext imgui;
    ImGui::GetIO().ConfigInputTrickleEventQueue = false;
    const SceneEntityId empty = fixture.scene.id(fixture.scene.createEntity("Empty"));
    fixture.selection.selectOnly(empty);
    const auto draw = [&](const std::function<void()>& before) { fixture.draw(empty, before); };
    draw({});
    CHECK_MESSAGE(hoverItem(rowItem("##value"), [&] { draw({}); }), "an empty object should show the Mesh row");

    // Dropping a model adds a renderer with its first mesh.
    const std::u8string text = (fixture.root / "models" / "room.gltf").generic_u8string();
    const std::string path(text.begin(), text.end());
    draw([&] {
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceExtern | ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
            ImGui::SetDragDropPayload(modelAssetPayload, path.c_str(), path.size() + 1);
            ImGui::EndDragDropSource();
        }
    });
    draw({});
    CHECK_MESSAGE(fixture.meshOf(empty) == fixture.roomMesh(0), "dropping a model should add its first mesh");
    CHECK_MESSAGE(fixture.history.size() == 1, "the drop should be one undo step");

    // Choosing the room's Block from the mesh combo.
    HeadlessImGuiContext::activateNextFrame(rowItem("Mesh##index"));
    draw({});
    draw({});
    const ImGuiWindow* popup = HeadlessImGuiContext::findWindow("##Combo_");
    CHECK_MESSAGE(popup != nullptr, "activating the mesh combo should open it");
    if (popup != nullptr) {
        const int block = 6;
        const ImGuiID scope = ImHashData(&block, sizeof(block), popup->ID);
        HeadlessImGuiContext::activateNextFrame(ImHashStr("Block", 0, scope));
        draw({});
        draw({});
        CHECK_MESSAGE(fixture.meshOf(empty) == fixture.roomMesh(6),
                      "choosing Block should assign the room's seventh mesh");
        CHECK_MESSAGE(fixture.history.size() == 2, "the choice should be one more undo step");
    }

    // A runtime mesh shows read-only and offers no picks.
    const MeshAsset* source = fixture.assets.mesh(fixture.roomMesh(0));
    const MeshHandle runtime = fixture.assets.createMesh(*source, "runtime floor").handle;
    fixture.scene.attachMesh(fixture.scene.findEntity(empty), runtime);
    draw({});
    HeadlessImGuiContext::activateNextFrame(rowItem("##value"));
    draw({});
    draw({});
    CHECK_MESSAGE((fixture.meshOf(empty) == runtime && fixture.history.size() == 2),
                  "a runtime mesh should show without a picker");
}
} // namespace

TEST_SUITE_END();
