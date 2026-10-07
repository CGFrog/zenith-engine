// The editor's pixel theme: metrics on the pixel grid, palette colours and
// accents, the chrome, icons and the pixel painter, themed widgets and their
// ImGui ids, copyable text, entity icons, and applying appearance preferences.

#include "HeadlessImGui.hpp"

#include "editor/application/EditorAppearance.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"
#include "editor/theme/EntityIcon.hpp"
#include "editor/theme/PixelIcons.hpp"
#include "editor/theme/PixelPainter.hpp"
#include "editor/theme/ThemeChrome.hpp"
#include "engine/audio/AudioSystem.hpp"
#include "engine/physics/PhysicsComponents.hpp"

#include "support/Doctest.hpp"

#include <cmath>
#include <filesystem>
#include <string>

namespace {

// A headless ImGui context styled as the editor styles its own.
struct ThemedImGui {
    ThemedImGui() {
        applyEditorMetrics(ImGui::GetStyle(), 1.0f);
        applyEditorColors(ImGui::GetStyle());
    }

    HeadlessImGuiContext imgui;
};

bool whole(float value) {
    return std::floor(value) == value;
}

} // namespace

TEST_SUITE_BEGIN("EditorTheme");

TEST_CASE("EditorTheme: metrics sit on the pixel grid") {
    const ThemedImGui imgui;
    CHECK_MESSAGE((artPixelForScale(0.5f) == 1.0f && artPixelForScale(1.0f) == 1.0f && artPixelForScale(1.6f) == 2.0f),
                  "display scales round to a whole art pixel of at least one");
    for (const float artPixel : {1.0f, 2.0f, 3.0f}) {
        CAPTURE(artPixel);
        ImGuiStyle style;
        applyEditorMetrics(style, artPixel);
        CHECK_MESSAGE((style.FrameRounding == 0.0f && style.WindowRounding == 0.0f && style.TabRounding == 0.0f &&
                       style.ScrollbarRounding == 0.0f && style.GrabRounding == 0.0f),
                      "the pixel theme has no rounded corners");
        CHECK_MESSAGE((!style.AntiAliasedLines && !style.AntiAliasedFill), "the pixel theme draws hard edges");
        CHECK_MESSAGE((style.FramePadding.x == 4.0f * artPixel && style.FramePadding.y == 2.0f * artPixel &&
                       style.FrameBorderSize == artPixel),
                      "spacing and borders scale with the art pixel");
        CHECK_MESSAGE((whole(style.ItemSpacing.x) && whole(style.ItemSpacing.y) && whole(style.WindowPadding.x) &&
                       whole(style.DockingSeparatorSize)),
                      "spacing is whole screen pixels");
        CHECK_MESSAGE(editorArtPixel() == artPixel, "the theme remembers the art pixel its metrics use");
    }
    applyEditorMetrics(ImGui::GetStyle(), 1.0f);
}

TEST_CASE("EditorTheme: colors come from the palette") {
    const ThemedImGui imgui;
    ImGuiStyle style;
    applyEditorColors(style);
    const ImVec4 text = style.Colors[ImGuiCol_Text];
    CHECK_MESSAGE((text.x == palette::ink.r && text.y == palette::ink.g && text.z == palette::ink.b),
                  "text uses the palette ink");
    CHECK_MESSAGE((style.Colors[ImGuiCol_DockingEmptyBg].w == 0.0f && style.Colors[ImGuiCol_MenuBarBg].w == 0.0f),
                  "empty dock space and menu bars let the painted gap and header strips show");
    CHECK_MESSAGE((style.Colors[ImGuiCol_WindowBg].w == 1.0f && style.Colors[ImGuiCol_WindowBg].x == palette::pane.r),
                  "windows are the opaque pane grey");
}

TEST_CASE("EditorTheme: theme parameters apply") {
    const ThemedImGui imgui;
    CHECK_MESSAGE((artPixelForUiScale(0, 1.6f) == 2.0f && artPixelForUiScale(3, 1.0f) == 3.0f &&
                   artPixelForUiScale(9, 1.0f) == static_cast<float>(maximumUiScale)),
                  "the UI scale follows the display unless overridden by a whole multiplier");
    ImGuiStyle designed;
    applyEditorColors(designed);
    const ImVec4 designedAccent = designed.Colors[ImGuiCol_NavCursor];
    CHECK_MESSAGE((designedAccent.x == palette::accentBlue.r && designedAccent.y == palette::accentBlue.g),
                  "the default parameters keep the designed blue accent");

    ImGuiStyle tuned;
    const ThemeParameters violet{.accent = ThemeAccent::Violet};
    applyEditorColors(tuned, violet);
    const ThemeColor accent = themeAccentInfo(ThemeAccent::Violet).color;
    CHECK_MESSAGE((tuned.Colors[ImGuiCol_NavCursor].x == accent.r &&
                   tuned.Colors[ImGuiCol_TabSelectedOverline].z == accent.b &&
                   tuned.Colors[ImGuiCol_SliderGrabActive].y == accent.g),
                  "the accent replaces the default everywhere it is drawn");
    CHECK_MESSAGE(
        (tuned.Colors[ImGuiCol_Text].x == palette::ink.r && tuned.Colors[ImGuiCol_WindowBg].x == palette::pane.r),
        "the accent leaves text and pane colours alone");

    ImGui::GetStyle() = tuned;
    CHECK_MESSAGE(editorAccent() == accent.withAlpha(1.0f), "widgets read the accent from the current style");
    applyEditorColors(ImGui::GetStyle());
}

