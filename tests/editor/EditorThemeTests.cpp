#include "HeadlessImGui.hpp"

#include "editor/application/EditorAppearance.hpp"
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
#include <filesystem>
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

void testThemeParametersApply() {
    expect(artPixelForUiScale(0, 1.6f) == 2.0f && artPixelForUiScale(3, 1.0f) == 3.0f &&
               artPixelForUiScale(9, 1.0f) == static_cast<float>(maximumUiScale),
           "the UI scale follows the display unless overridden by a whole multiplier");
    ImGuiStyle designed;
    applyEditorColors(designed);
    const ImVec4 glow = designed.Colors[ImGuiCol_NavCursor];
    expect(glow.x == palette::pixelGlow.r && glow.y == palette::pixelGlow.g &&
               designed.Colors[ImGuiCol_WindowBg].w == 1.0f,
           "the default parameters keep the designed glow and opaque panes");

    ImGuiStyle tuned;
    const ThemeParameters violet{.paneOpacity = 0.95f, .accent = ThemeAccent::Violet};
    applyEditorColors(tuned, violet);
    const ThemeColor accent = themeAccentInfo(ThemeAccent::Violet).color;
    expect(tuned.Colors[ImGuiCol_NavCursor].x == accent.r && tuned.Colors[ImGuiCol_TabSelectedOverline].z == accent.b &&
               tuned.Colors[ImGuiCol_SliderGrabActive].y == accent.g,
           "the accent replaces the glow everywhere it is drawn");
    expect(std::abs(tuned.Colors[ImGuiCol_WindowBg].w - 0.95f) < 0.0001f, "pane opacity reaches the window background");
    expect(tuned.Colors[ImGuiCol_Text].x == palette::ink.r, "the accent leaves text colours alone");

    ImGui::GetStyle() = tuned;
    expect(editorAccent() == accent.withAlpha(1.0f), "widgets read the accent from the current style");
    applyEditorColors(ImGui::GetStyle());
}

void testBackdropWithoutWallpaper() {
    RenderRuntime rendering;
    GlassBackdrop backdrop;
    for (int frame = 0; frame < 20; ++frame) {
        backdrop.update(rendering, {0.0f, 0.0f}, {320.0f, 180.0f}, 1.0f, {.wallpaper = false});
    }
    expect(!rendering.uiImageAlive({.index = 0, .generation = 1}), "with the wallpaper off no wallpaper is generated");
    expect(!backdrop.wallpaperShown(), "with the wallpaper off the backdrop shows no wallpaper");
    {
        HeadlessImGuiFrame frame("No wallpaper");
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const int before = drawList->VtxBuffer.Size;
        backdrop.drawWallpaper(drawList);
        backdrop.drawFrosted(drawList, {10.0f, 10.0f}, {100.0f, 60.0f}, 1.0f);
        expect(drawList->VtxBuffer.Size > before, "the flat sky and glass fill are drawn instead");
    }
    backdrop.release(rendering);

    ThemeChrome chrome;
    chrome.setParameters({.frostRadius = 99, .accent = ThemeAccent::Sky});
    expect(chrome.parameters().frostRadius == ThemeParameters::maximumFrostRadius &&
               chrome.parameters().accent == ThemeAccent::Sky,
           "the chrome keeps its parameters clamped");
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

// Fonts are added between frames, as the application and sessions do.
void testAppearanceAppliesPreferences() {
    ImGuiIO& io = ImGui::GetIO();
    EditorAppearance appearance(ZENITH_EDITOR_FONT, 1.0f);
    expect(appearance.current() == EditorAppearanceSettings{}, "before any apply the look is the default");
    const EditorAppearanceSettings tuned{.theme = {.accent = ThemeAccent::Violet}, .uiScale = 2};
    const EditorAppearanceResult applied = appearance.apply(tuned);
    expect(static_cast<bool>(applied) && editorArtPixel() == 2.0f &&
               editorAccent() == themeAccentInfo(ThemeAccent::Violet).color.withAlpha(1.0f),
           "applying preferences sets the UI scale's metrics and the accent");
    expect(appearance.current() == tuned && appearance.theme().accent == ThemeAccent::Violet,
           "the applied look becomes the current one");
    ImFont* scaledFont = io.FontDefault;
    expect(scaledFont != nullptr, "the UI scale's fonts become the default");

    expect(static_cast<bool>(appearance.apply({})) && editorArtPixel() == 1.0f &&
               editorAccent() == palette::pixelGlow.withAlpha(1.0f),
           "applying the defaults restores the designed look");
    const int fontCount = io.Fonts->Fonts.Size;
    expect(static_cast<bool>(appearance.apply(tuned)) && io.FontDefault == scaledFont &&
               io.Fonts->Fonts.Size == fontCount,
           "returning to a UI scale reuses its fonts instead of loading them again");

    EditorAppearance missingFonts(std::filesystem::path(ZENITH_EDITOR_FONT).parent_path() / "missing.ttf", 1.0f);
    const EditorAppearanceResult failed = missingFonts.apply({.uiScale = 3});
    expect(!failed && !failed.error.empty() && editorArtPixel() == 3.0f,
           "a font that cannot load is reported while the metrics still apply");
    expect(static_cast<bool>(missingFonts.apply({.uiScale = 3})), "applying the current look again does nothing");

    // Later frames need the grown atlas built, as a renderer would upload it.
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    applyEditorMetrics(ImGui::GetStyle(), 1.0f);
    applyEditorColors(ImGui::GetStyle());
}
} // namespace

int main() {
    HeadlessImGuiContext imgui;
    applyEditorMetrics(ImGui::GetStyle(), 1.0f);
    applyEditorColors(ImGui::GetStyle());
    testMetricsSitOnThePixelGrid();
    testColorsComeFromThePalette();
    testThemeParametersApply();
    testBackdropWithoutWallpaper();
    testIconsAndPainterDraw();
    testWidgetsKeepImGuiIds();
    testEntityIcons();
    testBackdropWithoutRenderer();
    testChromeOverSeveralFrames();
    testAppearanceAppliesPreferences();
    if (failures != 0) {
        std::cerr << failures << " editor theme test(s) failed\n";
        return 1;
    }
    std::cout << "Editor theme tests passed\n";
    return 0;
}
