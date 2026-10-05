// CPU tests of debug drawing: primitive tessellation (segment counts, depth
// lists, geometry), colour packing, lifetimes (one frame, timed, persistent)
// and named channels, frustum corners, label projection with behind-camera
// and off-screen culling, the RenderRuntime lists, and the Jolt adapter behind
// PhysicsWorld::drawDebug.

#include "engine/graphics/DebugDraw.hpp"
#include "engine/graphics/Rendering.hpp"
#include "engine/physics/PhysicsWorld.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>

namespace {
int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

bool near(float a, float b, float tolerance = 1e-4f) {
    return std::abs(a - b) <= tolerance;
}

bool near(const glm::vec3& a, const glm::vec3& b, float tolerance = 1e-4f) {
    return glm::length(a - b) <= tolerance;
}

// The renderer's camera convention: reversed Z (swapped planes) with Y flipped
// for Vulkan's downward clip-space Y.
glm::mat4 reversedProjection(float fov, float aspect, float nearPlane, float farPlane) {
    glm::mat4 projection = glm::perspectiveRH_ZO(fov, aspect, farPlane, nearPlane);
    projection[1][1] *= -1.0f;
    return projection;
}

size_t lines(const DebugDraw& draw, DebugDepth depth) {
    return draw.lineVertices(depth).size() / 2;
}

void testSegmentCounts() {
    constexpr uint32_t segments = DebugDraw::defaultSegments;
    const auto count = [](auto record) {
        DebugDraw draw;
        record(draw);
        return draw.lineCount();
    };
    expect(count([](DebugDraw& d) { d.line({0, 0, 0}, {1, 0, 0}); }) == 1, "a line is one segment");
    expect(count([](DebugDraw& d) { d.arrow({0, 0, 0}, {0, 0, 2}); }) == 5, "an arrow is a shaft and four head lines");
    expect(count([](DebugDraw& d) { d.aabb({-1, -1, -1}, {1, 2, 3}); }) == 12, "an AABB has twelve edges");
    expect(count([](DebugDraw& d) { d.box({0, 0, 0}, {1, 1, 1}, glm::quat(1, 0, 0, 0)); }) == 12,
           "an oriented box has twelve edges");
    expect(count([](DebugDraw& d) { d.box(glm::mat4(1.0f), {1, 1, 1}); }) == 12, "a transformed box has twelve edges");
    expect(count([](DebugDraw& d) { d.sphere({0, 0, 0}, 1.0f); }) == 3 * segments, "a sphere is three circles");
    expect(count([](DebugDraw& d) { d.sphere({0, 0, 0}, 1.0f, {}, 8); }) == 24, "sphere segments follow the argument");
    expect(count([](DebugDraw& d) { d.circle({0, 0, 0}, {0, 1, 0}, 1.0f); }) == segments, "a circle");
    expect(count([](DebugDraw& d) { d.frustum(reversedProjection(1.0f, 1.5f, 0.1f, 50.0f)); }) == 12,
           "a frustum has twelve edges");
    expect(count([](DebugDraw& d) { d.cone({0, 0, 0}, {0, 0, -1}, 5.0f, 0.4f); }) == segments + 4,
           "a cone is its base circle and four slant edges");
    expect(count([](DebugDraw& d) { d.capsule({0, 0, 0}, {0, 2, 0}, 0.5f); }) == 4 * segments + 4,
           "a capsule is two circles, four sides and four half arcs");
    expect(count([](DebugDraw& d) { d.capsule({0, 1, 0}, {0, 1, 0}, 0.5f); }) == 3 * segments,
           "a zero-length capsule is a sphere");
    expect(count([](DebugDraw& d) { d.circle({0, 0, 0}, {0, 0, 0}, 1.0f); }) == 0,
           "a circle without a normal records nothing");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    expect(count([nan](DebugDraw& d) { d.line({nan, 0, 0}, {1, 0, 0}); }) == 0, "non-finite segments are dropped");
}

void testDepthListsAndColors() {
    DebugDraw draw;
    expect(draw.empty(), "a new list is empty");
    draw.line({0, 0, 0}, {1, 0, 0}, {.color = {1.0f, 0.0f, 0.0f, 1.0f}});
    draw.aabb({0, 0, 0}, {1, 1, 1}, {.color = {0.0f, 1.0f, 0.0f, 0.5f}, .depth = DebugDepth::AlwaysOnTop});
    expect(lines(draw, DebugDepth::Tested) == 1 && lines(draw, DebugDepth::AlwaysOnTop) == 12,
           "primitives land in their depth mode's list");
    expect(draw.lineVertices(DebugDepth::Tested)[0].color == 0xff0000ffu, "red packs as RGBA8 with red lowest");
    expect(packDebugColor({0.0f, 1.0f, 0.0f, 0.5f}) == 0x8000ff00u, "half alpha rounds to 128");
    expect(packDebugColor({2.0f, -1.0f, 0.0f, 1.0f}) == 0xff0000ffu, "colour channels clamp to [0, 1]");
    draw.label({0, 0, 0}, "origin");
    expect(draw.labels().size() == 1 && draw.labels()[0].text == "origin", "labels are recorded");
    draw.clear();
    expect(draw.empty() && draw.lineCount() == 0 && draw.labels().empty(), "clear discards everything");
}

void testGeometry() {
    DebugDraw draw;
    draw.aabb({-1.0f, -2.0f, -3.0f}, {4.0f, 5.0f, 6.0f});
    glm::vec3 minimum(1e9f);
    glm::vec3 maximum(-1e9f);
    for (const DebugLineVertex& vertex : draw.lineVertices(DebugDepth::Tested)) {
        minimum = glm::min(minimum, vertex.position);
        maximum = glm::max(maximum, vertex.position);
    }
    expect(near(minimum, {-1.0f, -2.0f, -3.0f}) && near(maximum, {4.0f, 5.0f, 6.0f}), "AABB spans its corners");

    draw.clear();
    // A box rotated a quarter turn about Y swaps its X and Z extents.
    draw.box({10.0f, 0.0f, 0.0f}, {2.0f, 1.0f, 0.5f}, glm::angleAxis(glm::half_pi<float>(), glm::vec3(0, 1, 0)));
    minimum = glm::vec3(1e9f);
    maximum = glm::vec3(-1e9f);
    for (const DebugLineVertex& vertex : draw.lineVertices(DebugDepth::Tested)) {
        minimum = glm::min(minimum, vertex.position);
        maximum = glm::max(maximum, vertex.position);
    }
    expect(near(minimum, {9.5f, -1.0f, -2.0f}, 1e-3f) && near(maximum, {10.5f, 1.0f, 2.0f}, 1e-3f),
           "an oriented box is rotated about its centre");

    draw.clear();
    const glm::vec3 center{1.0f, 2.0f, 3.0f};
    const glm::vec3 normal = glm::normalize(glm::vec3(1.0f, 1.0f, 0.0f));
    draw.circle(center, normal, 2.0f);
    bool onCircle = true;
    for (const DebugLineVertex& vertex : draw.lineVertices(DebugDepth::Tested)) {
        const glm::vec3 offset = vertex.position - center;
        onCircle = onCircle && near(glm::length(offset), 2.0f, 1e-3f) && near(glm::dot(offset, normal), 0.0f, 1e-3f);
    }
    expect(onCircle, "circle vertices lie on the circle in the plane normal to its axis");

    draw.clear();
    draw.cone({0, 0, 0}, {0, 0, -2}, 4.0f, glm::radians(45.0f));
    bool coneEnds = true;
    for (const DebugLineVertex& vertex : draw.lineVertices(DebugDepth::Tested)) {
        const bool apex = near(vertex.position, glm::vec3(0.0f));
        const bool base =
            near(vertex.position.z, -4.0f, 1e-3f) && near(glm::length(glm::vec2(vertex.position)), 4.0f, 1e-3f);
        coneEnds = coneEnds && (apex || base);
    }
    expect(coneEnds, "cone vertices are its apex or on the base circle (direction normalized)");

    draw.clear();
    draw.capsule({0, 0, 0}, {0, 3, 0}, 0.5f);
    bool onCapsule = true;
    for (const DebugLineVertex& vertex : draw.lineVertices(DebugDepth::Tested)) {
        const float along = std::clamp(vertex.position.y, 0.0f, 3.0f);
        onCapsule = onCapsule && near(glm::distance(vertex.position, glm::vec3(0, along, 0)), 0.5f, 1e-3f);
        onCapsule = onCapsule && vertex.position.y >= -0.5f - 1e-3f && vertex.position.y <= 3.5f + 1e-3f;
    }
    expect(onCapsule, "capsule vertices lie on its surface");

    draw.clear();
    draw.arrow({0, 0, 0}, {0, 0, 10});
    bool headNearTip = true;
    const std::span<const DebugLineVertex> arrow = draw.lineVertices(DebugDepth::Tested);
    for (size_t index = 2; index < arrow.size(); index += 2) {
        headNearTip = headNearTip && near(arrow[index].position, {0, 0, 10}) && arrow[index + 1].position.z < 10.0f &&
                      arrow[index + 1].position.z >= 8.0f - 1e-3f;
    }
    expect(headNearTip, "arrow head lines start at the tip and span a fifth of the length");
}

void testFrustumCorners() {
    const glm::mat4 view = glm::lookAtRH(glm::vec3(0, 0, 0), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
    const glm::mat4 projection = reversedProjection(glm::radians(90.0f), 1.0f, 0.5f, 20.0f);
    const std::array<glm::vec3, 8> corners = frustumCorners(glm::inverse(projection * view));
    // Reversed Z: clip depth 0 is the far plane and 1 the near plane.
    bool farFace = true;
    bool nearFace = true;
    for (uint32_t index = 0; index < 4; ++index) {
        farFace = farFace && near(corners[index].z, -20.0f, 1e-2f) && near(std::abs(corners[index].x), 20.0f, 1e-2f) &&
                  near(std::abs(corners[index].y), 20.0f, 1e-2f);
        nearFace =
            nearFace && near(corners[index + 4].z, -0.5f, 1e-4f) && near(std::abs(corners[index + 4].x), 0.5f, 1e-4f);
    }
    expect(farFace && nearFace, "frustum corners sit on the far and near planes of a 90 degree view");
    const std::array<glm::vec3, 8> box =
        frustumCorners(glm::inverse(glm::orthoRH_ZO(-2.0f, 2.0f, -1.0f, 1.0f, 0.0f, 5.0f)));
    expect(near(box[0], {-2.0f, -1.0f, 0.0f}) && near(box[2], {2.0f, 1.0f, 0.0f}) && near(box[6], {2.0f, 1.0f, -5.0f}),
           "orthographic frustum corners are the box corners");
}

void testProjection() {
    const glm::mat4 view = glm::lookAtRH(glm::vec3(0, 0, 5), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
    const glm::mat4 viewProjection = reversedProjection(glm::radians(60.0f), 2.0f, 0.1f, 100.0f) * view;
    const glm::vec2 viewport{800.0f, 400.0f};
    const std::optional<ProjectedDebugPoint> center = projectDebugPoint(viewProjection, viewport, {0, 0, 0});
    expect(center && near(center->position.x, 400.0f, 1e-2f) && near(center->position.y, 200.0f, 1e-2f) &&
               near(center->distance, 5.0f, 1e-3f),
           "a point ahead projects to the viewport centre at its forward distance");
    const std::optional<ProjectedDebugPoint> upRight = projectDebugPoint(viewProjection, viewport, {1, 1, 0});
    expect(upRight && upRight->position.x > 400.0f && upRight->position.y < 200.0f,
           "up and right map toward the top-right (top-left pixel origin)");
    expect(!projectDebugPoint(viewProjection, viewport, {0, 0, 10}), "a point behind the camera is culled");
    expect(!projectDebugPoint(viewProjection, viewport, {0, 0, 5}), "a point at the camera is culled");
    expect(!projectDebugPoint(viewProjection, viewport, {50, 0, 0}), "a point outside the view's sides is culled");

    DebugDraw draw;
    draw.label({0, 0, 0}, "center", {1, 0, 0, 1});
    draw.label({0, 0, 20}, "behind");
    draw.label({0, 1, 0}, "above");
    std::vector<ProjectedDebugLabel> projected{{.text = "kept"}};
    projectDebugLabels(draw.labels(), viewProjection, viewport, projected);
    expect(projected.size() == 3 && projected[0].text == "kept" && projected[1].text == "center" &&
               projected[2].text == "above",
           "projection appends visible labels in recording order");
    expect(projected.size() == 3 && projected[1].color == glm::vec4(1, 0, 0, 1) &&
               projected[2].position.y < projected[1].position.y,
           "projected labels keep their colour and screen placement");
}

void testRenderRuntimeLists() {
    RenderRuntime rendering;
    rendering.debugDraw().line({0, 0, 0}, {1, 1, 1});
    expect(rendering.debugDraw().lineCount() == 1, "the runtime owns an all-views list");
    DebugDraw* defaultList = rendering.viewDebugDraw(rendering.defaultView());
    expect(defaultList != nullptr && defaultList->empty(), "the default view has its own list");
    const RenderViewId created = rendering.createView();
    DebugDraw* createdList = rendering.viewDebugDraw(created);
    expect(createdList != nullptr && createdList != defaultList, "a created view has a separate list");
    createdList->sphere({0, 0, 0}, 1.0f);
    expect(rendering.destroyView(created), "the created view is destroyed");
    expect(rendering.viewDebugDraw(created) == nullptr, "a stale handle has no list");
    expect(rendering.viewDebugLabels(created).empty(), "a stale handle has no labels");
    const RenderViewId reused = rendering.createView();
    expect(rendering.viewDebugDraw(reused) != nullptr && rendering.viewDebugDraw(reused)->empty(),
           "a reused slot starts with an empty list");
    expect(rendering.viewDebugLabels(rendering.defaultView()).empty(), "no labels before a frame renders");
    DebugDraw* reusedList = rendering.viewDebugDraw(reused);
    for (uint32_t index = 2; index < RenderRuntime::maximumViews; ++index) {
        static_cast<void>(rendering.createView());
    }
    expect(rendering.viewDebugDraw(reused) == reusedList &&
               rendering.viewDebugDraw(rendering.defaultView()) == defaultList,
           "creating views keeps existing lists in place");
}

void testPhysicsAdapter() {
    PhysicsWorld uninitialized;
    DebugDraw draw;
    expect(!uninitialized.drawDebug(draw), "drawing an uninitialized world fails");

    PhysicsWorld physics;
    expect(static_cast<bool>(physics.init()), "physics initializes");
    Scene scene;
    AssetManager assets;
    const auto addBody = [&](const glm::vec3& position, ColliderShape shape, PhysicsMotionType motion) {
        const Entity entity = scene.createEntity("Body");
        scene.registry().get<TransformComponent>(entity).local = glm::translate(glm::mat4(1.0f), position);
        scene.registry().emplace<ColliderComponent>(entity, ColliderComponent{shape});
        RigidBodyComponent body;
        body.motionType = motion;
        body.layer = motion == PhysicsMotionType::Static ? PhysicsLayer::Static : PhysicsLayer::Dynamic;
        body.gravityFactor = 0.0f;
        scene.registry().emplace<RigidBodyComponent>(entity, body);
    };
    addBody({0, 0, 0}, BoxCollider{glm::vec3(1.0f, 0.5f, 2.0f)}, PhysicsMotionType::Static);
    addBody({5, 0, 0}, SphereCollider{0.75f}, PhysicsMotionType::Dynamic);
    addBody({-5, 0, 0}, CapsuleCollider{1.0f, 0.5f}, PhysicsMotionType::Dynamic);
    expect(static_cast<bool>(physics.step(scene, assets, physics.config().fixedTimeStep)), "bodies are created");

    expect(static_cast<bool>(physics.drawDebug(draw)), "physics debug drawing succeeds");
    expect(lines(draw, DebugDepth::Tested) > 12 && lines(draw, DebugDepth::AlwaysOnTop) == 0,
           "collider shapes produce depth-tested lines");
    bool withinBodies = true;
    bool reachesSphere = false;
    bool reachesCapsule = false;
    for (const DebugLineVertex& vertex : draw.lineVertices(DebugDepth::Tested)) {
        const glm::vec3& p = vertex.position;
        withinBodies = withinBodies && p.x >= -5.6f && p.x <= 5.8f && std::abs(p.y) <= 1.6f && std::abs(p.z) <= 2.1f;
        reachesSphere = reachesSphere || p.x > 5.5f;
        reachesCapsule = reachesCapsule || p.x < -5.3f;
    }
    expect(withinBodies, "shape lines stay on the bodies");
    expect(reachesSphere && reachesCapsule, "every body's shape is drawn");

    DebugDraw boundsOnly;
    expect(static_cast<bool>(physics.drawDebug(
               boundsOnly, {.shapes = false, .boundingBoxes = true, .depth = DebugDepth::AlwaysOnTop})),
           "bounding boxes draw");
    expect(lines(boundsOnly, DebugDepth::AlwaysOnTop) == 36 && lines(boundsOnly, DebugDepth::Tested) == 0,
           "three bodies give three on-top boxes");

    // Into one channel, which hides the bodies on its own.
    DebugDraw channelled;
    expect(static_cast<bool>(physics.drawDebug(channelled.channel("physics"))) &&
               lines(channelled, DebugDepth::Tested) == lines(draw, DebugDepth::Tested),
           "physics draws into a channel");
    expect(channelled.channels().size() == 1 && channelled.channels().front().name == "physics",
           "the channel lists the bodies");
    channelled.endFrame();
    channelled.setChannelVisible("physics", false);
    expect(static_cast<bool>(physics.drawDebug(channelled.channel("physics"))) && channelled.empty(),
           "a hidden channel drops the bodies");

    // Jolt allows one debug renderer per process; every world shares it.
    PhysicsWorld second;
    expect(static_cast<bool>(second.init()), "a second world initializes");
    DebugDraw secondDraw;
    expect(static_cast<bool>(second.drawDebug(secondDraw)) && secondDraw.empty(),
           "a second world draws (nothing) through the shared renderer");
    // The world that created the renderer shuts down first; the shared Jolt
    // runtime keeps it (and the factory) alive for the remaining world.
    physics.shutdown();
    expect(static_cast<bool>(second.drawDebug(secondDraw)), "a world draws after the first world shut down");
    second.shutdown();
}

// One-frame primitives end with the frame, timed ones after their duration of
// endFrame time (and never before they were drawn once), persistent ones when
// cleared; channels hide and clear as a group.
void testLifetimesAndChannels() {
    DebugDraw draw;
    draw.line({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
    draw.forSeconds(1.0f).line({0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    draw.forSeconds(0.0f).label({0.0f, 0.0f, 0.0f}, "instant");
    draw.persistent().line({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
    expect(draw.lineCount() == 3 && draw.labels().size() == 1, "every lifetime draws in the frame it is recorded");
    draw.endFrame(0.4f);
    expect(draw.lineCount() == 2 && draw.labels().empty(),
           "the frame ends one-frame primitives and a zero duration, which still drew once");
    draw.endFrame(0.4f);
    expect(draw.lineCount() == 2, "a timed primitive stays within its duration");
    draw.endFrame(0.4f);
    expect(draw.lineCount() == 1 && !draw.empty(), "a timed primitive ends when its time is up");
    draw.endFrame(100.0f);
    expect(draw.lineCount() == 1, "a persistent primitive outlives any time");
    draw.endFrame(std::numeric_limits<float>::quiet_NaN());
    expect(draw.lineCount() == 1, "a non-finite frame time ages nothing");

    // Channels: recorded through recorders, listed by name, toggled and
    // cleared as a group.
    const DebugDrawRecorder rooms = draw.channel("gen.rooms").persistent();
    rooms.aabb({0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f});
    rooms.label({0.5f, 0.5f, 0.5f}, "room 0");
    draw.channel("gen.paths").forSeconds(5.0f).arrow({0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f});
    draw.channel("gen.spawns").sphere({0.0f, 0.0f, 0.0f}, 1.0f, {}, 8);
    expect(draw.lineCount() == 1 + 12 + 5 + 24 && draw.labels().size() == 1, "every channel draws");
    const std::vector<DebugDrawChannelInfo> channels = draw.channels();
    expect(channels.size() == 3 && channels[0].name == "gen.paths" && channels[1].name == "gen.rooms" &&
               channels[2].name == "gen.spawns" &&
               std::ranges::all_of(channels, [](const auto& channel) { return channel.visible && channel.active; }),
           "named channels are listed sorted, visible, and active");

    draw.setChannelVisible("gen.rooms", false);
    expect(!draw.channelVisible("gen.rooms") && draw.channelVisible("never.named") && draw.lineCount() == 1 + 5 + 24 &&
               draw.labels().empty(),
           "a hidden channel's primitives and labels are not drawn");
    draw.channel("gen.rooms").line({0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f});
    draw.setChannelVisible("gen.rooms", true);
    expect(draw.lineCount() == 1 + 12 + 5 + 24 && draw.labels().size() == 1,
           "showing a channel again shows what it kept; one-frame shapes recorded while hidden were dropped");

    draw.endFrame(0.016f);
    const std::vector<DebugDrawChannelInfo> afterFrame = draw.channels();
    expect(afterFrame.size() == 3 && afterFrame[2].name == "gen.spawns" && afterFrame[2].active,
           "a channel that drew last frame stays active between frames");
    draw.endFrame(0.016f);
    expect(!draw.channels()[2].active, "a channel that stopped drawing becomes inactive but stays listed");

    draw.clearChannel("gen.rooms");
    draw.clearChannel("gen.paths");
    expect(draw.lineCount() == 1 && draw.labels().empty() && !draw.channels()[0].active,
           "clearing channels discards their timed and persistent primitives");
    draw.clearChannel("never.named");
    expect(draw.channels().size() == 3, "clearing an unknown channel creates nothing");

    draw.setChannelVisible("gen.rooms", false);
    draw.clear();
    expect(draw.empty() && draw.lineCount() == 0 && !draw.channelVisible("gen.rooms"),
           "clear discards every lifetime and keeps the channels' visibility");
}
} // namespace

int main() {
    testSegmentCounts();
    testDepthListsAndColors();
    testLifetimesAndChannels();
    testGeometry();
    testFrustumCorners();
    testProjection();
    testRenderRuntimeLists();
    testPhysicsAdapter();
    if (failures == 0) {
        std::cout << "Debug draw tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
