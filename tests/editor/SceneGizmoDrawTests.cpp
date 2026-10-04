// CPU tests of the viewport's scene gizmos (editor/viewport/SceneGizmoDraw):
// the geometry each component kind records, the overlay scopes, hidden
// entities, local-view isolation, selection colours and bounds (collection
// instances and the cached mesh box), labels, and the Play-mode policy.

#include "editor/panels/ViewportPanel.hpp"
#include "editor/viewport/SceneGizmoDraw.hpp"
#include "engine/resources/ResourceLocator.hpp"
#include "engine/scene/Collections.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

bool near(float actual, float expected, float tolerance = 1e-3f) {
    return std::abs(actual - expected) <= tolerance;
}

// Line counts of the DebugDraw shapes at the gizmo tessellation.
constexpr size_t sphereLines(uint32_t segments) {
    return 3u * segments;
}

constexpr size_t coneLines = sceneGizmoSegments::range + 4u;
constexpr size_t arrowLines = 5;
constexpr size_t boxLines = 12;
constexpr size_t markerLines = sphereLines(sceneGizmoSegments::marker);
constexpr size_t capsuleLines = 2u * sceneGizmoSegments::range + 4u + 4u * (sceneGizmoSegments::range / 2u);
constexpr size_t cameraLines = 12 + 3;

SceneGizmoOverlays allOff() {
    return {.lights = OverlayScope::Off,
            .cameras = OverlayScope::Off,
            .colliders = OverlayScope::Off,
            .reflectionProbes = OverlayScope::Off,
            .decals = OverlayScope::Off,
            .irradianceVolumes = OverlayScope::Off,
            .names = OverlayScope::Off,
            .selectionBounds = false};
}

struct GizmoScene {
    Scene scene;
    AssetManager assets;
    std::vector<SceneEntityId> selection;
    std::optional<SceneEntityId> viewCamera;
    std::optional<std::vector<RenderObjectId>> isolatedObjects;

    Entity add(const char* name, const glm::mat4& local = glm::mat4(1.0f)) {
        const Entity entity = scene.createEntity(name);
        scene.registry().get<TransformComponent>(entity).local = local;
        scene.markTransformDirty(entity);
        return entity;
    }

    DebugDraw draw(const SceneGizmoOverlays& overlays) {
        scene.updateWorldTransforms();
        DebugDraw output;
        drawSceneGizmos({.scene = scene,
                         .assets = assets,
                         .selection = selection,
                         .aspectRatio = 2.0f,
                         .viewCamera = viewCamera,
                         .isolatedObjects = isolatedObjects ? &*isolatedObjects : nullptr},
                        overlays, output);
        return output;
    }
};

std::optional<MeshHandle> loadCube(AssetManager& assets) {
    const AssetLoadResult<MeshHandle> cube = assets.loadMesh(assetRoot() / "primitives" / "cube.gltf", 0);
    expect(static_cast<bool>(cube), "the cube mesh should load: " + cube.error);
    return cube ? std::optional<MeshHandle>(cube.handle) : std::nullopt;
}

// Every line end, both depth modes.
std::vector<DebugLineVertex> vertices(const DebugDraw& draw) {
    std::vector<DebugLineVertex> result;
    for (const DebugDepth depth : {DebugDepth::Tested, DebugDepth::AlwaysOnTop}) {
        const std::span<const DebugLineVertex> lines = draw.lineVertices(depth);
        result.insert(result.end(), lines.begin(), lines.end());
    }
    return result;
}

glm::vec3 lowest(const DebugDraw& draw) {
    glm::vec3 result(std::numeric_limits<float>::max());
    for (const DebugLineVertex& vertex : vertices(draw)) {
        result = glm::min(result, vertex.position);
    }
    return result;
}

glm::vec3 highest(const DebugDraw& draw) {
    glm::vec3 result(std::numeric_limits<float>::lowest());
    for (const DebugLineVertex& vertex : vertices(draw)) {
        result = glm::max(result, vertex.position);
    }
    return result;
}

