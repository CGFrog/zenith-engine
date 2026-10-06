// Bounded Vulkan smoke test of secondary render scenes (RenderRuntime::
// createScene and setViewScene): the application's scene renders in the
// default view while a created view draws a second, separately owned scene in
// the same frames, with preview lighting. The secondary scene's entities carry
// ids above 100, which the application's scene never reaches, so a GPU pick
// in that view proves which scene it drew. The secondary view also checks:
// its draws count only its own objects, it honours the editor's viewport
// hide while the application's scene renders in Runtime mode, it outlines its
// own objects, it skips frames whose scene was not bound, and a scene
// destroyed and created again in the same slot starts from its new content.
// Run with ZENITH_SMOKE_FRAMES (the test registration uses 90) and
// optionally ZENITH_VALIDATION_STRICT=1.

#include "engine/Engine.hpp"
#include "engine/graphics/GraphicsValidation.hpp"
#include "engine/resources/ResourceLocator.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {
// Ids the secondary scene burns before creating its objects, so its objects'
// ids never collide with the application's scene's.
constexpr uint64_t secondaryIdFloor = 100;

struct SmokeResults {
    std::vector<std::string> failures;
    uint32_t framesWithSecondaryView{0};
    bool secondaryPickHit{false};

    void expect(bool condition, uint32_t frame, const std::string& message) {
        if (!condition) {
            failures.push_back("frame " + std::to_string(frame) + ": " + message);
        }
    }
};

glm::mat4 cameraWorld(const glm::vec3& position, const glm::vec3& target) {
    return glm::inverse(glm::lookAtRH(position, target, glm::vec3(0.0f, 1.0f, 0.0f)));
}

Entity addCube(Scene& scene, MeshHandle cube, const char* name, const glm::vec3& position) {
    const Entity entity = scene.createEntity(name);
    scene.registry().get<TransformComponent>(entity).local = glm::translate(glm::mat4(1.0f), position);
    scene.markTransformDirty(entity);
    scene.attachMesh(entity, cube);
    return entity;
}

// The secondary scene: `cubes` cubes in a row, one more hidden in editor
// views, and nothing else (no camera, lights, or environment).
void buildSecondaryScene(Scene& scene, MeshHandle cube, int cubes) {
    scene.clear();
    for (uint64_t burned = 0; burned < secondaryIdFloor; ++burned) {
        scene.destroyEntity(scene.createEntity("Burned"));
    }
    for (int index = 0; index < cubes; ++index) {
        addCube(scene, cube, "Preview cube", {static_cast<float>(index - cubes / 2) * 1.5f, 0.0f, 0.0f});
    }
    const Entity hidden = addCube(scene, cube, "Hidden preview cube", {0.0f, 2.0f, 0.0f});
    scene.registry().emplace<VisibilityComponent>(hidden, VisibilityComponent{.hidden = true});
    scene.updateWorldTransforms();
}

class TwoSceneScene final : public ApplicationScene {
  public:
    explicit TwoSceneScene(SmokeResults& results)
        : m_results(results) {}

    SceneResult load(ApplicationContext& context) override {
        const AssetLoadResult<MeshHandle> cube = context.assets.loadMesh(assetRoot() / "primitives" / "cube.gltf", 0);
        if (!cube) {
            return {.success = false, .error = "Cannot load the cube mesh: " + cube.error};
        }
        m_cube = cube.handle;
        Scene& scene = context.scene;
        for (int x = -2; x <= 2; ++x) {
            for (int z = -2; z <= 2; ++z) {
                addCube(scene, m_cube, "Level cube",
                        {static_cast<float>(x) * 2.5f, 0.5f, static_cast<float>(z) * 2.5f});
            }
        }
        const Entity camera = scene.createEntity("Camera");
        scene.registry().get<TransformComponent>(camera).local = cameraWorld({0.0f, 4.0f, 12.0f}, glm::vec3(0.0f));
        scene.markTransformDirty(camera);
        scene.registry().emplace<CameraComponent>(camera);
        scene.markCameraDirty(camera);
        const Entity sun = scene.createEntity("Sun");
        scene.registry().emplace<DirectionalLightComponent>(sun);
        scene.markLightDirty(sun);
        buildSecondaryScene(m_secondary, m_cube, 3);
        return {.success = true};
    }

