// Placement speed without a window or GPU: surface normals of the CPU ray
// casts (negative scale included), placing under the mouse, model and prefab
// placement with align-to-normal through Adjust Last Operation, array and
// scatter (one paste, deterministic, re-runnable), and duplicates that keep
// their parents.

#include "EditorTestFixture.hpp"

#include "editor/document/DefaultScene.hpp"
#include "editor/document/SceneFragment.hpp"
#include "editor/interaction/EditorCamera.hpp"
#include "editor/operators/ObjectOperators.hpp"
#include "editor/operators/PlacementOperators.hpp"
#include "editor/operators/PrefabOperators.hpp"
#include "editor/viewport/SnapQueries.hpp"

#include "support/Doctest.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

TEST_SUITE_BEGIN("Placement");

namespace {

bool near(glm::vec3 left, glm::vec3 right, float tolerance = 0.001f) {
    return glm::length(left - right) <= tolerance;
}

bool near(float left, float right, float tolerance = 0.001f) {
    return std::abs(left - right) <= tolerance;
}

glm::mat4 at(glm::vec3 position) {
    return glm::translate(glm::mat4(1.0f), position);
}

glm::vec3 position(const glm::mat4& world) {
    return glm::vec3(world[3]);
}

size_t entityCount(const Scene& scene) {
    return scene.registry().view<const SceneEntityIdComponent>().size();
}

// A scene of default cubes in a fresh asset root (the fixture's own scratch
// directory), with the object, prefab, and placement operators.
struct Fixture : EditorTestFixture {
    std::filesystem::path root;
    // Half the default cube's size, read from its mesh.
    float half{0.5f};

    Fixture()
        : root(document.options().assetRoot) {
        std::filesystem::create_directories(root / defaultCubeModel.parent_path());
        std::filesystem::copy_file(std::filesystem::path(ZENITH_ASSET_DIR) / defaultCubeModel, root / defaultCubeModel);
        CHECK_MESSAGE((registerObjectOperators(operators).success && registerPrefabOperators(operators).success &&
                       registerPlacementOperators(operators).success),
                      "the object, prefab, and placement operators should register");
        const AssetLoadResult<ModelHandle> model = assets.loadModel(root / defaultCubeModel);
        const ModelAsset* asset = model ? assets.model(model.handle) : nullptr;
        const MeshAsset* mesh = asset != nullptr && !asset->nodes.empty() && asset->nodes.front().mesh
                                    ? assets.mesh(asset->nodes.front().mesh)
                                    : nullptr;
        CHECK_MESSAGE((mesh != nullptr && !mesh->localBox.empty()), "the default cube's mesh should load");
        if (mesh != nullptr && !mesh->localBox.empty()) {
            half = mesh->localBox.maximum.y;
        }
    }

    Entity cube(const glm::mat4& local) {
        Entity entity = entt::null;
        CHECK_MESSAGE(addCube(scene, assets, root, entity).success, "the test cube should load");
        if (entity != entt::null) {
            scene.registry().get<TransformComponent>(entity).local = local;
            scene.markTransformDirty(entity);
        }
        scene.updateWorldTransforms();
        return entity;
    }

    glm::mat4 world(SceneEntityId id) {
        scene.updateWorldTransforms();
        const Entity entity = scene.findEntity(id);
        return entity == entt::null ? glm::mat4(0.0f) : scene.registry().get<TransformComponent>(entity).world;
    }

    glm::mat4 activeWorld() {
        return world(selection.active().value_or(SceneEntityId{}));
    }

