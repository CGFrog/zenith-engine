// Selection tools without a window or GPU: select hierarchy and select
// grouped, and clicking objects without a mesh by their overlay marks.

#include "EditorTestFixture.hpp"

#include "editor/document/DefaultScene.hpp"
#include "editor/interaction/EditorCamera.hpp"
#include "editor/operators/SelectOperators.hpp"
#include "editor/viewport/IconPicking.hpp"
#include "editor/viewport/ViewportPicker.hpp"
#include "engine/audio/AudioSystem.hpp"
#include "engine/scene/Collections.hpp"
#include "engine/scene/Tags.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

glm::mat4 at(glm::vec3 position) {
    return glm::translate(glm::mat4(1.0f), position);
}

bool selected(const Selection& selection, std::initializer_list<SceneEntityId> ids) {
    std::vector<SceneEntityId> expected(ids);
    std::vector<SceneEntityId> actual = selection.ids();
    std::ranges::sort(expected, {}, &SceneEntityId::value);
    std::ranges::sort(actual, {}, &SceneEntityId::value);
    return expected == actual;
}

struct Fixture : EditorTestFixture {
    std::filesystem::path root;

    Fixture()
        : EditorTestFixture({.documentRoot = std::filesystem::temp_directory_path() / "zenith-selection-tools-tests"})
        , root(document.options().assetRoot) {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root / defaultCubeModel.parent_path());
        std::filesystem::copy_file(std::filesystem::path(ZENITH_ASSET_DIR) / defaultCubeModel, root / defaultCubeModel);
        expect(registerSelectOperators(operators).success, "the select operators should register");
    }

    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    SceneEntityId entity(const char* name, Entity parent = entt::null) {
        const Entity created = scene.createEntity(name);
        if (parent != entt::null) {
            scene.setParent(created, parent);
        }
        return scene.id(created);
    }

    SceneEntityId cube(glm::vec3 location) {
        Entity created = entt::null;
        expect(addCube(scene, assets, root, created).success, "the test cube should load");
        scene.registry().get<TransformComponent>(created).local = at(location);
        scene.markTransformDirty(created);
        scene.updateWorldTransforms();
        return scene.id(created);
    }

    Entity find(SceneEntityId id) const {
        return scene.findEntity(id);
    }
};

void testSelectHierarchy() {
    Fixture fixture;
    const SceneEntityId parent = fixture.entity("Parent");
    const SceneEntityId first = fixture.entity("First", fixture.find(parent));
    const SceneEntityId second = fixture.entity("Second", fixture.find(parent));
    const SceneEntityId grandchild = fixture.entity("Grandchild", fixture.find(first));
    const SceneEntityId hidden = fixture.entity("Hidden", fixture.find(parent));
    fixture.scene.registry().emplace<VisibilityComponent>(fixture.find(hidden)).hidden = true;

    fixture.selection.selectOnly(parent);
    expect(static_cast<bool>(fixture.invoke("object.select_hierarchy", R"({"direction":"children"})")) &&
               selected(fixture.selection, {first, second}) && fixture.selection.active() == first,
           "] selects the direct, selectable children, the first one active");
    expect(fixture.history.size() == 0, "selection operators record no undo step");
    expect(static_cast<bool>(fixture.invoke("object.select_hierarchy", R"({"direction":"parent"})")) &&
               selected(fixture.selection, {parent}),
           "[ selects the parents");
    expect(static_cast<bool>(fixture.invoke("object.select_hierarchy", R"({"direction":"children","extend":true})")) &&
               selected(fixture.selection, {parent, first, second}),
           "extend adds the children to the selection");
    fixture.selection.selectOnly(grandchild);
    expect(fixture.invoke("object.select_hierarchy", R"({"direction":"children"})").status ==
                   OperatorStatus::Cancelled &&
               selected(fixture.selection, {grandchild}),
           "finding nothing leaves the selection alone");

    fixture.selection.selectOnly(parent);
    expect(static_cast<bool>(fixture.invoke("object.select_grouped", R"({"type":"children"})")) &&
               selected(fixture.selection, {first, second, grandchild}),
           "grouped children are every selectable descendant");
    fixture.selection.selectOnly(parent);
    expect(static_cast<bool>(fixture.invoke("object.select_grouped", R"({"type":"immediate_children"})")) &&
               selected(fixture.selection, {first, second}),
           "immediate children are the direct ones");
}