TEST_CASE("EditorTheme: chrome clamps parameters") {
    const ThemedImGui imgui;
    ThemeChrome chrome;
    chrome.setParameters({.accent = ThemeAccent::Teal});
    CHECK_MESSAGE(chrome.parameters().accent == ThemeAccent::Teal, "the chrome keeps a supported accent");
    chrome.setParameters({.accent = static_cast<ThemeAccent>(99)});
    CHECK_MESSAGE(chrome.parameters().accent == ThemeAccent::Blue, "the chrome keeps its parameters clamped");
}

TEST_CASE("EditorTheme: icons and painter draw") {
    const ThemedImGui imgui;
    HeadlessImGuiFrame frame("Icons");
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    for (size_t index = 0; index < editorIconCount; ++index) {
        for (const float pixelSize : {1.0f, 2.0f, 3.0f}) {
            for (const bool disabled : {false, true}) {
                const int before = drawList->VtxBuffer.Size;
                drawIcon(drawList, static_cast<EditorIcon>(index), {10.0f, 10.0f}, pixelSize, {.disabled = disabled});
                CHECK_MESSAGE(drawList->VtxBuffer.Size > before, "every icon draws at every size and state");
            }
        }
    }
    const int before = drawList->VtxBuffer.Size;
    pixel::rect(drawList, {10.0f, 10.0f}, {10.0f, 40.0f}, IM_COL32_WHITE);
    pixel::rect(drawList, {40.0f, 40.0f}, {10.0f, 10.0f}, IM_COL32_WHITE);
    pixel::outline(drawList, {5.0f, 5.0f}, {5.0f, 9.0f}, IM_COL32_WHITE, 1.0f);
    pixel::bevel(drawList, {0.0f, 0.0f}, {0.0f, 0.0f}, IM_COL32_WHITE, IM_COL32_BLACK, 1.0f);
    CHECK_MESSAGE(drawList->VtxBuffer.Size == before, "empty and inverted rectangles draw nothing");
    pixel::outline(drawList, {0.0f, 0.0f}, {3.0f, 3.0f}, IM_COL32_WHITE, 10.0f);
    CHECK_MESSAGE(drawList->VtxBuffer.Size > before, "an outline thicker than its rectangle still fills it");
}

TEST_CASE("EditorTheme: widgets keep ImGui ids") {
    const ThemedImGui imgui;
    ImGuiID buttonId = 0;
    {
        HeadlessImGuiFrame frame("Widgets");
        widgets::Button("Add Fog");
        buttonId = ImGui::GetItemID();
        widgets::ToolButton("Move", EditorIcon::ToolMove, true, {24.0f, 24.0f});
        widgets::SectionHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen);
        widgets::StatusPill(EditorIcon::Play, "Play", widgets::StatusPillStyle::Lit);
        widgets::PaintButtonFrame(ImGui::GetWindowDrawList(), {0.0f, 0.0f}, {40.0f, 20.0f},
                                  widgets::ButtonFrame::Disabled);
        widgets::CaptionText("Ready", palette::captionInk);
    }
    const ImGuiWindow* window = HeadlessImGuiContext::findWindow("Widgets");
    CHECK_MESSAGE((window != nullptr && buttonId == HeadlessImGuiContext::itemId(*window, "Add Fog")),
                  "a themed button has ImGui::Button's id");
    HeadlessImGuiContext::activateNextFrame(buttonId);
    bool pressed = false;
    bool toolPressed = false;
    {
        HeadlessImGuiFrame frame("Widgets");
        pressed = widgets::Button("Add Fog");
        toolPressed = widgets::ToolButton("Move", EditorIcon::ToolMove, true, {24.0f, 24.0f});
    }
    CHECK_MESSAGE((pressed && !toolPressed), "activating a themed button's id presses it");
}

