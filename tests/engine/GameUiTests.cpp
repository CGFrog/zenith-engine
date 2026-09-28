#include "engine/ui/GameUi.hpp"

#include <cmath>
#include <iostream>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}
} // namespace

int main() {
    GameUiContext ui;
    ui.beginFrame({1920, 1080}, {});
    expect(std::abs(ui.scale() - 1.0f) < 0.0001f, "the design canvas is one-to-one at 1080p");
    const UiRect centered = ui.resolve({{-100.0f, -50.0f}, {200.0f, 100.0f}}, UiAnchor::Center);
    expect(centered.position == glm::vec2(860.0f, 490.0f), "center anchors resolve around the viewport center");

    ui.beginFrame({2560, 1080}, {});
    const UiRect ultrawide = ui.resolve({{0.0f, 0.0f}, {100.0f, 100.0f}}, UiAnchor::TopLeft);
    expect(ultrawide.position == glm::vec2(320.0f, 0.0f),
           "ultrawide layout preserves the design aspect ratio with centered gutters");
    ui.beginFrame({3840, 2160}, {});
    expect(std::abs(ui.scale() - 2.0f) < 0.0001f, "high-DPI viewports scale the design canvas uniformly");

    InputState pointer;
    pointer.mousePosition = {960.0f, 540.0f};
    pointer.setMouseButtonPressed(MouseButton::Left);
    ui.beginFrame({1920, 1080}, pointer);
    expect(ui.button("pointer", {{-100.0f, -40.0f}, {200.0f, 80.0f}}, "Pointer", UiAnchor::Center),
           "pointer hit testing activates buttons in resolved screen coordinates");

    ui.beginFrame({1920, 1080}, {});
    ui.button("first", {{-100.0f, -80.0f}, {200.0f, 60.0f}}, "First", UiAnchor::Center);
    ui.button("second", {{-100.0f, 20.0f}, {200.0f, 60.0f}}, "Second", UiAnchor::Center);
    ui.beginFrame({1920, 1080}, {});
    ui.setNavigation({.nextPressed = true, .activatePressed = true});
    const bool firstActivated = ui.button("first", {{-100.0f, -80.0f}, {200.0f, 60.0f}}, "First", UiAnchor::Center);
    const bool secondActivated = ui.button("second", {{-100.0f, 20.0f}, {200.0f, 60.0f}}, "Second", UiAnchor::Center);
    expect(!firstActivated && secondActivated && ui.focusedId() == "second",
           "action navigation advances focus and activates only the focused button");

    ui.panel({{10.0f, 20.0f}, {100.0f, 50.0f}}, {.color = glm::vec4(0.5f)}, UiAnchor::TopLeft, 3,
             UiRect{{0.0f, 0.0f}, {50.0f, 50.0f}});
    ui.text({20.0f, 30.0f}, "Text", 24.0f);
    ui.image({{30.0f, 40.0f}, {64.0f, 64.0f}}, TextureHandle{0, 1});
    expect(ui.commands().size() == 7, "panels, button layers, text, and images produce renderer commands");

    return failures == 0 ? 0 : 1;
}
