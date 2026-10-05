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

// object.mesh_assign and the inspector's Mesh row: assigning a mesh with
// undo, redo, and a save/load round trip; several objects at once and adding
// a renderer; detaching model nodes and the skin and morph rules; the
// collider option; runtime meshes; the model outline the row names meshes
// and materials from; and the row's drop, combo, and runtime display.

namespace {
int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

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
        expect(registerModelOperators(operators).success, "the model operators should register");
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
        expect(scene.instantiateModel(root, handle, assets).success, std::string(name) + " should instantiate");
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

void testAssignUndoRedoAndRoundTrip() {
    Fixture fixture;
    const Entity root = fixture.instance("Prop", "primitives/cube.gltf");
    const SceneEntityId node = fixture.scene.id(fixture.nodeWith<MeshRendererComponent>(root));
    const MeshHandle cube = fixture.meshOf(node);
    const MeshHandle wall = fixture.load(roomModel, 2);
    expect(cube && wall, "the cube and room meshes should load");

    const OperatorResult result = fixture.assign(targets({node}), roomModel, 2);
    expect(result.status == OperatorStatus::Finished, "assigning a mesh should finish: " + result.message);
    expect(fixture.meshOf(node) == wall, "the node should draw the room's third mesh");
    expect(!fixture.has<ModelNodeComponent>(node), "the node should leave its model instance");
    expect(fixture.infoMentions("left its model instance"), "detaching should be reported in Info");
    expect(fixture.history.size() == 1, "the assignment should be one undo step");

    fixture.editing().undo();
    expect(fixture.meshOf(node) == cube && fixture.has<ModelNodeComponent>(node),
           "undo should restore the cube mesh and the model node");
    fixture.editing().redo();
    expect(fixture.meshOf(node) == wall && !fixture.has<ModelNodeComponent>(node), "redo should assign it again");

    std::string saved;
    const SceneDocumentResult serialized =
        SceneDocument::serialize(fixture.scene, fixture.assets, fixture.document.options(), saved);
    expect(serialized.success, "the scene should save: " + serialized.error);
    Scene reloaded;
    const SceneDocumentResult loaded =
        SceneDocument::deserialize(saved, fixture.assets, fixture.document.options(), reloaded);
    expect(loaded.success, "the scene should load: " + loaded.error);
    const Entity reloadedNode = reloaded.findEntity(node);
    const auto* renderer =
        reloadedNode != entt::null ? reloaded.registry().try_get<MeshRendererComponent>(reloadedNode) : nullptr;
    expect(renderer != nullptr && renderer->mesh == wall, "the assigned mesh should survive a save and load");
    expect(reloadedNode != entt::null && !reloaded.registry().all_of<ModelNodeComponent>(reloadedNode),
           "the detached node should stay detached after loading");

    expect(fixture.assign(targets({node}), roomModel, 2).status == OperatorStatus::Cancelled,
           "assigning the mesh an object already draws should change nothing");
}

void testSeveralObjectsAndAddingRenderers() {
    Fixture fixture;
    const SceneEntityId first = fixture.scene.id(fixture.scene.createEntity("First"));
    const SceneEntityId second = fixture.scene.id(fixture.scene.createEntity("Second"));
    const OperatorResult result = fixture.assign(targets({first, second}), roomModel, 6);
    expect(result.status == OperatorStatus::Finished, "assigning to two objects should finish: " + result.message);
    const MeshHandle block = fixture.load(roomModel, 6);
    expect(fixture.meshOf(first) == block && fixture.meshOf(second) == block,
           "both empty objects should get a Mesh Renderer with the mesh");
    expect(fixture.history.size() == 1, "assigning to several objects should be one undo step");
    fixture.editing().undo();
    expect(!fixture.has<MeshRendererComponent>(first) && !fixture.has<MeshRendererComponent>(second),
           "undo should remove the added renderers");

    // Without listed entities, the selection gets the mesh.
    fixture.selection.assign({first, second});
    expect(fixture.invoke("object.mesh_assign", nlohmann::json{{"asset", roomModel}, {"mesh", 7}}.dump()).status ==
               OperatorStatus::Finished,
           "invoking without entities should take the selection");
    expect(fixture.meshOf(first) == fixture.load(roomModel, 7) && fixture.meshOf(second) == fixture.load(roomModel, 7),
           "every selected object should get the mesh");

    expect(fixture.assign(targets({first}), "", 0).status == OperatorStatus::Cancelled,
           "assigning without a model should change nothing");
    expect(fixture.assign(targets({first}), roomModel, 99).status == OperatorStatus::Failed,
           "a mesh index the model lacks should fail");
    expect(fixture.assign(targets({first}), "missing.gltf", 0).status == OperatorStatus::Failed,
           "a missing model should fail");
    const size_t steps = fixture.history.size();
    expect(fixture.assign(targets({first, SceneEntityId{999999}}), roomModel, 0).status == OperatorStatus::Failed &&
               fixture.meshOf(first) == fixture.load(roomModel, 7) && fixture.history.size() == steps,
           "a missing object should fail without changing the others");
    fixture.mode = EditorMode::Play;
    expect(fixture.assign(targets({first}), roomModel, 0).status != OperatorStatus::Finished,
           "meshes should be assigned in Edit mode only");
}

void testSkinRules() {
    Fixture fixture;
    const Entity root = fixture.instance("Tentacle", "showcase/coverage/tentacle.gltf");
    const Entity skinned = fixture.nodeWith<SkinComponent>(root);
    expect(skinned != entt::null, "the tentacle should have a skinned node");
    if (skinned == entt::null) {
        return;
    }
    const SceneEntityId skinnedId = fixture.scene.id(skinned);
    const SkinComponent skin = fixture.scene.registry().get<SkinComponent>(skinned);
    const MeshHandle tentacle = fixture.meshOf(skinnedId);

    // A static mesh: the skin goes, so no stale palette is drawn with it.
    expect(fixture.assign(targets({skinnedId}), "primitives/cube.gltf", 0).status == OperatorStatus::Finished,
           "a skinned node should take a static mesh");
    expect(!fixture.has<SkinComponent>(skinnedId) && !fixture.has<ModelNodeComponent>(skinnedId),
           "a static mesh should remove the skin and detach the node");
    expect(fixture.infoMentions("Removed the skin"), "removing a skin should be reported in Info");
    fixture.editing().undo();
    expect(fixture.has<SkinComponent>(skinnedId) && fixture.meshOf(skinnedId) == tentacle,
           "undo should bring the skin back");

    // Another object skinned with the same skin keeps it for a mesh the model
    // draws with that skin.
    const Entity other = fixture.scene.createEntity("Second tentacle");
    fixture.scene.setParent(other, root);
    fixture.scene.attachMesh(other, fixture.load(roomModel, 0));
    fixture.scene.registry().emplace<SkinComponent>(other, skin.model, skin.instanceRoot, skin.skinIndex);
    const SceneEntityId otherId = fixture.scene.id(other);
    expect(fixture.assign(targets({otherId}), "showcase/coverage/tentacle.gltf", 0).status == OperatorStatus::Finished,
           "an object should take the skinned mesh");
    expect(fixture.has<SkinComponent>(otherId) && fixture.meshOf(otherId) == tentacle,
           "a mesh the model draws with the same skin should keep it");

    // A joint of the skeleton cannot leave the instance.
    const SceneEntityId rootId = fixture.scene.id(root);
    Entity joint = entt::null;
    for (const auto [entity, node] : fixture.scene.registry().view<const ModelNodeComponent>().each()) {
        if (node.instanceRoot == rootId && !fixture.scene.registry().all_of<MeshRendererComponent>(entity)) {
            joint = entity;
        }
    }
    expect(joint != entt::null, "the tentacle should have joint nodes");
    const OperatorResult refused = fixture.assign(targets({fixture.scene.id(joint)}), "primitives/cube.gltf", 0);
    expect(refused.status == OperatorStatus::Failed && refused.message.find("joint") != std::string::npos,
           "a skeleton joint should be refused: " + refused.message);
}

void testMorphRules() {
    Fixture fixture;
    const Entity root = fixture.instance("Morph", "morph_triangle.gltf");
    const Entity morphed = fixture.nodeWith<MorphWeightsComponent>(root);
    expect(morphed != entt::null, "the morph triangle should have morph weights");
    if (morphed == entt::null) {
        return;
    }
    const SceneEntityId morphedId = fixture.scene.id(morphed);
    expect(fixture.assign(targets({morphedId}), "primitives/cube.gltf", 0).status == OperatorStatus::Finished,
           "a morphed node should take a mesh without targets");
    expect(!fixture.has<MorphWeightsComponent>(morphedId), "a mesh without morph targets should drop the weights");

    const SceneEntityId plain = fixture.scene.id(fixture.scene.createEntity("Plain"));
    expect(fixture.assign(targets({plain}), "morph_triangle.gltf", 0).status == OperatorStatus::Finished,
           "an empty object should take a morphed mesh");
    const auto* weights = fixture.scene.registry().try_get<MorphWeightsComponent>(fixture.scene.findEntity(plain));
    expect(weights != nullptr && weights->weights.size() == 1, "a morphed mesh should bring one weight per target");
}

void testColliderOption() {
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

    expect(fixture.assign(targets({id}), roomModel, 2).status == OperatorStatus::Finished, "the mesh should change");
    expect(colliderMesh() == cube, "by default the collider should keep its mesh");
    fixture.editing().undo();
    expect(fixture.assign(targets({id}), roomModel, 2, true).status == OperatorStatus::Finished,
           "the mesh should change with the collider");
    expect(colliderMesh() == wall, "update_collider should point the collider at the new mesh");
    fixture.editing().undo();
    expect(colliderMesh() == cube && fixture.meshOf(id) == cube, "undo should restore the collider and the mesh");
}

void testRuntimeMesh() {
    Fixture fixture;
    const MeshAsset* cube = fixture.assets.mesh(fixture.load("primitives/cube.gltf", 0));
    const AssetLoadResult<MeshHandle> runtime = fixture.assets.createMesh(*cube, "runtime cube");
    expect(static_cast<bool>(runtime), "a runtime mesh should be created: " + runtime.error);
    const Entity entity = fixture.scene.createEntity("Procedural");
    fixture.scene.attachMesh(entity, runtime.handle);
    const SceneEntityId id = fixture.scene.id(entity);
    // A scene holding a runtime mesh cannot be saved, so it cannot be
    // recorded for undo either: the assignment is refused and changes
    // nothing.
    const OperatorResult result = fixture.assign(targets({id}), roomModel, 0);
    expect(result.status == OperatorStatus::Failed && fixture.meshOf(id) == runtime.handle &&
               fixture.history.size() == 0,
           "replacing a runtime mesh should be refused unchanged: " + result.message);
}

void testOutline() {
    const ModelOutline room = readModelOutline(assetRoot / roomModel);
    expect(static_cast<bool>(room) && room.meshes.size() == 8, "the room should list eight meshes");
    const std::vector<MeshChoice> choices = meshChoices(room);
    expect(choices.size() == 8 && choices.front().label == "Floor" && choices[6].label == "Block" &&
               choices[6].index == 6,
           "the room's meshes should be offered by name and index");
    const ModelOutline tentacle = readModelOutline(assetRoot / "showcase" / "coverage" / "tentacle.gltf");
    expect(meshChoices(tentacle).front().label == "Tentacle (skinned)", "a skinned mesh should be marked");
    expect(meshChoices(readModelOutline(assetRoot / "morph_triangle.gltf")).front().label == "Mesh 0 (morph)",
           "an unnamed morphed mesh should be named by index and marked");
    const ModelOutline gallery = readModelOutline(assetRoot / "showcase" / "materials" / "material_gallery.gltf");
    const std::vector<std::string> slots = materialSlotNames(gallery, 0);
    expect(slots.size() == 16 && slots[0] == "CarPaintRed" && slots[2] == "Velvet",
           "every primitive's material should be named");
    expect(materialSlotNames(readModelOutline(assetRoot / "morph_triangle.gltf"), 0) ==
               std::vector<std::string>{"Default material"},
           "a primitive without a material should show the default");
    expect(materialSlotNames(gallery, 5).empty(), "a missing mesh should have no slots");
    expect(!readModelOutline(assetRoot / "missing.gltf"), "a missing file should report an error");
}

// The Mesh row against a project holding the room model.
struct WidgetFixture : Fixture {
    std::filesystem::path root;
    std::unique_ptr<ProjectAssets> project;
    EntityPropertiesView view{componentLayout};

