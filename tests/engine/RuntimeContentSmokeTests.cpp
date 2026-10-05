// Bounded Vulkan smoke test of runtime content: three boxes whose mesh,
// material and texture are all created in code (AssetManager::createMesh,
// createMaterial, createTexture). The left box carries a red
// MaterialOverrideComponent tint, the middle one draws its plain white
// material, and the right one replaces its material with a runtime green one.
// The registration captures a frame (ZENITH_CAPTURE_FRAME and
// ZENITH_CAPTURE_PATH) and this test checks each box's colour in it; a second
// registration repeats it on the GPU-driven path (ZENITH_FORCE_GPU_DRIVEN).

#include "engine/Engine.hpp"
#include "engine/assets/MeshBuilder.hpp"
#include "engine/diagnostics/RgbaImage.hpp"
#include "engine/graphics/GraphicsValidation.hpp"
#include "engine/resources/ResourceLocator.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {
constexpr float boxSpacing = 3.0f;
constexpr float boxHalfExtent = 0.9f;
const glm::vec3 cameraPosition{0.0f, 0.0f, 9.0f};
constexpr float verticalFov = glm::radians(50.0f);

struct SmokeResults {
    std::vector<std::string> failures;
    uint32_t checkedFrames{0};

    void expect(bool condition, const std::string& message) {
        if (!condition) {
            failures.push_back(message);
        }
    }
};

class RuntimeContentScene final : public ApplicationScene {
  public:
    explicit RuntimeContentScene(SmokeResults& results)
        : m_results(results) {}

    SceneResult load(ApplicationContext& context) override {
        AssetManager& assets = context.assets;
        const AssetLoadResult<TextureHandle> texture = assets.createTexture(
            TextureAsset{.width = 2, .height = 2, .rgba = std::vector<uint8_t>(16, 255)}, "smoke white texture");
        if (!texture) {
            return {.success = false, .error = texture.error};
        }
        MaterialAsset white;
        white.baseColor = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
        white.metallic = 0.0f;
        white.roughness = 0.7f;
        white.baseColorTexture = texture.handle;
        const AssetLoadResult<MaterialHandle> whiteMaterial = assets.createMaterial(white, "smoke white");
        MaterialAsset green = white;
        green.baseColor = glm::vec4(0.05f, 0.8f, 0.05f, 1.0f);
        const AssetLoadResult<MaterialHandle> greenMaterial = assets.createMaterial(green, "smoke green");
        if (!whiteMaterial || !greenMaterial) {
            return {.success = false, .error = whiteMaterial.error + greenMaterial.error};
        }
        const MeshBuildResult built =
            MeshBuilder().beginPrimitive(whiteMaterial.handle).box(glm::vec3(0.0f), glm::vec3(boxHalfExtent)).build();
        if (!built) {
            return {.success = false, .error = built.error};
        }
        const AssetLoadResult<MeshHandle> box = assets.createMesh(built.mesh, "smoke box");
        if (!box) {
            return {.success = false, .error = box.error};
        }
        // The creator holds the runtime assets for the scene's lifetime.
        m_leases = {assets.lease(box.handle), assets.lease(greenMaterial.handle)};

        Scene& scene = context.scene;
        entt::registry& registry = scene.registry();
        for (int index = -1; index <= 1; ++index) {
            const Entity entity = scene.createEntity("Runtime box");
            scene.setLocalTransform(entity, glm::translate(glm::mat4(1.0f), glm::vec3(boxSpacing * index, 0.0f, 0.0f)));
            scene.attachMesh(entity, box.handle);
            if (index == -1) {
                registry.emplace<MaterialOverrideComponent>(
                    entity, MaterialOverrideComponent{.baseColorFactor = {1.0f, 0.08f, 0.08f, 1.0f}});
            } else if (index == 1) {
                registry.emplace<MaterialOverrideComponent>(
                    entity, MaterialOverrideComponent{.material = greenMaterial.handle});
            }
            scene.markMaterialDirty(entity);
        }
        const Entity camera = scene.createEntity("Camera");
        scene.setLocalTransform(camera, glm::inverse(glm::lookAtRH(cameraPosition, glm::vec3(0.0f), {0, 1, 0})));
        registry.emplace<CameraComponent>(camera, CameraComponent{.verticalFovRadians = verticalFov});
        scene.markCameraDirty(camera);
        const Entity sun = scene.createEntity("Sun");
        registry.emplace<DirectionalLightComponent>(
            sun,
            DirectionalLightComponent{.direction = glm::normalize(glm::vec3(0.2f, 0.4f, 1.0f)), .intensity = 1.0f});
        registry.emplace<EnvironmentLightComponent>(sun);
        scene.markLightDirty(sun);
        return {.success = true};
    }

    // Stats describe the frame submitted before this update.
    void update(ApplicationContext& context, const InputState&, float) override {
        if (++m_frame < 4) {
            return;
        }
        const RenderStats& stats = context.rendering.stats();
        m_results.expect(stats.renderableCount == 3,
                         "the three runtime boxes are drawn, not " + std::to_string(stats.renderableCount));
        m_results.expect(stats.droppedRenderItems == 0, "no render item is dropped");
        ++m_results.checkedFrames;
    }

    void fixedUpdate(ApplicationContext&, const InputState&, float) override {}

    void unload(ApplicationContext&) override {
        m_leases.clear();
    }

  private:
    SmokeResults& m_results;
    uint32_t m_frame{0};