    // Re-runs the last operator with one property changed, as the Adjust
    // Last Operation panel does.
    OperatorResult adjust(std::string_view json) {
        const LastOperator* last = runner.lastOperator();
        if (last == nullptr) {
            return OperatorResult::failed("no last operator");
        }
        OperatorProperties properties = last->properties;
        CHECK_MESSAGE(properties.applyJson(json).empty(), "adjusted properties should apply");
        OperatorContext context = operatorContext();
        return runner.adjustLast(context, properties);
    }
};

// A lens at (0, 0, 10) looking down -Z.
SnapView frontView() {
    EditorCamera camera;
    camera.setPivot(glm::vec3(0.0f));
    camera.setDistance(10.0f);
    camera.setViewAngles(0.0f, 0.0f);
    return {.lens = camera.renderView(), .viewSize = {800.0f, 600.0f}};
}

TEST_CASE("Placement: hit normals") {
    Fixture fixture;
    const Entity plain = fixture.cube(at({0.0f, 0.0f, 0.0f}));
    const LensRay down{.origin = {0.0f, 10.0f, 0.0f}, .direction = {0.0f, -1.0f, 0.0f}};
    std::vector<LensHit> hits = pickAllAlongRay(fixture.scene, fixture.assets, down);
    CHECK_MESSAGE((hits.size() == 1 && hits.front().entity == plain && near(hits.front().normal, {0.0f, 1.0f, 0.0f})),
                  "a ray from above hits the cube's top face, whose normal points up");

    // Mirrored on Y, the cube's faces flip their winding; the normal must
    // still point out of the surface.
    fixture.scene.registry().get<TransformComponent>(plain).local =
        glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, -2.0f, 1.0f));
    fixture.scene.markTransformDirty(plain);
    fixture.scene.updateWorldTransforms();
    hits = pickAllAlongRay(fixture.scene, fixture.assets, down);
    CHECK_MESSAGE((hits.size() == 1 && near(hits.front().distance, 10.0f - 2.0f * fixture.half) &&
                   near(hits.front().normal, {0.0f, 1.0f, 0.0f})),
                  "under negative scale the hit normal still points out of the surface");

    // Turned 30 degrees about Z and stretched, the top face's normal turns
    // with it and stays unit length.
    const glm::mat4 turned = glm::rotate(glm::mat4(1.0f), glm::radians(30.0f), glm::vec3(0.0f, 0.0f, 1.0f)) *
                             glm::scale(glm::mat4(1.0f), glm::vec3(4.0f, 1.0f, 1.0f));
    fixture.scene.registry().get<TransformComponent>(plain).local = turned;
    fixture.scene.markTransformDirty(plain);
    fixture.scene.updateWorldTransforms();
    hits = pickAllAlongRay(fixture.scene, fixture.assets, down);
    const glm::vec3 expected = glm::normalize(glm::vec3(turned * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f)));
    CHECK_MESSAGE((hits.size() == 1 && near(hits.front().normal, expected, 0.01f) &&
                   near(glm::length(hits.front().normal), 1.0f)),
                  "a turned, non-uniformly scaled face reports its world normal");

    // Surface hits face the ray: from below, the bottom face points down.
    fixture.scene.registry().get<TransformComponent>(plain).local = glm::mat4(1.0f);
    fixture.scene.markTransformDirty(plain);
    fixture.scene.updateWorldTransforms();
    const std::optional<SurfaceHit> below = surfaceHitAlongRay(
        fixture.scene, fixture.assets, {.origin = {0.0f, -10.0f, 0.0f}, .direction = {0.0f, 1.0f, 0.0f}}, nullptr);
    CHECK_MESSAGE(
        (below && near(below->normal, {0.0f, -1.0f, 0.0f}) && near(below->position, {0.0f, -fixture.half, 0.0f})),
        "a surface hit from below reports the bottom face");
}

TEST_CASE("Placement: placement under mouse") {
    Fixture fixture;
    fixture.cube(at({0.0f, 0.0f, 0.0f}));
    const SnapView view = frontView();
    const Placement onCube = placementUnderMouse(fixture.scene, fixture.assets, view, {400.0f, 300.0f}, 5.0f);
    CHECK_MESSAGE((near(onCube.position, {0.0f, 0.0f, fixture.half}) && onCube.normal &&
                   near(*onCube.normal, {0.0f, 0.0f, 1.0f})),
                  "over a surface the placement is the hit point and its normal");
    const Placement empty = placementUnderMouse(fixture.scene, fixture.assets, view, {10.0f, 10.0f}, 4.0f);
    CHECK_MESSAGE(
        (!empty.normal && near(glm::dot(empty.position - view.lens.position, glm::normalize(view.lens.forward)), 4.0f)),
        "over empty space the placement falls back to the given depth, without a normal");
}

