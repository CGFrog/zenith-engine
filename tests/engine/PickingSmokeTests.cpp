// Bounded Vulkan smoke test of GPU picking (RenderRuntime::requestPick): two
// cubes seen by a created view and the default view. Picks at a cube's
// projected centre, on empty space, and over the whole view must resolve to
// the right entities with depth that reconstructs the cube's front face; the
// id-buffer pass must record only on frames with requests; and picks whose
// view is resized or destroyed first must resolve to Cancelled. Run with
// ZENITH_SMOKE_FRAMES (the registration uses 80) and optionally
// ZENITH_VALIDATION_STRICT=1.

#include "engine/Engine.hpp"
#include "engine/graphics/GraphicsValidation.hpp"
#include "engine/resources/ResourceLocator.hpp"

#include "support/Skip.hpp"
#include "support/StandaloneChecks.hpp"
#include "support/VulkanProbe.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {
constexpr uint32_t viewSize = 256;
constexpr uint32_t viewportWidth = 320;
constexpr uint32_t viewportHeight = 240;
constexpr uint32_t pickFrame = 10;
constexpr uint32_t resultDeadline = 30;
constexpr uint32_t resizeFrame = 40;
constexpr uint32_t destroyFrame = 50;

struct SmokeResults {
    testsupport::StandaloneChecks checks;
    bool picksVerified{false};
};

glm::mat4 cameraWorld(const glm::vec3& position, const glm::vec3& target) {
    return glm::inverse(glm::lookAtRH(position, target, glm::vec3(0.0f, 1.0f, 0.0f)));
}

// The distinct entities a result's ids name.
std::vector<SceneEntityId> pickedEntities(const PickResult& result) {
    std::vector<SceneEntityId> entities;
    for (uint32_t id : result.ids) {
        if (id == 0 || id > result.objects.size()) {
            continue;
        }
        const SceneEntityId entity = result.objects[id - 1].entity;
        if (std::ranges::find(entities, entity) == entities.end()) {
            entities.push_back(entity);
        }
    }
    std::ranges::sort(entities);
    return entities;
}

class PickingScene final : public ApplicationScene {
  public:
    explicit PickingScene(SmokeResults& results)
        : m_results(results) {}

    SceneResult load(ApplicationContext& context) override {
        const AssetLoadResult<MeshHandle> cube = context.assets.loadMesh(assetRoot() / "primitives" / "cube.gltf", 0);
        if (!cube) {
            return {.success = false, .error = "Cannot load the cube mesh: " + cube.error};
        }
        Scene& scene = context.scene;
        entt::registry& registry = scene.registry();
        // The cube spans -1..1, so the centre cube's front face is at z = 1.
        const auto addCube = [&](const char* name, const glm::vec3& position) {
            const Entity entity = scene.createEntity(name);
            registry.get<TransformComponent>(entity).local = glm::translate(glm::mat4(1.0f), position);
            scene.markTransformDirty(entity);
            scene.attachMesh(entity, cube.handle);
            return scene.id(entity);
        };
        m_centre = addCube("Centre", glm::vec3(0.0f));
        m_side = addCube("Side", glm::vec3(3.0f, 0.0f, 0.0f));
        const Entity camera = scene.createEntity("Camera");
        registry.get<TransformComponent>(camera).local = cameraWorld({0.0f, 0.0f, 10.0f}, glm::vec3(0.0f));
        scene.markTransformDirty(camera);
        registry.emplace<CameraComponent>(camera);
        scene.markCameraDirty(camera);
        const Entity sun = scene.createEntity("Sun");
        registry.emplace<DirectionalLightComponent>(sun);
        scene.markLightDirty(sun);
        return {.success = true};
    }