bool allColored(std::span<const DebugLineVertex> lines, const glm::vec4& color) {
    return !lines.empty() && std::ranges::all_of(lines, [&](const DebugLineVertex& vertex) {
        return vertex.color == packDebugColor(color);
    });
}

void testLights() {
    const SceneGizmoColors colors = sceneGizmoColors();
    SceneGizmoOverlays lights = allOff();
    lights.lights = OverlayScope::All;
    {
        GizmoScene fixture;
        const Entity point = fixture.add("Point", glm::translate(glm::mat4(1.0f), {1.0f, 2.0f, 3.0f}));
        fixture.scene.registry().emplace<PointLightComponent>(point).range = 4.0f;
        const DebugDraw output = fixture.draw(lights);
        expect(output.lineCount() == sphereLines(sceneGizmoSegments::range) + markerLines,
               "a point light draws its range sphere and a marker");
        expect(near(highest(output).x, 5.0f) && near(lowest(output).y, -2.0f),
               "the range sphere has the light's range around its position");
        expect(allColored(output.lineVertices(DebugDepth::Tested), colors.light) &&
                   output.lineVertices(DebugDepth::AlwaysOnTop).empty(),
               "an unselected light is depth tested in the light colour");
        expect(fixture.draw(allOff()).lineCount() == 0, "lights off draws nothing");
    }
    {
        GizmoScene fixture;
        // A quarter turn about X takes the default -Y spot direction to -Z.
        const Entity spot =
            fixture.add("Spot", glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
        SpotLightComponent& light = fixture.scene.registry().emplace<SpotLightComponent>(spot);
        light.range = 5.0f;
        light.outerConeRadians = glm::radians(30.0f);
        light.innerConeRadians = glm::radians(15.0f);
        const DebugDraw output = fixture.draw(lights);
        expect(output.lineCount() == 2u * coneLines + markerLines, "a spot light draws outer and inner cones");
        expect(near(lowest(output).z, -5.0f) && near(highest(output).y, 5.0f * std::tan(glm::radians(30.0f))),
               "the spot cones follow the transformed direction for the range at the outer angle");
    }
    {
        GizmoScene fixture;
        const Entity sun = fixture.add("Sun", glm::translate(glm::mat4(1.0f), {0.0f, 10.0f, 0.0f}));
        // Toward the light: straight up, so the light travels down.
        fixture.scene.registry().emplace<DirectionalLightComponent>(sun).direction = {0.0f, 2.0f, 0.0f};
        const DebugDraw output = fixture.draw(lights);
        expect(output.lineCount() == sceneGizmoSegments::marker + 3u * arrowLines,
               "a directional light draws a circle and three arrows");
        expect(near(lowest(output).y, 10.0f - sceneGizmoSizes::directionalArrowLength),
               "directional arrows point along the light's travel");
    }
    {
        GizmoScene fixture;
        fixture.scene.registry().emplace<EnvironmentLightComponent>(fixture.add("Sky"));
        expect(fixture.draw(lights).lineCount() == markerLines, "an environment light draws a marker");
    }
}

void testCameras() {
    const SceneGizmoColors colors = sceneGizmoColors();
    SceneGizmoOverlays cameras = allOff();
    cameras.cameras = OverlayScope::All;
    GizmoScene fixture;
    const Entity primary = fixture.add("Primary");
    CameraComponent& primaryCamera = fixture.scene.registry().emplace<CameraComponent>(primary);
    primaryCamera.nearPlane = 0.1f;
    primaryCamera.farPlane = 20.0f;
    const Entity second = fixture.add("Second", glm::translate(glm::mat4(1.0f), {10.0f, 0.0f, 0.0f}));
    fixture.scene.registry().emplace<CameraComponent>(second).primary = false;

    DebugDraw output = fixture.draw(cameras);
    expect(output.lineCount() == 2u * cameraLines, "each camera draws a frustum and an up triangle");
    const std::span<const DebugLineVertex> tested = output.lineVertices(DebugDepth::Tested);
    const auto countColored = [&](const glm::vec4& color) {
        return std::ranges::count_if(
            tested, [&](const DebugLineVertex& vertex) { return vertex.color == packDebugColor(color); });
    };
    expect(countColored(colors.activeCamera) == static_cast<long long>(cameraLines * 2u) &&
               countColored(colors.camera) == static_cast<long long>(cameraLines * 2u),
           "the primary camera is highlighted as the active scene camera");
    expect(lowest(output).z > -(0.1f + sceneGizmoSizes::cameraDisplayDepth) - 1e-3f,
           "an unselected camera's frustum stops at the display depth");

    fixture.selection = {fixture.scene.id(primary)};
    output = fixture.draw(cameras);
    expect(near(lowest(output).z, -20.0f), "a selected camera shows its frustum out to the far plane");
    expect(allColored(output.lineVertices(DebugDepth::AlwaysOnTop), colors.active),
           "the active camera draws over the scene in the active colour");

    fixture.viewCamera = fixture.scene.id(primary);
    expect(fixture.draw(cameras).lineCount() == cameraLines, "the camera the view looks through draws no frustum");
}

void testCollidersAndVolumes() {
    GizmoScene fixture;
    entt::registry& registry = fixture.scene.registry();
    const Entity box = fixture.add("Box", glm::scale(glm::mat4(1.0f), {2.0f, 1.0f, 1.0f}));
    registry.emplace<ColliderComponent>(box, ColliderComponent{.shape = BoxCollider{.halfExtents = glm::vec3(0.5f)}});
    const SceneGizmoOverlays defaults;
    expect(defaults.colliders == OverlayScope::Selected, "colliders default to the selection");
    expect(fixture.draw(defaults).lineCount() == 0, "an unselected collider draws nothing by default");
    fixture.selection = {fixture.scene.id(box)};
    SceneGizmoOverlays colliders = allOff();
    colliders.colliders = OverlayScope::Selected;
    DebugDraw output = fixture.draw(colliders);
    expect(output.lineCount() == boxLines && near(highest(output).x, 1.0f),
           "a selected box collider is drawn with the world scale baked in");
    fixture.selection.clear();

    colliders.colliders = OverlayScope::All;
    const Entity sphere = fixture.add("Sphere", glm::translate(glm::mat4(1.0f), {0.0f, 10.0f, 0.0f}));
    registry.emplace<ColliderComponent>(sphere, ColliderComponent{.shape = SphereCollider{.radius = 1.0f}});
    const Entity capsule = fixture.add("Capsule", glm::translate(glm::mat4(1.0f), {0.0f, -10.0f, 0.0f}));
    registry.emplace<ColliderComponent>(
        capsule, ColliderComponent{.shape = CapsuleCollider{.halfHeight = 1.0f, .radius = 0.5f}});
    output = fixture.draw(colliders);
    expect(output.lineCount() == boxLines + sphereLines(sceneGizmoSegments::range) + capsuleLines,
           "box, sphere, and capsule colliders draw their shapes");
    expect(near(highest(output).y, 11.0f) && near(lowest(output).y, -11.5f),
           "spheres and capsules (along local Y) have their authored sizes");
    if (const std::optional<MeshHandle> cube = loadCube(fixture.assets)) {
        const Entity mesh = fixture.add("Mesh collider");
        registry.emplace<ColliderComponent>(mesh, ColliderComponent{.shape = TriangleMeshCollider{.mesh = *cube}});
        const MeshAsset* cubeMesh = fixture.assets.mesh(*cube);
        const size_t triangles = cubeMesh != nullptr ? cubeMesh->indices.size() / 3u : 0u;
        expect(fixture.draw(colliders).lineCount() == output.lineCount() + triangles * 3u,
               "a triangle mesh collider draws every triangle edge");
    }
}

void testVolumes() {
    const glm::mat4 scaled = glm::scale(glm::translate(glm::mat4(1.0f), {1.0f, 0.0f, 0.0f}), glm::vec3(3.0f));
    GizmoScene probeScene;
    probeScene.scene.registry().emplace<ReflectionProbeComponent>(probeScene.add("Probe", scaled)).halfExtents =
        glm::vec3(1.0f);
    SceneGizmoOverlays probes = allOff();
    probes.reflectionProbes = OverlayScope::All;
    DebugDraw output = probeScene.draw(probes);
    expect(output.lineCount() == boxLines && near(highest(output).x, 2.0f),
           "a reflection probe draws its axis-aligned box, ignoring scale");

    GizmoScene volumeScene;
    const glm::mat4 turned = glm::rotate(scaled, glm::radians(45.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    volumeScene.scene.registry().emplace<IrradianceVolumeComponent>(volumeScene.add("Volume", turned)).halfExtents =
        glm::vec3(1.0f);
    SceneGizmoOverlays irradiance = allOff();
    irradiance.irradianceVolumes = OverlayScope::All;
    output = volumeScene.draw(irradiance);
    expect(output.lineCount() == boxLines && near(highest(output).x, 2.0f) && near(highest(output).z, 1.0f),
           "an irradiance volume draws its axis-aligned box, ignoring rotation and scale");

    GizmoScene decalScene;
    decalScene.scene.registry().emplace<DecalComponent>(decalScene.add("Decal", scaled)).halfExtents = {0.5f, 0.25f,
                                                                                                        0.5f};
    SceneGizmoOverlays decals = allOff();
    decals.decals = OverlayScope::All;
    output = decalScene.draw(decals);
    expect(output.lineCount() == boxLines + arrowLines && near(highest(output).x, 2.5f) &&
               near(lowest(output).y, -0.75f),
           "a decal draws its scaled oriented box and a projection arrow along -Y");
}

void testSelectionBoundsAndHidden() {
    const SceneGizmoColors colors = sceneGizmoColors();
    GizmoScene fixture;
    const std::optional<MeshHandle> loaded = loadCube(fixture.assets);
    const MeshAsset* mesh = loaded ? fixture.assets.mesh(*loaded) : nullptr;
    if (mesh == nullptr) {
        return;
    }
    glm::vec3 meshUpper(std::numeric_limits<float>::lowest());
    for (const Vertex& vertex : mesh->vertices) {
        meshUpper = glm::max(meshUpper, vertex.position);
    }
    const Entity first = fixture.add("First", glm::scale(glm::mat4(1.0f), glm::vec3(2.0f)));
    fixture.scene.attachMesh(first, *loaded);
    // A model-like parent whose mesh lives on a child.
    const Entity parent = fixture.add("Parent", glm::translate(glm::mat4(1.0f), {10.0f, 0.0f, 0.0f}));
    const Entity child = fixture.add("Child", glm::translate(glm::mat4(1.0f), {0.0f, 3.0f, 0.0f}));
    fixture.scene.setParent(child, parent);
    fixture.scene.attachMesh(child, *loaded);
    fixture.selection = {fixture.scene.id(first), fixture.scene.id(parent)};

    SceneGizmoOverlays bounds = allOff();
    bounds.selectionBounds = true;
    DebugDraw output = fixture.draw(bounds);
    const std::span<const DebugLineVertex> onTop = output.lineVertices(DebugDepth::AlwaysOnTop);
    expect(output.lineCount() == 2u * boxLines && onTop.size() == 4u * boxLines,
           "each selected object draws one bounds box over the scene");
    expect(onTop.size() == 4u * boxLines && onTop.front().color == packDebugColor(colors.active) &&
               onTop.back().color == packDebugColor(colors.selected),
           "the active object's bounds use the active colour, the rest the selected colour");
    expect(near(highest(output).x, 10.0f + meshUpper.x) && near(highest(output).y, 3.0f + meshUpper.y) &&
               std::ranges::any_of(
                   onTop, [&](const DebugLineVertex& vertex) { return near(vertex.position.x, 2.0f * meshUpper.x); }),
           "bounds enclose the world-space mesh, including a child's");

    fixture.scene.registry().emplace<VisibilityComponent>(first, VisibilityComponent{.hidden = true});
    fixture.scene.registry().emplace<PointLightComponent>(first);
    SceneGizmoOverlays everything;
    everything.lights = OverlayScope::All;
    everything.names = OverlayScope::All;
    output = fixture.draw(everything);
    expect(output.lineCount() == boxLines, "a hidden entity draws no gizmos and no bounds");
    expect(std::ranges::none_of(output.labels(), [](const DebugLabel& label) { return label.text == "First"; }),
           "a hidden entity has no name label");
}

RenderObjectId objectOf(Entity entity) {
    return static_cast<RenderObjectId>(entt::to_integral(entity));
}

void testLocalViewIsolation() {
    GizmoScene fixture;
    const std::optional<MeshHandle> loaded = loadCube(fixture.assets);
    if (!loaded) {
        return;
    }
    const Entity inside = fixture.add("Inside", glm::translate(glm::mat4(1.0f), {1.0f, 0.0f, 0.0f}));
    fixture.scene.registry().emplace<PointLightComponent>(inside).range = 2.0f;
    fixture.scene.attachMesh(inside, *loaded);
    const Entity outside = fixture.add("Outside", glm::translate(glm::mat4(1.0f), {-20.0f, 0.0f, 0.0f}));
    fixture.scene.registry().emplace<PointLightComponent>(outside).range = 2.0f;
    fixture.scene.attachMesh(outside, *loaded);
    fixture.selection = {fixture.scene.id(inside), fixture.scene.id(outside)};
    fixture.isolatedObjects = std::vector<RenderObjectId>{objectOf(inside)};

    SceneGizmoOverlays everything;
    everything.lights = OverlayScope::All;
    everything.names = OverlayScope::All;
    everything.selectionBounds = true;
    const DebugDraw output = fixture.draw(everything);
    expect(output.lineCount() == sphereLines(sceneGizmoSegments::range) + markerLines + boxLines,
           "local view draws the light and bounds of isolated objects only");
    expect(lowest(output).x > -5.0f, "nothing is drawn for an object outside the local view");
    expect(output.labels().size() == 1u && output.labels().front().text == "Inside",
           "local view labels isolated objects only");

    fixture.isolatedObjects.reset();
    expect(fixture.draw(everything).lineCount() == 2u * output.lineCount(),
           "leaving local view draws every object's gizmos again");
}

void testCollectionInstanceBounds() {
    GizmoScene fixture;
    const std::optional<MeshHandle> loaded = loadCube(fixture.assets);
    const MeshAsset* mesh = loaded ? fixture.assets.mesh(*loaded) : nullptr;
    if (mesh == nullptr) {
        return;
    }
    const BoundingBox scanned = calculateBoundingBox(mesh->vertices);
    expect(!mesh->localBox.empty() && mesh->localBox.minimum == scanned.minimum &&
               mesh->localBox.maximum == scanned.maximum,
           "a loaded mesh caches the box around its vertices");
    expect(calculateBoundingBox({}).empty(), "no vertices make an empty box");

    SceneCollections collections;
    SceneCollectionId instanced;
    expect(static_cast<bool>(collections.create("Instanced", {}, instanced)), "the collection is created");
    fixture.scene.setCollections(std::move(collections));
    const Entity member = fixture.add("Member");
    fixture.scene.attachMesh(member, *loaded);
    expect(static_cast<bool>(linkToCollection(fixture.scene, member, instanced)), "the member joins the collection");
    const Entity instancer = fixture.add("Instancer", glm::translate(glm::mat4(1.0f), {10.0f, 0.0f, 0.0f}));
    fixture.scene.registry().emplace<CollectionInstanceComponent>(instancer, instanced);
    fixture.selection = {fixture.scene.id(instancer)};

    SceneGizmoOverlays bounds = allOff();
    bounds.selectionBounds = true;
    DebugDraw output = fixture.draw(bounds);
    expect(output.lineCount() == boxLines, "a selected collection instancer draws one bounds box");
    expect(near(lowest(output).x, 10.0f + mesh->localBox.minimum.x) &&
               near(highest(output).x, 10.0f + mesh->localBox.maximum.x) &&
               near(highest(output).y, mesh->localBox.maximum.y),
           "an instancer's bounds cover the members its instance draws");

    fixture.isolatedObjects = std::vector<RenderObjectId>{objectOf(member)};
    expect(fixture.draw(bounds).lineCount() == 0, "an instancer outside the local view draws no bounds");
}

void testNames() {
    const SceneGizmoColors colors = sceneGizmoColors();
    GizmoScene fixture;
    const Entity active = fixture.add("Active", glm::translate(glm::mat4(1.0f), {1.0f, 2.0f, 3.0f}));
    const Entity other = fixture.add("Other");
    fixture.add("Unselected");
    const Entity node = fixture.add("Node");
    fixture.scene.registry().emplace<ModelNodeComponent>(node);
    fixture.selection = {fixture.scene.id(active), fixture.scene.id(other)};

    SceneGizmoOverlays names = allOff();
    names.names = OverlayScope::Selected;
    DebugDraw output = fixture.draw(names);
    expect(output.labels().size() == 2u && output.lineCount() == 0, "names of the selection are labels only");
    const auto find = [&](const std::string& text) -> const DebugLabel* {
        const auto label =
            std::ranges::find_if(output.labels(), [&](const DebugLabel& entry) { return entry.text == text; });
        return label != output.labels().end() ? &*label : nullptr;
    };
    const DebugLabel* activeLabel = find("Active");
    expect(activeLabel != nullptr && activeLabel->color == colors.active &&
               glm::all(glm::equal(activeLabel->position, glm::vec3(1.0f, 2.0f, 3.0f))),
           "the active name sits at its origin in the active colour");
    expect(find("Other") != nullptr && find("Other")->color == colors.selected,
           "selected names use the selected colour");
    names.names = OverlayScope::All;
    output = fixture.draw(names);
    expect(output.labels().size() == 3u && find("Unselected") != nullptr && find("Unselected")->color == colors.label &&
               find("Node") == nullptr,
           "all names label every object except a model's nodes");
}

void testPlayModePolicy() {
    ViewportOverlays overlays;
    expect(!overlays.sceneInPlay, "scene gizmos are off in Play mode by default");
    expect(viewportDrawsSceneGizmos(EditorMode::Edit, false, overlays) &&
               viewportDrawsSceneGizmos(EditorMode::Edit, true, overlays),
           "Edit mode always draws scene gizmos");
    expect(!viewportDrawsSceneGizmos(EditorMode::Play, false, overlays) &&
               !viewportDrawsSceneGizmos(EditorMode::Paused, false, overlays),
           "Play mode draws none unless enabled");
    overlays.sceneInPlay = true;
    expect(viewportDrawsSceneGizmos(EditorMode::Play, false, overlays) &&
               !viewportDrawsSceneGizmos(EditorMode::Play, true, overlays),
           "enabled Play-mode gizmos never draw over a scene camera's view");
}
} // namespace

int main(int, char** argv) {
    initializeResourceLocator(argv[0]);
    testLights();
    testCameras();
    testCollidersAndVolumes();
    testVolumes();
    testSelectionBoundsAndHidden();
    testLocalViewIsolation();
    testCollectionInstanceBounds();
    testNames();
    testPlayModePolicy();
    if (failures != 0) {
        std::cerr << failures << " scene gizmo test(s) failed\n";
        return 1;
    }
    return 0;
}