TEST_CASE("Placement: place model") {
    Fixture fixture;
    const std::string asset = (fixture.root / defaultCubeModel).generic_string();
    const size_t before = entityCount(fixture.scene);
    const OperatorResult placed =
        fixture.invoke("object.place_model", R"({"asset":")" + asset + R"(","location":[1,2,3],"normal":[1,0,0]})");
    CHECK_MESSAGE((static_cast<bool>(placed) && entityCount(fixture.scene) > before), "placing a model should finish");
    CHECK_MESSAGE((near(position(fixture.activeWorld()), {1.0f, 2.0f, 3.0f}) &&
                   near(glm::vec3(fixture.activeWorld()[1]), {0.0f, 1.0f, 0.0f})),
                  "a placed model stands upright at its location until aligned");
    CHECK_MESSAGE(fixture.history.size() == 1, "placing a model is one undo step");

    CHECK_MESSAGE((static_cast<bool>(fixture.adjust(R"({"align_to_normal":true})"))),
                  "Adjust Last Operation should re-run it");
    CHECK_MESSAGE((near(position(fixture.activeWorld()), {1.0f, 2.0f, 3.0f}) &&
                   near(glm::vec3(fixture.activeWorld()[1]), {1.0f, 0.0f, 0.0f})),
                  "aligning turns the model's up axis onto the surface normal");
    CHECK_MESSAGE(fixture.history.size() == 1, "an adjusted placement is still one undo step");
    CHECK_MESSAGE((static_cast<bool>(fixture.adjust(R"({"rotation":[0,90,0]})")) &&
                   near(glm::vec3(fixture.activeWorld()[1]), {1.0f, 0.0f, 0.0f}) &&
                   near(glm::vec3(fixture.activeWorld()[0]), {0.0f, 0.0f, -1.0f})),
                  "the rotation turns the model about its aligned up axis");
    fixture.editing().undo();
    CHECK_MESSAGE(entityCount(fixture.scene) == before, "undo removes the placed model");

    const OperatorResult missing =
        fixture.invoke("object.place_model", R"({"asset":"missing/nothing.gltf","location":[0,0,0]})");
    CHECK_MESSAGE((missing.status == OperatorStatus::Failed && entityCount(fixture.scene) == before),
                  "placing a missing model fails and leaves the scene unchanged");
    fixture.diagnostics.acknowledgeErrors();

    // Placement is document work: Play refuses it.
    fixture.mode = EditorMode::Play;
    CHECK_MESSAGE(fixture.invoke("object.place_model", R"({"asset":")" + asset + R"("})").status ==
                      OperatorStatus::Cancelled,
                  "models are placed only in Edit mode");
    fixture.mode = EditorMode::Edit;
}

TEST_CASE("Placement: instantiate aligned") {
    Fixture fixture;
    const Entity lamp = fixture.scene.createEntity("Lamp");
    fixture.selection.selectOnly(fixture.scene.id(lamp));
    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("prefab.create_from_selection", R"({"path":"lamp.prefab"})"))),
                  "the test prefab should be created");
    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("prefab.instantiate",
                                                    R"({"asset":"lamp.prefab","location":[0,1,0],"normal":[0,0,1]})"))),
                  "a prefab should instantiate at a location");
    CHECK_MESSAGE(near(glm::vec3(fixture.activeWorld()[1]), {0.0f, 1.0f, 0.0f}), "an unaligned instance stays upright");
    CHECK_MESSAGE((static_cast<bool>(fixture.adjust(R"({"align_to_normal":true})")) &&
                   near(position(fixture.activeWorld()), {0.0f, 1.0f, 0.0f}) &&
                   near(glm::vec3(fixture.activeWorld()[1]), {0.0f, 0.0f, 1.0f})),
                  "Adjust Last Operation aligns the instance to the surface normal");
}