    explicit WidgetFixture(const char* name)
        : Fixture(std::filesystem::temp_directory_path() / name)
        , root(std::filesystem::temp_directory_path() / name) {
        std::filesystem::remove_all(root);
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
        std::error_code error;
        std::filesystem::remove_all(root, error);
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

void testRowDropComboAndRuntime() {
    WidgetFixture fixture("zenith_mesh_row");
    HeadlessImGuiContext imgui;
    ImGui::GetIO().ConfigInputTrickleEventQueue = false;
    const SceneEntityId empty = fixture.scene.id(fixture.scene.createEntity("Empty"));
    fixture.selection.selectOnly(empty);
    const auto draw = [&](const std::function<void()>& before) { fixture.draw(empty, before); };
    draw({});
    expect(hoverItem(rowItem("##value"), [&] { draw({}); }), "an empty object should show the Mesh row");

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
    expect(fixture.meshOf(empty) == fixture.roomMesh(0), "dropping a model should add its first mesh");
    expect(fixture.history.size() == 1, "the drop should be one undo step");

    // Choosing the room's Block from the mesh combo.
    HeadlessImGuiContext::activateNextFrame(rowItem("Mesh##index"));
    draw({});
    draw({});
    const ImGuiWindow* popup = HeadlessImGuiContext::findWindow("##Combo_");
    expect(popup != nullptr, "activating the mesh combo should open it");
    if (popup != nullptr) {
        const int block = 6;
        const ImGuiID scope = ImHashData(&block, sizeof(block), popup->ID);
        HeadlessImGuiContext::activateNextFrame(ImHashStr("Block", 0, scope));
        draw({});
        draw({});
        expect(fixture.meshOf(empty) == fixture.roomMesh(6), "choosing Block should assign the room's seventh mesh");
        expect(fixture.history.size() == 2, "the choice should be one more undo step");
    }

    // A runtime mesh shows read-only and offers no picks.
    const MeshAsset* source = fixture.assets.mesh(fixture.roomMesh(0));
    const MeshHandle runtime = fixture.assets.createMesh(*source, "runtime floor").handle;
    fixture.scene.attachMesh(fixture.scene.findEntity(empty), runtime);
    draw({});
    HeadlessImGuiContext::activateNextFrame(rowItem("##value"));
    draw({});
    draw({});
    expect(fixture.meshOf(empty) == runtime && fixture.history.size() == 2,
           "a runtime mesh should show without a picker");
}
} // namespace

int main() {
    testAssignUndoRedoAndRoundTrip();
    testSeveralObjectsAndAddingRenderers();
    testSkinRules();
    testMorphRules();
    testColliderOption();
    testRuntimeMesh();
    testOutline();
    testRowDropComboAndRuntime();
    if (failures != 0) {
        std::cerr << failures << " mesh assign test(s) failed\n";
        return 1;
    }
    std::cout << "Mesh assign tests passed\n";
    return 0;
}
