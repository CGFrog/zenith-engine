#include "HeadlessImGui.hpp"

#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"
#include "editor/theme/EntityIcon.hpp"
#include "editor/theme/GlassBackdrop.hpp"
#include "editor/theme/PixelIcons.hpp"
#include "editor/theme/PixelPainter.hpp"
#include "editor/theme/ThemeChrome.hpp"
#include "engine/audio/AudioSystem.hpp"
#include "engine/graphics/Rendering.hpp"
#include "engine/physics/PhysicsComponents.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <thread>

namespace {
int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

bool whole(float value) {
    return std::floor(value) == value;
}

void testMetricsSitOnThePixelGrid() {
    expect(artPixelForScale(0.5f) == 1.0f && artPixelForScale(1.0f) == 1.0f && artPixelForScale(1.6f) == 2.0f,
           "display scales round to a whole art pixel of at least one");
    for (const float artPixel : {1.0f, 2.0f, 3.0f}) {
        ImGuiStyle style;
        applyEditorMetrics(style, artPixel);
        expect(style.FrameRounding == 0.0f && style.WindowRounding == 0.0f && style.TabRounding == 0.0f &&
                   style.ScrollbarRounding == 0.0f && style.GrabRounding == 0.0f,
               "the pixel theme has no rounded corners");
        expect(!style.AntiAliasedLines && !style.AntiAliasedFill, "the pixel theme draws hard edges");
        expect(style.FramePadding.x == 4.0f * artPixel && style.FramePadding.y == 2.0f * artPixel &&
                   style.FrameBorderSize == artPixel,
               "spacing and borders scale with the art pixel");
        expect(whole(style.ItemSpacing.x) && whole(style.ItemSpacing.y) && whole(style.WindowPadding.x) &&
                   whole(style.DockingSeparatorSize),
               "spacing is whole screen pixels");
        expect(editorArtPixel() == artPixel, "the theme remembers the art pixel its metrics use");
    }
    applyEditorMetrics(ImGui::GetStyle(), 1.0f);
}

void testColorsComeFromThePalette() {
    ImGuiStyle style;
    applyEditorColors(style);
    const ImVec4 text = style.Colors[ImGuiCol_Text];
    expect(text.x == palette::ink.r && text.y == palette::ink.g && text.z == palette::ink.b,
           "text uses the palette ink");
    expect(style.Colors[ImGuiCol_DockingEmptyBg].w == 0.0f && style.Colors[ImGuiCol_MenuBarBg].w == 0.0f,
           "empty dock space and menu bars let the painted glass show");
}

void testIconsAndPainterDraw() {
    HeadlessImGuiFrame frame("Icons");
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    for (size_t index = 0; index < editorIconCount; ++index) {
        for (const float pixelSize : {1.0f, 2.0f, 3.0f}) {
            for (const bool disabled : {false, true}) {
                const int before = drawList->VtxBuffer.Size;
                drawIcon(drawList, static_cast<EditorIcon>(index), {10.0f, 10.0f}, pixelSize, {.disabled = disabled});
                expect(drawList->VtxBuffer.Size > before, "every icon draws at every size and state");
            }
        }
    }
    const int before = drawList->VtxBuffer.Size;
    pixel::notchedRect(drawList, {10.0f, 10.0f}, {10.0f, 40.0f}, IM_COL32_WHITE, 2.0f);
    pixel::notchedRect(drawList, {40.0f, 40.0f}, {10.0f, 10.0f}, IM_COL32_WHITE, 2.0f);
    pixel::splitGloss(drawList, {0.0f, 0.0f}, {0.0f, 0.0f}, IM_COL32_WHITE, IM_COL32_BLACK, 1.0f);
    pixel::notchedOutline(drawList, {5.0f, 5.0f}, {5.0f, 9.0f}, IM_COL32_WHITE, 1.0f);
    expect(drawList->VtxBuffer.Size == before, "empty and inverted rectangles draw nothing");
    pixel::notchedRect(drawList, {0.0f, 0.0f}, {3.0f, 3.0f}, IM_COL32_WHITE, 10.0f);
    pixel::glintStripes(drawList, {0.0f, 0.0f}, {200.0f, 30.0f}, IM_COL32_WHITE, IM_COL32_WHITE, 1.0f);
    expect(drawList->VtxBuffer.Size > before, "an oversized notch still leaves a body to fill");
}

void testWidgetsKeepImGuiIds() {
    ImGuiID buttonId = 0;
    {
        HeadlessImGuiFrame frame("Widgets");
        widgets::Button("Add Fog");
        buttonId = ImGui::GetItemID();
        widgets::ToolButton("Move", EditorIcon::ToolMove, true, {24.0f, 24.0f});
        widgets::SectionHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen);
        widgets::StatusPill(EditorIcon::Play, "Play", palette::buttonActiveTop, palette::buttonActiveBottom,
                            palette::activeInk);
        widgets::CaptionText("Ready", palette::captionInk);
    }
    const ImGuiWindow* window = HeadlessImGuiContext::findWindow("Widgets");
    expect(window != nullptr && buttonId == HeadlessImGuiContext::itemId(*window, "Add Fog"),
           "a themed button has ImGui::Button's id");
    HeadlessImGuiContext::activateNextFrame(buttonId);
    bool pressed = false;
    bool toolPressed = false;
    {
        HeadlessImGuiFrame frame("Widgets");
        pressed = widgets::Button("Add Fog");
        toolPressed = widgets::ToolButton("Move", EditorIcon::ToolMove, true, {24.0f, 24.0f});
    }
    expect(pressed && !toolPressed, "activating a themed button's id presses it");
}