void testSelectGrouped() {
    Fixture fixture;
    const SceneEntityId cubeA = fixture.cube({0.0f, 0.0f, 0.0f});
    const SceneEntityId cubeB = fixture.cube({2.0f, 0.0f, 0.0f});
    const SceneEntityId lampA = fixture.entity("Lamp A");
    const SceneEntityId lampB = fixture.entity("Lamp B");
    const SceneEntityId plain = fixture.entity("Plain");
    entt::registry& registry = fixture.scene.registry();
    for (const SceneEntityId lamp : {lampA, lampB}) {
        registry.emplace<PointLightComponent>(fixture.find(lamp));
    }
    registry.emplace<TagsComponent>(fixture.find(lampA)).tags = {"torch", "warm"};
    registry.emplace<TagsComponent>(fixture.find(cubeB)).tags = {"warm"};
    registry.emplace<TagsComponent>(fixture.find(plain)).tags = {"cold"};

    fixture.selection.selectOnly(cubeA);
    expect(static_cast<bool>(fixture.invoke("object.select_grouped", R"({"type":"mesh"})")) &&
               selected(fixture.selection, {cubeA, cubeB}) && fixture.selection.active() == cubeA,
           "mesh selects the objects drawing the same mesh, keeping the active one");
    fixture.selection.selectOnly(lampB);
    expect(static_cast<bool>(fixture.invoke("object.select_grouped", R"({"type":"components"})")) &&
               selected(fixture.selection, {lampA, lampB}),
           "components selects the objects with every component type the active one has");
    fixture.selection.selectOnly(lampA);
    expect(static_cast<bool>(fixture.invoke("object.select_grouped", R"({"type":"tag"})")) &&
               selected(fixture.selection, {lampA, cubeB}),
           "tag selects the objects sharing any tag");
    fixture.selection.selectOnly(plain);
    expect(fixture.invoke("object.select_grouped", R"({"type":"tag"})").status == OperatorStatus::Cancelled &&
               selected(fixture.selection, {plain}),
           "a group of one cancels and keeps the selection");

    SceneCollections collections = fixture.scene.collections();
    SceneCollectionId props;
    expect(static_cast<bool>(collections.create("Props", SceneCollectionId{}, props)),
           "a collection should be created");
    fixture.scene.setCollections(std::move(collections));
    for (const SceneEntityId id : {cubeB, plain}) {
        registry.emplace_or_replace<CollectionMembershipComponent>(fixture.find(id)).insert(props);
    }
    fixture.selection.assign({plain, cubeA});
    expect(static_cast<bool>(fixture.invoke("object.select_grouped", R"({"type":"collection","extend":true})")) &&
               selected(fixture.selection, {plain, cubeB, cubeA}),
           "collection with extend adds the active object's collection mates");
    fixture.selection.clear();
    expect(fixture.invoke("object.select_grouped", R"({"type":"mesh"})").status == OperatorStatus::Cancelled,
           "select grouped needs an active object");
}

// A lens at (0, 0, 10) looking down -Z over an 800 x 600 view.
RenderView frontLens() {
    EditorCamera camera;
    camera.setPivot(glm::vec3(0.0f));
    camera.setDistance(10.0f);
    camera.setViewAngles(0.0f, 0.0f);
    return camera.renderView();
}

glm::vec2 toView(const RenderView& lens, glm::vec2 viewSize, glm::vec3 point) {
    const glm::vec4 clip =
        lensProjectionMatrix(lens, viewSize.x / viewSize.y) * lensViewMatrix(lens) * glm::vec4(point, 1.0f);
    return (glm::vec2(clip) / clip.w * 0.5f + 0.5f) * viewSize;
}

