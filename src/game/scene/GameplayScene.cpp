#include "GameplayScene.hpp"

#include "DemoScene.hpp"

#include <imgui.h>

#include "game/application/DemoApplication.hpp"

#include <SDL3/SDL_stdinc.h>
#include <charconv>
#include <glm/gtc/matrix_transform.hpp>
#include <string_view>

namespace {
uint32_t benchmarkObjectCount() {
    const char* value = SDL_getenv("ZENITH_BENCHMARK_OBJECTS");
    if (!value) {
        return 0;
    }
    uint32_t count = 0;
    const char* end = value + std::string_view(value).size();
    const auto [position, error] = std::from_chars(value, end, count);
    return error == std::errc{} && position == end ? std::min(count, 10000u) : 0;
}
} // namespace

GameplayScene::GameplayScene(AudioClipHandle interactionSound)
    : m_interactionSound(interactionSound) {}

SceneResult GameplayScene::load(ApplicationContext& context) {
    Scene& scene = context.scene;
    if (const SceneResult result = populateDemoScene(scene, context.assets, benchmarkObjectCount()); !result) {
        return result;
    }

    const Entity player = scene.createEntity("Player");
    scene.registry().get<TransformComponent>(player).local =
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.2f, 4.0f));
    CharacterControllerComponent controller;
    const Entity camera = scene.createEntity("Player Camera");
    controller.camera = camera;
    scene.registry().emplace<CharacterControllerComponent>(player, controller);
    scene.registry().emplace<ColliderComponent>(player, CapsuleCollider{controller.halfHeight, controller.radius});
    RigidBodyComponent playerBody{};
    playerBody.motionType = PhysicsMotionType::Dynamic;
    playerBody.layer = PhysicsLayer::Player;
    playerBody.mass = 80.0f;
    playerBody.friction = 0.0f;
    playerBody.angularDamping = 1.0f;
    playerBody.continuousCollision = true;
    playerBody.lockRotation = true;
    scene.registry().emplace<RigidBodyComponent>(player, playerBody);

    scene.registry().emplace<CameraComponent>(camera);
    scene.registry().emplace<AudioListenerComponent>(camera);
    scene.setParent(camera, player);
    scene.registry().get<TransformComponent>(camera).local =
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, controller.halfHeight + controller.radius, 0.0f));
    return {.success = true};
}

void GameplayScene::update(ApplicationContext& context, const InputState& input, float) {
    if (input.keyPressed(Key::R)) {
        context.control.requestSceneChange(demoSceneKey(DemoSceneId::Gameplay));
    }
    if (input.keyPressed(Key::E)) {
        AudioSpatialOptions options;
        options.position = {0.0f, 1.0f, -2.0f};
        options.gain = 0.8f;
        context.audio.playAt(m_interactionSound, options);
    }
    m_firstPersonControllers.updateView(context.scene, input);
}

void GameplayScene::fixedUpdate(ApplicationContext& context, const InputState& input, float deltaSeconds) {
    m_firstPersonControllers.fixedUpdate(context.scene, context.physics, input, deltaSeconds);
}

void GameplayScene::drawUi(ApplicationContext& context) {
    ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.85f);
    if (ImGui::Begin("Zenith")) {
        ImGui::TextUnformatted("UI support is active.");
        ImGui::Separator();
        ImGui::TextUnformatted("Click the viewport to capture the mouse.");
        ImGui::TextUnformatted("Press Escape to interact with tools.");
        ImGui::SeparatorText("Rendering");
        RenderSettings settings = context.rendering.settings();
        int antialiasing = static_cast<int>(settings.antialiasing);
        int shadows = static_cast<int>(settings.shadows);
        int lighting = static_cast<int>(settings.lighting);
        int bloom = static_cast<int>(settings.bloom);
        bool changed = false;
        changed |= ImGui::Combo("Anti-aliasing", &antialiasing, "Off\0FXAA\0");
        changed |= ImGui::Combo("Shadows", &shadows, "Off\0Single\0Cascaded balanced\0Cascaded high\0");
        changed |= ImGui::Combo("Lighting", &lighting, "Fast\0Physically based\0");
        changed |= ImGui::Combo("Bloom", &bloom, "Off\0Downsampled\0");
        changed |= ImGui::Checkbox("Visualize shadow cascades", &settings.visualizeShadowCascades);
        changed |= ImGui::SliderFloat("Cascade split", &settings.cascadeSplitLambda, 0.0f, 1.0f);
        changed |= ImGui::SliderFloat("Cascade blend", &settings.cascadeBlendFraction, 0.0f, 0.5f);
        if (changed) {
            settings.antialiasing = static_cast<AntialiasingMode>(antialiasing);
            settings.shadows = static_cast<ShadowQuality>(shadows);
            settings.lighting = static_cast<LightingQuality>(lighting);
            settings.bloom = static_cast<BloomQuality>(bloom);
            context.rendering.setSettings(settings);
        }
        const RenderStats& stats = context.rendering.stats();
        ImGui::Text("GPU %.2f ms (shadow %.2f, scene %.2f, post %.2f)", stats.gpuFrameMilliseconds,
                    stats.gpuShadowMilliseconds, stats.gpuSceneMilliseconds, stats.gpuPostMilliseconds);
        ImGui::Text("Visible %u / %u, culled %u", stats.visibleCount, stats.renderableCount, stats.culledCount);
        ImGui::Text("Draws %u, triangles %llu", stats.drawCalls, static_cast<unsigned long long>(stats.triangleCount));
    }
    ImGui::End();
}

void GameplayScene::unload(ApplicationContext&) {}