TEST_CASE("EditorTheme: copyable text") {
    const ThemedImGui imgui;
    const std::string message = "Build failed: error C2065";
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 center{};
    const auto draw = [&](bool& copied) {
        HeadlessImGuiFrame frame("Copyable");
        copied = widgets::CopyableText(message, palette::error);
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        center = {(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f};
    };
    bool copied = false;
    draw(copied);
    io.AddMousePosEvent(center.x, center.y);
    draw(copied);
    CHECK_MESSAGE(!copied, "hovering error text alone copies nothing");
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_C, true);
    draw(copied);
    io.AddKeyEvent(ImGuiKey_C, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    CHECK_MESSAGE(copied, "Ctrl+C over error text copies it");
    const char* clipboard = ImGui::GetClipboardText();
    CHECK_MESSAGE((clipboard != nullptr && message == clipboard), "the clipboard holds the whole message");

    // Empty text has nothing to copy.
    bool emptyCopied = true;
    {
        HeadlessImGuiFrame frame("Copyable empty");
        emptyCopied = widgets::CopyableText("");
    }
    CHECK_MESSAGE(!emptyCopied, "empty text is never copied");
}

TEST_CASE("EditorTheme: entity icons") {
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
    CHECK_MESSAGE(entityIcon(registry, camera) == EditorIcon::Camera, "cameras show the camera icon");
    CHECK_MESSAGE(entityIcon(registry, sun) == EditorIcon::Sun, "directional lights show the sun");
    CHECK_MESSAGE(entityIcon(registry, lamp) == EditorIcon::PointLight, "point lights show a bulb");
    CHECK_MESSAGE(entityIcon(registry, speaker) == EditorIcon::AudioEmitter, "audio emitters show a speaker");
    CHECK_MESSAGE(entityIcon(registry, crate) == EditorIcon::Physics, "physics bodies show the physics icon");
    CHECK_MESSAGE(entityIcon(registry, empty) == EditorIcon::Empty, "a bare entity is empty");
    CHECK_MESSAGE((entityIcon(registry, entt::null) == EditorIcon::Empty), "an invalid entity is empty");
}

TEST_CASE("EditorTheme: chrome over several frames") {
    const ThemedImGui imgui;
    ThemeChrome chrome;
    ThemeChrome::installPopupBevels(*ImGui::GetCurrentContext());
    ThemeChrome::installPopupBevels(*ImGui::GetCurrentContext());
    int bevelHooks = 0;
    for (const ImGuiContextHook& hook : ImGui::GetCurrentContext()->Hooks) {
        bevelHooks += hook.Type == ImGuiContextHookType_EndFramePre ? 1 : 0;
    }
    CHECK_MESSAGE(bevelHooks == 1, "installing popup bevels twice adds one hook");
    for (int frame = 0; frame < 4; ++frame) {
        // Later frames run with the Play tint, which also paints the edge.
        chrome.setModeTint(frame >= 2 ? ThemeModeTint::Play : ThemeModeTint::None);
        ImGui::NewFrame();
        chrome.beginFrame();
        ImGui::Begin("Header bar");
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
    CHECK_MESSAGE((ImGui::GetDrawData() != nullptr && ImGui::GetDrawData()->CmdListsCount > 0),
                  "the chrome frames render");
}

// Fonts are added between frames, as the application and sessions do.
TEST_CASE("EditorTheme: appearance applies preferences") {
    const ThemedImGui imgui;
    ImGuiIO& io = ImGui::GetIO();
    EditorAppearance appearance(ZENITH_EDITOR_FONT, 1.0f);
    CHECK_MESSAGE(appearance.current() == EditorAppearanceSettings{}, "before any apply the look is the default");
    const EditorAppearanceSettings tuned{.theme = {.accent = ThemeAccent::Violet}, .uiScale = 2};
    const EditorAppearanceResult applied = appearance.apply(tuned);
    CHECK_MESSAGE((static_cast<bool>(applied) && editorArtPixel() == 2.0f &&
                   editorAccent() == themeAccentInfo(ThemeAccent::Violet).color.withAlpha(1.0f)),
                  "applying preferences sets the UI scale's metrics and the accent");
    CHECK_MESSAGE((appearance.current() == tuned && appearance.theme().accent == ThemeAccent::Violet),
                  "the applied look becomes the current one");
    ImFont* scaledFont = io.FontDefault;
    CHECK_MESSAGE(scaledFont != nullptr, "the UI scale's fonts become the default");

    CHECK_MESSAGE((static_cast<bool>(appearance.apply({})) && editorArtPixel() == 1.0f &&
                   editorAccent() == palette::accentBlue.withAlpha(1.0f)),
                  "applying the defaults restores the designed look");
    const int fontCount = io.Fonts->Fonts.Size;
    CHECK_MESSAGE((static_cast<bool>(appearance.apply(tuned)) && io.FontDefault == scaledFont &&
                   io.Fonts->Fonts.Size == fontCount),
                  "returning to a UI scale reuses its fonts instead of loading them again");

    EditorAppearance missingFonts(std::filesystem::path(ZENITH_EDITOR_FONT).parent_path() / "missing.ttf", 1.0f);
    const EditorAppearanceResult failed = missingFonts.apply({.uiScale = 3});
    CHECK_MESSAGE((!failed && !failed.error.empty() && editorArtPixel() == 3.0f),
                  "a font that cannot load is reported while the metrics still apply");
    CHECK_MESSAGE(static_cast<bool>(missingFonts.apply({.uiScale = 3})),
                  "applying the current look again does nothing");

    // Later frames need the grown atlas built, as a renderer would upload it.
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    applyEditorMetrics(ImGui::GetStyle(), 1.0f);
    applyEditorColors(ImGui::GetStyle());
}

TEST_SUITE_END();