    void update(ApplicationContext& context, const InputState&, float) override {
        ++m_frame;
        RenderRuntime& rendering = context.rendering;
        checkPreviousFrame(rendering);
        if (m_frame == 1) {
            // The application's scene plays: Runtime ignores the editor hide.
            rendering.setSceneVisibilityMode(SceneVisibilityMode::Runtime);
            m_view = rendering.createView();
            m_scene = rendering.createScene();
            m_results.expect(m_view && m_scene, m_frame, "a view and a secondary scene are created");
            m_results.expect(rendering.setViewScene(m_view, m_scene), m_frame, "the view binds the scene");
            m_results.expect(rendering.setViewOverride(m_view, RenderView{.position = {0.0f, 1.0f, 6.0f},
                                                                          .forward = {0.0f, -0.15f, -1.0f},
                                                                          .verticalFovRadians = glm::radians(50.0f),
                                                                          .nearPlane = 0.1f,
                                                                          .farPlane = 50.0f}),
                             m_frame, "the view accepts a camera");
        }
        if (m_frame == 20) {
            const Entity first = firstPreviewCube();
            const RenderObjectId outlined = static_cast<RenderObjectId>(entt::to_integral(first));
            m_results.expect(rendering.setViewOutlinedObjects(m_view, std::span(&outlined, 1)), m_frame,
                             "the view outlines a secondary object");
        }
        if (m_frame == 30) {
            m_pick = rendering.requestPick(m_view, {.x = 150, .y = 100, .width = 20, .height = 20});
            m_results.expect(static_cast<bool>(m_pick), m_frame, "a pick is requested in the secondary view");
        }
        if (m_frame == 60) {
            // Destroyed and created again in the same slot, with other content.
            m_results.expect(rendering.destroyScene(m_scene), m_frame, "the scene is destroyed");
            m_scene = rendering.createScene();
            buildSecondaryScene(m_secondary, m_cube, 5);
            m_results.expect(rendering.setViewScene(m_view, m_scene), m_frame, "the view binds the new scene");
        }
        if (m_pick && rendering.pickStatus(m_pick) == PickStatus::Ready) {
            const PickResult result = rendering.pickResult(m_pick);
            for (const PickedObject& object : result.objects) {
                const Entity entity = m_secondary.findEntity(object.entity);
                m_results.secondaryPickHit =
                    m_results.secondaryPickHit || (object.entity.value > secondaryIdFloor && entity != entt::null);
                m_results.expect(object.entity.value > secondaryIdFloor, m_frame,
                                 "the secondary view's pick names only secondary objects");
            }
            m_pick = {};
        }
        // Frames 40-44 leave the scene unbound: the view must not render.
        const bool bound = m_frame < 40 || m_frame >= 45;
        if (bound) {
            m_results.expect(rendering.setSceneContent(m_scene, &m_secondary), m_frame, "the scene content binds");
        }
        rendering.requestViewSize(m_view, 320, 240);
    }

    void fixedUpdate(ApplicationContext&, const InputState&, float) override {}

    void unload(ApplicationContext&) override {}

  private:
    Entity firstPreviewCube() const {
        const auto names = m_secondary.registry().view<const NameComponent>();
        for (const Entity entity : names) {
            if (names.get<const NameComponent>(entity).name == "Preview cube") {
                return entity;
            }
        }
        return entt::null;
    }

    // Stats describe the frame submitted before this update.
    void checkPreviousFrame(const RenderRuntime& rendering) {
        const uint32_t frame = m_frame - 1;
        if (frame < 3) {
            return;
        }
        const RenderViewStats level = rendering.viewStats(rendering.defaultView());
        const RenderViewStats preview = rendering.viewStats(m_view);
        m_results.expect(level.rendered, frame, "the default view renders every frame");
        if (frame >= 40 && frame < 45) {
            m_results.expect(!preview.rendered, frame, "the view skips frames its scene was not bound on");
            m_results.expect(static_cast<bool>(rendering.viewTexture(m_view)), frame, "the view keeps its texture");
            return;
        }
        if (frame == 60) {
            return;
        }
        m_results.expect(preview.rendered && preview.width == 320, frame, "the secondary view renders at 320x240");
        // Three (later five) cubes; the hidden one stays hidden in the
        // Editor mode secondary scenes use, though the frame's scene plays.
        const uint32_t expected = frame < 60 ? 3u : 5u;
        m_results.expect(preview.visibleCount <= expected && preview.visibleCount > 0, frame,
                         "the secondary view draws only its own visible objects (" +
                             std::to_string(preview.visibleCount) + " visible)");
        m_results.expect(level.visibleCount > expected, frame, "the default view draws the application's scene");
        if (preview.rendered) {
            ++m_results.framesWithSecondaryView;
        }
    }

    SmokeResults& m_results;
    MeshHandle m_cube;
    Scene m_secondary;
    RenderViewId m_view;
    RenderSceneId m_scene;
    PickTicket m_pick;
    uint32_t m_frame{0};
};

class TwoSceneApplication final : public Application {
  public:
    explicit TwoSceneApplication(SmokeResults& results)
        : m_results(results) {}

    SceneKey initialScene() const override {
        return {1};
    }

    std::unique_ptr<ApplicationScene> createScene(SceneKey) override {
        return std::make_unique<TwoSceneScene>(m_results);
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
        return validationExitCode(engine.graphicsValidation(), EXIT_FAILURE);
    }
    SmokeResults results;
    TwoSceneApplication application(results);
    const bool ran = engine.run(application);
    engine.shutdown();
    for (const std::string& failure : results.failures) {
        std::cerr << "FAILED: " << failure << '\n';
    }
    if (results.framesWithSecondaryView < 40) {
        std::cerr << "FAILED: the secondary view rendered on " << results.framesWithSecondaryView
                  << " frames (is ZENITH_SMOKE_FRAMES at least 90?)\n";
    }
    if (!results.secondaryPickHit) {
        std::cerr << "FAILED: no pick in the secondary view hit a secondary object\n";
    }
    const bool passed =
        ran && results.failures.empty() && results.framesWithSecondaryView >= 40 && results.secondaryPickHit;
    if (passed) {
        std::cout << "Two-scene smoke test passed\n";
    }
    return validationExitCode(engine.graphicsValidation(), passed ? EXIT_SUCCESS : EXIT_FAILURE);
}