    void update(ApplicationContext& context, const InputState&, float) override {
        ++m_frame;
        RenderRuntime& rendering = context.rendering;
        if (m_frame == 1) {
            m_view = rendering.createView();
            m_results.checks.check(static_cast<bool>(m_view), m_frame, "a view is created");
            m_results.checks.check(rendering.setViewOverride(m_view, RenderView{.position = {0.0f, 0.0f, 10.0f},
                                                                                .forward = {0.0f, 0.0f, -1.0f}}),
                                   m_frame, "the view accepts a camera");
        }
        checkPassRecording(rendering);
        if (m_frame == pickFrame) {
            requestPicks(rendering);
        }
        if (m_frame > pickFrame && !m_results.picksVerified) {
            collectPicks(rendering);
        }
        checkCancellation(rendering);
        const uint32_t size = m_frame >= resizeFrame ? viewSize + 44 : viewSize;
        rendering.requestViewSize(m_view, size, size);
        rendering.requestViewportSize(viewportWidth, viewportHeight);
    }

    void fixedUpdate(ApplicationContext&, const InputState&, float) override {}

    void unload(ApplicationContext&) override {}

  private:
    struct PendingPick {
        const char* name{""};
        PickTicket ticket;
        std::optional<PickResult> result;
    };

    void requestPicks(RenderRuntime& rendering) {
        const int32_t centre = static_cast<int32_t>(viewSize / 2);
        m_centrePick = {"centre",
                        rendering.requestPick(m_view, {.x = centre - 1, .y = centre - 1, .width = 3, .height = 3})};
        m_emptyPick = {"empty", rendering.requestPick(m_view, {.x = 0, .y = 0, .width = 4, .height = 4})};
        m_boxPick = {
            "box", rendering.requestPick(m_view, {.x = -8, .y = -8, .width = viewSize + 16, .height = viewSize + 16})};
        m_defaultPick = {"default view", rendering.requestPick(rendering.defaultView(),
                                                               {.x = static_cast<int32_t>(viewportWidth / 2) - 1,
                                                                .y = static_cast<int32_t>(viewportHeight / 2) - 1,
                                                                .width = 3,
                                                                .height = 3})};
        for (const PendingPick* pick : {&m_centrePick, &m_emptyPick, &m_boxPick, &m_defaultPick}) {
            m_results.checks.check(static_cast<bool>(pick->ticket), m_frame,
                                   std::string("the ") + pick->name + " pick is accepted");
        }
    }

    // Stats describe the frame submitted before this update.
    void checkPassRecording(const RenderRuntime& rendering) {
        const uint32_t frame = m_frame - 1;
        const uint32_t passes = rendering.stats().pickPasses;
        if (frame == pickFrame) {
            m_results.checks.check(passes == 4, frame,
                                   "the frame with four picks records four id-buffer passes, not " +
                                       std::to_string(passes));
        } else if (frame >= 2 && frame < resizeFrame) {
            m_results.checks.check(passes == 0, frame, "a frame without picks records no id-buffer pass");
        }
    }

    void collectPicks(RenderRuntime& rendering) {
        bool allReady = true;
        for (PendingPick* pick : {&m_centrePick, &m_emptyPick, &m_boxPick, &m_defaultPick}) {
            if (!pick->result) {
                const PickStatus status = rendering.pickStatus(pick->ticket);
                if (status == PickStatus::Pending) {
                    allReady = false;
                    continue;
                }
                pick->result = rendering.pickResult(pick->ticket);
                m_results.checks.check(pick->result->status == PickStatus::Ready, m_frame,
                                       std::string("the ") + pick->name +
                                           " pick becomes Ready: " + pick->result->error);
            }
        }
        if (allReady) {
            verifyPicks();
            m_results.picksVerified = true;
            return;
        }
        m_results.checks.check(m_frame < resultDeadline, m_frame, "picks resolve within twenty frames");
        if (m_frame >= resultDeadline) {
            m_results.picksVerified = true;
        }
    }