void testIconPicking() {
    Fixture fixture;
    Scene& scene = fixture.scene;
    const SceneEntityId wall = fixture.cube({0.0f, 0.0f, 0.0f});
    const SceneEntityId light = fixture.entity("Light");
    scene.registry().emplace<PointLightComponent>(fixture.find(light));
    scene.registry().get<TransformComponent>(fixture.find(light)).local = at({0.0f, 0.0f, 2.0f});
    const SceneEntityId speaker = fixture.entity("Speaker");
    scene.registry().emplace<AudioEmitterComponent>(fixture.find(speaker));
    scene.registry().get<TransformComponent>(fixture.find(speaker)).local = at({3.0f, 0.0f, 0.0f});
    const SceneEntityId hiddenLight = fixture.entity("Hidden Light");
    scene.registry().emplace<PointLightComponent>(fixture.find(hiddenLight));
    scene.registry().get<TransformComponent>(fixture.find(hiddenLight)).local = at({-3.0f, 0.0f, 0.0f});
    scene.registry().emplace<VisibilityComponent>(fixture.find(hiddenLight)).hidden = true;
    for (const SceneEntityId id : {light, speaker, hiddenLight}) {
        scene.markTransformDirty(fixture.find(id));
    }
    scene.updateWorldTransforms();

    const RenderView lens = frontLens();
    const glm::vec2 viewSize(800.0f, 600.0f);
    const SceneGizmoView view{
        .scene = scene, .assets = fixture.assets, .selection = {}, .aspectRatio = 800.0f / 600.0f};
    const std::vector<SceneGizmoAnchor> icons = pickableIcons(view, {});
    const auto listed = [&icons](SceneEntityId id) {
        return std::ranges::find(icons, id, &SceneGizmoAnchor::entity) != icons.end();
    };
    expect(listed(light) && listed(speaker) && !listed(hiddenLight) && !listed(wall),
           "lights and emitters are pickable icons; hidden objects and meshes are not");
    expect(!listed(light) || pickableIcons(view, {.lights = OverlayScope::Off}).size() == icons.size() - 1,
           "an icon whose overlay is off cannot be clicked");

    // A click on the light in front of the wall picks the light; beside it,
    // the speaker alone; the wall still picks away from both.
    ViewportPickQuery click{.kind = ViewportPickKind::Click,
                            .point = toView(lens, viewSize, {0.0f, 0.0f, 2.0f}) + glm::vec2(3.0f, 0.0f),
                            .lens = lens,
                            .viewSize = viewSize,
                            .icons = icons};
    ViewportPickOutcome outcome = resolveCpuPick(click, scene, fixture.assets);
    expect(outcome.entities.size() == 1 && outcome.entities.front() == wall,
           "without icons the wall is under the click");
    mergeIconPicks(click, scene, fixture.assets, outcome);
    expect(outcome.entities.size() == 1 && outcome.entities.front() == light,
           "a click on a light's icon picks the light over the mesh behind it");

    ViewportPickQuery cycle = click;
    cycle.kind = ViewportPickKind::Cycle;
    ViewportPickOutcome candidates = resolveCpuPick(cycle, scene, fixture.assets);
    mergeIconPicks(cycle, scene, fixture.assets, candidates);
    expect(candidates.entities.size() == 2 && candidates.entities[0] == light && candidates.entities[1] == wall,
           "Alt+click cycling offers the icon before the meshes under it");

    click.point = toView(lens, viewSize, {3.0f, 0.0f, 0.0f});
    outcome = resolveCpuPick(click, scene, fixture.assets);
    mergeIconPicks(click, scene, fixture.assets, outcome);
    expect(outcome.entities.size() == 1 && outcome.entities.front() == speaker, "an emitter is clicked by its origin");

    click.point = toView(lens, viewSize, {0.0f, 0.4f, 0.5f});
    outcome = resolveCpuPick(click, scene, fixture.assets);
    mergeIconPicks(click, scene, fixture.assets, outcome);
    expect(outcome.entities.size() == 1 && outcome.entities.front() == wall, "away from icons the mesh pick stands");

    // A light inside the wall does not steal the wall's clicks, though Alt+click still reaches it.
    scene.registry().get<TransformComponent>(fixture.find(light)).local = at({0.0f, 0.0f, 0.0f});
    scene.markTransformDirty(fixture.find(light));
    scene.updateWorldTransforms();
    click.icons = pickableIcons(view, {});
    click.point = toView(lens, viewSize, {0.0f, 0.0f, 0.0f});
    outcome = resolveCpuPick(click, scene, fixture.assets);
    mergeIconPicks(click, scene, fixture.assets, outcome);
    expect(outcome.entities.size() == 1 && outcome.entities.front() == wall, "an icon behind a surface is not clicked");
    cycle = click;
    cycle.kind = ViewportPickKind::Cycle;
    candidates = resolveCpuPick(cycle, scene, fixture.assets);
    mergeIconPicks(cycle, scene, fixture.assets, candidates);
    expect(candidates.entities.size() == 2 && candidates.entities[0] == wall && candidates.entities[1] == light,
           "cycling reaches an icon hidden behind a surface after the surface");

    ViewportPickQuery box{
        .kind = ViewportPickKind::Region,
        .region = {.shape = RegionShape::Box,
                   .points = {toView(lens, viewSize, {2.0f, 1.0f, 0.0f}), toView(lens, viewSize, {4.0f, -1.0f, 0.0f})}},
        .lens = lens,
        .viewSize = viewSize,
        .icons = icons};
    ViewportPickOutcome inside = resolveCpuPick(box, scene, fixture.assets);
    mergeIconPicks(box, scene, fixture.assets, inside);
    expect(inside.entities.size() == 1 && inside.entities.front() == speaker, "box select takes the icons inside");
}
} // namespace

int main() {
    testSelectHierarchy();
    testSelectGrouped();
    testIconPicking();
    if (failures != 0) {
        std::cerr << failures << " selection tool test(s) failed\n";
        return 1;
    }
    return 0;
}