TEST_CASE("Placement: array") {
    Fixture fixture;
    const Entity pillar = fixture.cube(at({0.0f, 0.0f, 0.0f}));
    const SceneEntityId pillarId = fixture.scene.id(pillar);
    fixture.selection.selectOnly(pillarId);
    const size_t before = entityCount(fixture.scene);
    CHECK_MESSAGE((static_cast<bool>(
                      fixture.invoke("object.array", R"({"count":3,"offset":[2,0,0],"relative_to_bounds":false})"))),
                  "an array should finish");
    CHECK_MESSAGE((entityCount(fixture.scene) == before + 3 && fixture.selection.size() == 3),
                  "an array adds and selects its copies");
    std::vector<float> xs;
    for (SceneEntityId id : fixture.selection.ids()) {
        xs.push_back(position(fixture.world(id)).x);
    }
    CHECK_MESSAGE((xs == std::vector<float>{2.0f, 4.0f, 6.0f}), "each copy steps one offset from the last");
    CHECK_MESSAGE(fixture.history.size() == 1, "an array is one undo step");

    CHECK_MESSAGE((static_cast<bool>(fixture.adjust(R"({"count":5})")) && entityCount(fixture.scene) == before + 5),
                  "Adjust Last Operation re-runs the array with a new count");
    CHECK_MESSAGE((static_cast<bool>(fixture.adjust(R"({"count":2,"offset":[1,0,0],"relative_to_bounds":true})")) &&
                   near(position(fixture.activeWorld()).x, 2.0f * fixture.half)),
                  "a relative offset steps by the selection's bounds");
    fixture.editing().undo();
    CHECK_MESSAGE(entityCount(fixture.scene) == before, "undo removes every copy at once");

    // A turning array curves: its copies stay a fixed distance apart.
    const std::vector<glm::mat4> start{at({0.0f, 0.0f, 0.0f})};
    const std::vector<std::vector<glm::mat4>> arc =
        arrayCopyTransforms(start, 4, {1.0f, 0.0f, 0.0f}, {0.0f, 90.0f, 0.0f}, 0.0f);
    CHECK_MESSAGE((arc.size() == 4 && near(position(arc[3][0]), {0.0f, 0.0f, 0.0f})),
                  "four quarter turns of a unit step close a square");
    CHECK_MESSAGE(arrayCopyTransforms(start, 4, {1.0f, 0.0f, 0.0f}, {0.0f, 90.0f, 0.0f}, 0.0f) == arc,
                  "the same array gives the same copies");

    // Every copy is one fragment, pasted once.
    std::string fragment;
    const std::vector<SceneEntityId> roots{pillarId};
    CHECK_MESSAGE(
        SceneFragment::copy(fixture.scene, fixture.assets, fixture.document.options(), roots, fragment).success,
        "the pillar should copy");
    std::string replicated;
    CHECK_MESSAGE(SceneFragment::replicate(fragment, fixture.document.options(), arc, replicated).success,
                  "the copies should replicate into one fragment");
    std::vector<SceneEntityId> fragmentRoots;
    CHECK_MESSAGE((SceneFragment::roots(replicated, fragmentRoots).success && fragmentRoots.size() == 4),
                  "one fragment holds every copy");
}