void testEntityIcons() {
    Scene scene;
    const Entity camera = scene.createEntity("Camera");
    scene.registry().emplace<CameraComponent>(camera);
    const Entity sun = scene.createEntity("Sun");
    scene.registry().emplace<DirectionalLightComponent>(sun);
    const Entity lamp = scene.createEntity("Lamp");
    scene.registry().emplace<PointLightComponent>(lamp);
    const Entity speaker = scene.createEntity("Speaker");
    scene.registry().emplace<AudioEmitterComponent>(speaker);
    const Entity crate = scene.createEntity("Crate");
    scene.registry().emplace<RigidBodyComponent>(crate);
    const Entity empty = scene.createEntity("Empty");
    const entt::registry& registry = scene.registry();
    expect(entityIcon(registry, camera) == EditorIcon::Camera, "cameras show the camera icon");
    expect(entityIcon(registry, sun) == EditorIcon::Sun, "directional lights show the sun");
    expect(entityIcon(registry, lamp) == EditorIcon::PointLight, "point lights show a bulb");
    expect(entityIcon(registry, speaker) == EditorIcon::AudioEmitter, "audio emitters show a speaker");
    expect(entityIcon(registry, crate) == EditorIcon::Physics, "physics bodies show the physics icon");
    expect(entityIcon(registry, empty) == EditorIcon::Empty, "a bare entity is empty");
    expect(entityIcon(registry, entt::null) == EditorIcon::Empty, "an invalid entity is empty");
}

void testBackdropWithoutRenderer() {
    RenderRuntime rendering;
    GlassBackdrop backdrop;
    // The first wallpaper is generated on a worker thread; wait for it.
    for (int attempt = 0; attempt < 400 && !rendering.uiImageAlive({.index = 0, .generation = 1}); ++attempt) {
        backdrop.update(rendering, {0.0f, 0.0f}, {320.0f, 180.0f}, 1.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    expect(rendering.uiImageAlive({.index = 0, .generation = 1}) &&
               rendering.uiImageAlive({.index = 1, .generation = 1}),
           "the backdrop requests a sharp and a frosted wallpaper image");
    expect(!backdrop.texturesReady(), "without a renderer no wallpaper texture is published");
    {
        HeadlessImGuiFrame frame("Backdrop");
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const int before = drawList->VtxBuffer.Size;
        backdrop.drawWallpaper(drawList);
        backdrop.drawFrosted(drawList, {10.0f, 10.0f}, {100.0f, 60.0f}, 1.0f);
        expect(drawList->VtxBuffer.Size > before, "the backdrop draws fallbacks until its textures exist");
    }
    backdrop.release(rendering);
    expect(!rendering.uiImageAlive({.index = 0, .generation = 1}), "releasing destroys the wallpaper images");
}

void testChromeOverSeveralFrames() {
    ThemeChrome chrome;
    RenderRuntime rendering;
    ThemeChrome::installPopupBevels(*ImGui::GetCurrentContext());
    ThemeChrome::installPopupBevels(*ImGui::GetCurrentContext());
    int bevelHooks = 0;
    for (const ImGuiContextHook& hook : ImGui::GetCurrentContext()->Hooks) {
        bevelHooks += hook.Type == ImGuiContextHookType_EndFramePre ? 1 : 0;
    }
    expect(bevelHooks == 1, "installing popup bevels twice adds one hook");
    for (int frame = 0; frame < 4; ++frame) {
        ImGui::NewFrame();
        chrome.beginFrame(rendering);
        ImGui::Begin("Glass bar");
        chrome.paintBarWindow();
        chrome.paintAreaWindow();
        if (frame == 1) {
            ImGui::OpenPopup("Menu");
        }
        if (ImGui::BeginPopup("Menu")) {
            ImGui::TextUnformatted("Item");
            ImGui::EndPopup();
        }
        ImGui::End();
        ImGui::Render();
    }
    chrome.release(rendering);
    expect(ImGui::GetDrawData() != nullptr && ImGui::GetDrawData()->CmdListsCount > 0, "the chrome frames render");
}
} // namespace

int main() {
    HeadlessImGuiContext imgui;
    applyEditorMetrics(ImGui::GetStyle(), 1.0f);
    applyEditorColors(ImGui::GetStyle());
    testMetricsSitOnThePixelGrid();
    testColorsComeFromThePalette();
    testIconsAndPainterDraw();
    testWidgetsKeepImGuiIds();
    testEntityIcons();
    testBackdropWithoutRenderer();
    testChromeOverSeveralFrames();
    if (failures != 0) {
        std::cerr << failures << " editor theme test(s) failed\n";
        return 1;
    }
    std::cout << "Editor theme tests passed\n";
    return 0;
}