    struct Leases {
        AssetLease<MeshHandle> mesh;
        AssetLease<MaterialHandle> material;

        void clear() {
            mesh = {};
            material = {};
        }
    } m_leases;
};

class RuntimeContentApplication final : public Application {
  public:
    explicit RuntimeContentApplication(SmokeResults& results)
        : m_results(results) {}

    SceneKey initialScene() const override {
        return {1};
    }

    std::unique_ptr<ApplicationScene> createScene(SceneKey) override {
        return std::make_unique<RuntimeContentScene>(m_results);
    }

  private:
    SmokeResults& m_results;
};

// Mean colour of a 5x5 block around the pixel where world point lands.
glm::vec3 sampleAt(const RgbaImage& image, const glm::vec3& point) {
    const float aspect = static_cast<float>(image.width) / static_cast<float>(image.height);
    const glm::mat4 projection = glm::perspective(verticalFov, aspect, 0.1f, 100.0f);
    const glm::mat4 view = glm::lookAtRH(cameraPosition, glm::vec3(0.0f), {0, 1, 0});
    const glm::vec4 clip = projection * view * glm::vec4(point, 1.0f);
    const glm::vec2 ndc = glm::vec2(clip) / clip.w;
    const int centerX = static_cast<int>((ndc.x * 0.5f + 0.5f) * static_cast<float>(image.width));
    const int centerY = static_cast<int>((0.5f - ndc.y * 0.5f) * static_cast<float>(image.height));
    glm::vec3 sum(0.0f);
    int count = 0;
    for (int y = centerY - 2; y <= centerY + 2; ++y) {
        for (int x = centerX - 2; x <= centerX + 2; ++x) {
            if (x < 0 || y < 0 || x >= static_cast<int>(image.width) || y >= static_cast<int>(image.height)) {
                continue;
            }
            const size_t base =
                (static_cast<size_t>(y) * image.width + static_cast<size_t>(x)) * RgbaImage::channelCount;
            sum += glm::vec3(image.pixels[base], image.pixels[base + 1], image.pixels[base + 2]);
            ++count;
        }
    }
    return count > 0 ? sum / static_cast<float>(count) : glm::vec3(0.0f);
}

std::string describe(const glm::vec3& color) {
    return "(" + std::to_string(static_cast<int>(color.r)) + ", " + std::to_string(static_cast<int>(color.g)) + ", " +
           std::to_string(static_cast<int>(color.b)) + ")";
}

void checkCapture(const std::filesystem::path& path, SmokeResults& results) {
    RgbaImage image;
    if (const ImageFileResult loaded = loadPng(path, image); !loaded || !image.valid()) {
        results.failures.push_back("cannot read the capture " + path.string() + ": " + loaded.error);
        return;
    }
    // The front faces' centres.
    const glm::vec3 red = sampleAt(image, {-boxSpacing, 0.0f, boxHalfExtent});
    const glm::vec3 white = sampleAt(image, {0.0f, 0.0f, boxHalfExtent});
    const glm::vec3 green = sampleAt(image, {boxSpacing, 0.0f, boxHalfExtent});
    // Ambient and specular light add white to every box, so the tints are
    // judged by how far their channel stands above the others.
    results.expect(red.r > red.g + 40.0f && red.r > red.b + 40.0f, "the tinted box is red, not " + describe(red));
    const float whiteMax = std::max({white.r, white.g, white.b});
    const float whiteMin = std::min({white.r, white.g, white.b});
    results.expect(whiteMin > 40.0f && whiteMax < 1.35f * whiteMin,
                   "the plain box is a light neutral grey, not " + describe(white));
    results.expect(green.g > green.r + 40.0f && green.g > green.b + 40.0f,
                   "the box with a replacement material is green, not " + describe(green));
}
} // namespace

int main(int, char** argv) {
    initializeResourceLocator(argv[0]);
    // Copied: SDL may reallocate its environment once the engine starts.
    const char* captureVariable = SDL_getenv("ZENITH_CAPTURE_PATH");
    const std::filesystem::path capturePath = captureVariable != nullptr ? captureVariable : "";
    if (capturePath.empty() || SDL_getenv("ZENITH_CAPTURE_FRAME") == nullptr) {
        std::cerr << "Set ZENITH_CAPTURE_FRAME and ZENITH_CAPTURE_PATH (the CTest registration does)\n";
        return EXIT_FAILURE;
    }
    std::error_code ignored;
    std::filesystem::remove(capturePath, ignored);
    Engine engine;
    if (!engine.init()) {
        std::cerr << "Engine initialization failed\n";
        engine.shutdown();
        return validationExitCode(engine.graphicsValidation(), EXIT_FAILURE);
    }
    SmokeResults results;
    RuntimeContentApplication application(results);
    const bool ran = engine.run(application);
    engine.shutdown();
    if (!ran) {
        results.failures.push_back("the run ended with an error");
    }
    if (results.checkedFrames == 0) {
        results.failures.push_back("no frame was checked (is ZENITH_CAPTURE_FRAME at least 5?)");
    }
    checkCapture(capturePath, results);
    for (const std::string& failure : results.failures) {
        std::cerr << "FAILED: " << failure << '\n';
    }
    const bool passed = results.failures.empty();
    if (passed) {
        std::cout << "Runtime content smoke test passed\n";
    }
    return validationExitCode(engine.graphicsValidation(), passed ? EXIT_SUCCESS : EXIT_FAILURE);
}