TEST_CASE("Placement: scatter") {
    Fixture fixture;
    // A floor whose top is at y = 0, and a prop resting on it.
    fixture.cube(at({0.0f, -fixture.half, 0.0f}) * glm::scale(glm::mat4(1.0f), glm::vec3(40.0f, 1.0f, 40.0f)));
    const Entity prop = fixture.cube(at({0.0f, 0.0f, 0.0f}) * glm::scale(glm::mat4(1.0f), glm::vec3(0.2f)));
    fixture.selection.selectOnly(fixture.scene.id(prop));
    const auto scatterPositions = [&fixture](std::string_view properties) {
        std::vector<glm::vec3> positions;
        if (fixture.invoke("object.scatter", properties)) {
            for (SceneEntityId id : fixture.selection.ids()) {
                positions.push_back(position(fixture.world(id)));
            }
        }
        return positions;
    };
    const std::vector<glm::vec3> first = scatterPositions(R"({"count":6,"radius":3,"seed":7})");
    CHECK_MESSAGE(first.size() == 6, "scatter adds and selects its copies");
    bool onFloor = !first.empty();
    for (const glm::vec3& point : first) {
        onFloor = onFloor && near(point.y, 0.0f) && glm::length(glm::vec2(point.x, point.z)) <= 3.0f + 1e-3f;
    }
    CHECK_MESSAGE(onFloor, "scattered copies land on the surface below, inside the radius");
    CHECK_MESSAGE(fixture.history.size() == 1, "scatter is one undo step");
    fixture.editing().undo();
    fixture.selection.selectOnly(fixture.scene.id(prop));
    CHECK_MESSAGE(scatterPositions(R"({"count":6,"radius":3,"seed":7})") == first,
                  "the same seed scatters the same way");
    fixture.editing().undo();
    fixture.selection.selectOnly(fixture.scene.id(prop));
    CHECK_MESSAGE(scatterPositions(R"({"count":6,"radius":3,"seed":8})") != first, "another seed scatters differently");
    fixture.editing().undo();
}

// On a slope, an aligned scatter stands each copy on the surface's normal.
TEST_CASE("Placement: scatter aligned") {
    Fixture fixture;
    const glm::mat4 tilt = glm::rotate(glm::mat4(1.0f), glm::radians(20.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    // A slab whose top face passes through the origin, tilted 20 degrees.
    fixture.cube(tilt * at({0.0f, -fixture.half, 0.0f}) * glm::scale(glm::mat4(1.0f), glm::vec3(40.0f, 1.0f, 40.0f)));
    const Entity prop = fixture.cube(glm::scale(glm::mat4(1.0f), glm::vec3(0.2f)));
    fixture.selection.selectOnly(fixture.scene.id(prop));
    CHECK_MESSAGE((static_cast<bool>(fixture.invoke(
                      "object.scatter", R"({"count":1,"radius":0,"align_to_surface":true,"random_yaw":false})"))),
                  "an aligned scatter should finish");
    const glm::vec3 slopeNormal = glm::vec3(tilt * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f));
    CHECK_MESSAGE((near(glm::normalize(glm::vec3(fixture.activeWorld()[1])), slopeNormal) &&
                   near(position(fixture.activeWorld()), {0.0f, 0.0f, 0.0f})),
                  "an aligned copy stands on the slope's normal");
}

// Duplicating several objects keeps each copy under its original's parent
// with the same local transform; a copied root pasted at the top level keeps
// its world transform.
TEST_CASE("Placement: duplicate keeps parents") {
    Fixture fixture;
    Scene& scene = fixture.scene;
    const Entity parent = scene.createEntity("Parent");
    scene.registry().get<TransformComponent>(parent).local = at({0.0f, 5.0f, 0.0f});
    const Entity left = scene.createEntity("Left");
    const Entity right = scene.createEntity("Right");
    const Entity loose = scene.createEntity("Loose");
    for (const Entity child : {left, right}) {
        scene.setParent(child, parent);
    }
    scene.registry().get<TransformComponent>(left).local = at({-1.0f, 0.0f, 0.0f});
    scene.registry().get<TransformComponent>(right).local = at({1.0f, 0.0f, 0.0f});
    scene.registry().get<TransformComponent>(loose).local = at({7.0f, 0.0f, 0.0f});
    for (const Entity entity : {parent, left, right, loose}) {
        scene.markTransformDirty(entity);
    }
    scene.updateWorldTransforms();
    const SceneEntityId parentId = scene.id(parent);
    fixture.selection.assign({scene.id(left), scene.id(right), scene.id(loose)});
    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("object.duplicate")) && fixture.selection.size() == 3),
                  "duplicating three objects selects three copies");
    const std::vector<SceneEntityId> copies = fixture.selection.ids();
    const auto parentOf = [&](SceneEntityId id) {
        const Entity entity = scene.findEntity(id);
        const Entity found =
            entity == entt::null ? entt::null : scene.registry().get<RelationshipComponent>(entity).parent;
        return found == entt::null ? SceneEntityId{} : scene.id(found);
    };
    CHECK_MESSAGE((copies.size() == 3 && parentOf(copies[0]) == parentId && parentOf(copies[1]) == parentId),
                  "copies of children keep their parent");
    CHECK_MESSAGE((copies.size() == 3 && near(position(fixture.world(copies[0])), {-1.0f, 5.0f, 0.0f}) &&
                   near(position(fixture.world(copies[1])), {1.0f, 5.0f, 0.0f})),
                  "copies of children land on their originals");
    CHECK_MESSAGE(
        (copies.size() == 3 && !parentOf(copies[2]) && near(position(fixture.world(copies[2])), {7.0f, 0.0f, 0.0f})),
        "a copy of a root stays a root on its original");

    // Pasting a copied child at the top level keeps where it was in the world.
    std::string fragment;
    const std::vector<SceneEntityId> roots{scene.id(scene.findEntity(copies[0]))};
    CHECK_MESSAGE(SceneFragment::copy(scene, fixture.assets, fixture.document.options(), roots, fragment).success,
                  "a child should copy");
    std::vector<SceneEntityId> pasted;
    CHECK_MESSAGE((SceneFragment::paste(fragment, scene, fixture.assets, fixture.document.options(), entt::null, pasted)
                       .success &&
                   pasted.size() == 1 && !parentOf(pasted.front()) &&
                   near(position(fixture.world(pasted.front())), {-1.0f, 5.0f, 0.0f})),
                  "a child pasted at the top level keeps its world transform");
}