    void verifyPicks() {
        const PickResult& centre = *m_centrePick.result;
        m_results.checks.check(centre.region.width == 3 && centre.ids.size() == 9 && centre.depth.size() == 9, m_frame,
                               "the centre pick covers its 3x3 region");
        if (centre.ids.size() == 9) {
            const uint32_t id = centre.ids[4];
            m_results.checks.check(id != 0 && id <= centre.objects.size() && centre.objects[id - 1].entity == m_centre,
                                   m_frame, "the centre pixel names the centre cube");
            m_results.checks.check(centre.depth[4] > 0.0f && centre.depth[4] <= 1.0f, m_frame,
                                   "the centre pixel has a reversed-Z depth");
            const std::optional<glm::vec3> surface = pickWorldPosition(centre, viewSize / 2, viewSize / 2);
            m_results.checks.check(surface && std::abs(surface->x) < 0.1f && std::abs(surface->y) < 0.1f &&
                                       std::abs(surface->z - 1.0f) < 0.05f,
                                   m_frame, "the centre pixel's depth reconstructs the cube's front face");
        }
        const PickResult& empty = *m_emptyPick.result;
        m_results.checks.check(empty.ids.size() == 16 &&
                                   std::ranges::all_of(empty.ids, [](uint32_t id) { return id == 0; }),
                               m_frame, "a pick on empty space returns no object");
        const PickResult& box = *m_boxPick.result;
        m_results.checks.check(box.region.x == 0 && box.region.width == viewSize && box.region.height == viewSize,
                               m_frame, "a pick past the view's edges is clamped to the view");
        std::vector<SceneEntityId> expected{m_centre, m_side};
        std::ranges::sort(expected);
        m_results.checks.check(pickedEntities(box) == expected, m_frame, "the whole-view pick names both cubes");
        const PickResult& defaultView = *m_defaultPick.result;
        m_results.checks.check(defaultView.ids.size() == 9 && defaultView.ids[4] != 0 &&
                                   pickedEntities(defaultView) == std::vector<SceneEntityId>{m_centre},
                               m_frame, "the default view's centre names the centre cube");
    }

    void checkCancellation(RenderRuntime& rendering) {
        if (m_frame == resizeFrame) {
            // The size request below resizes the view before it renders again.
            m_resizedPick = rendering.requestPick(m_view, {.x = 10, .y = 10, .width = 2, .height = 2});
        }
        if (m_frame == resizeFrame + 2) {
            m_results.checks.check(rendering.pickResult(m_resizedPick).status == PickStatus::Cancelled, m_frame,
                                   "resizing the view cancels its pending pick");
        }
        if (m_frame == destroyFrame) {
            const RenderViewId doomed = rendering.createView();
            const PickTicket ticket = rendering.requestPick(doomed, {});
            m_results.checks.check(static_cast<bool>(ticket) && rendering.destroyView(doomed), m_frame,
                                   "a pick on a new view is accepted before the view is destroyed");
            m_results.checks.check(rendering.pickStatus(ticket) == PickStatus::Cancelled, m_frame,
                                   "destroying the view cancels its pick");
        }
    }

    SmokeResults& m_results;
    SceneEntityId m_centre;
    SceneEntityId m_side;
    RenderViewId m_view;
    PendingPick m_centrePick;
    PendingPick m_emptyPick;
    PendingPick m_boxPick;
    PendingPick m_defaultPick;
    PickTicket m_resizedPick;
    uint32_t m_frame{0};
};

class PickingApplication final : public Application {
  public:
    explicit PickingApplication(SmokeResults& results)
        : m_results(results) {}

    SceneKey initialScene() const override {
        return {1};
    }

    std::unique_ptr<ApplicationScene> createScene(SceneKey) override {
        return std::make_unique<PickingScene>(m_results);
    }

  private:
    SmokeResults& m_results;
};
} // namespace

int main(int, char** argv) {
    initializeResourceLocator(argv[0]);
    Engine engine;
    if (!engine.init()) {
        std::cerr << "Engine initialization failed\n";
        engine.shutdown();
        if (std::string reason; !testsupport::vulkanDeviceAvailable(testsupport::VulkanDeviceKind::Any, reason)) {
            std::cout << "Skipping: " << reason << '\n';
            return testsupport::skipExitCode;
        }
        return validationExitCode(engine.graphicsValidation(), EXIT_FAILURE);
    }
    SmokeResults results;
    PickingApplication application(results);
    const bool ran = engine.run(application);
    engine.shutdown();
    results.checks.check(results.picksVerified, "the picks were never checked (is ZENITH_SMOKE_FRAMES at least 60?)");
    const bool passed = ran && results.checks.failures() == 0;
    if (passed) {
        std::cout << "Picking smoke test passed\n";
    }
    return validationExitCode(engine.graphicsValidation(), passed ? EXIT_SUCCESS : EXIT_FAILURE);
}