// Duplicates are named Crate_1, Crate_2, ... counting up from the largest
// number any object of that base name has in the scene.
TEST_CASE("Placement: duplicate names") {
    Fixture fixture;
    Scene& scene = fixture.scene;
    const Entity crate = scene.createEntity("Crate");
    scene.createEntity("Crate_4");
    const auto nameOf = [&](SceneEntityId id) {
        return scene.registry().get<NameComponent>(scene.findEntity(id)).name;
    };

    fixture.selection.assign({scene.id(crate)});
    CHECK_MESSAGE((static_cast<bool>(fixture.invoke("object.duplicate")) && fixture.selection.size() == 1),
                  "duplicating Crate selects the copy");
    CHECK_MESSAGE(nameOf(fixture.selection.ids().front()) == "Crate_5",
                  "a duplicate takes the largest number in the scene plus one");
    CHECK_MESSAGE(
        (static_cast<bool>(fixture.invoke("object.duplicate")) && nameOf(fixture.selection.ids().front()) == "Crate_6"),
        "duplicating a numbered copy continues the count");
    CHECK_MESSAGE(nameOf(scene.id(crate)) == "Crate", "the original keeps its name");

    // A pasted copy of an object whose name nothing else has keeps it.
    std::string fragment;
    const std::vector<SceneEntityId> roots{scene.id(crate)};
    CHECK_MESSAGE(SceneFragment::copy(scene, fixture.assets, fixture.document.options(), roots, fragment).success,
                  "Crate should copy");
    scene.destroyEntity(crate);
    std::vector<SceneEntityId> pasted;
    CHECK_MESSAGE((SceneFragment::paste(fragment, scene, fixture.assets, fixture.document.options(), entt::null, pasted)
                       .success &&
                   pasted.size() == 1 && nameOf(pasted.front()) == "Crate"),
                  "a paste whose name is free keeps it");
    pasted.clear();
    CHECK_MESSAGE((SceneFragment::paste(fragment, scene, fixture.assets, fixture.document.options(), entt::null, pasted)
                       .success &&
                   pasted.size() == 1 && nameOf(pasted.front()) == "Crate_7"),
                  "pasting a name already in the scene numbers the copy");
}
} // namespace

TEST_SUITE_END();
