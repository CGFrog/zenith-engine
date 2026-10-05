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

void testToggle() {
    GameUiContext ui;
    bool value = false;
    InputState click;
    click.mousePosition = {50.0f, 50.0f};
    click.setMouseButtonPressed(MouseButton::Left);
    ui.beginFrame({1920, 1080}, click);
    expect(ui.toggle("toggle", {{0.0f, 0.0f}, {400.0f, 100.0f}}, "Toggle", value) && value,
           "clicking a toggle turns it on");
    ui.beginFrame({1920, 1080}, {});
    expect(!ui.toggle("toggle", {{0.0f, 0.0f}, {400.0f, 100.0f}}, "Toggle", value) && value,
           "a toggle keeps its value without input");
    ui.beginFrame({1920, 1080}, {});
    ui.setNavigation({.activatePressed = true});
    expect(ui.toggle("toggle", {{0.0f, 0.0f}, {400.0f, 100.0f}}, "Toggle", value) && !value,
           "activating a focused toggle turns it off");
    expect(ui.commands().size() == 4, "a toggle draws its frame, label, box, and fill");
}

void testSlider() {
    GameUiContext ui;
    const UiRect rect{{0.0f, 0.0f}, {1000.0f, 100.0f}};
    float value = 0.0f;
    // The track spans x 560 to 960.
    InputState press;
    press.mousePosition = {760.0f, 50.0f};
    press.setMouseButtonPressed(MouseButton::Left);
    press.setMouseButtonDown(MouseButton::Left);
    ui.beginFrame({1920, 1080}, press);
    expect(ui.slider("slider", rect, "Slider", value, 0.0f, 10.0f, 1.0f) && value == 5.0f,
           "pressing on the track sets the slider");
    InputState drag;
    drag.mousePosition = {1500.0f, 300.0f};
    drag.setMouseButtonDown(MouseButton::Left);
    ui.beginFrame({1920, 1080}, drag);
    expect(ui.slider("slider", rect, "Slider", value, 0.0f, 10.0f, 1.0f) && value == 10.0f,
           "dragging past the end of the track clamps to the maximum, even off the widget");
    InputState released;
    released.mousePosition = {560.0f, 50.0f};
    ui.beginFrame({1920, 1080}, released);
    expect(!ui.slider("slider", rect, "Slider", value, 0.0f, 10.0f, 1.0f) && value == 10.0f,
           "releasing the pointer ends the drag");

    ui.beginFrame({1920, 1080}, released);
    ui.setNavigation({.leftPressed = true});
    expect(ui.slider("slider", rect, "Slider", value, 0.0f, 10.0f, 1.0f) && value == 9.0f,
           "left navigation steps the focused slider down");
    value = 10.0f;
    ui.beginFrame({1920, 1080}, released);
    ui.setNavigation({.rightPressed = true});
    expect(!ui.slider("slider", rect, "Slider", value, 0.0f, 10.0f, 1.0f) && value == 10.0f,
           "a slider at its maximum does not step past it");
    value = 3.3f;
    ui.beginFrame({1920, 1080}, {});
    ui.slider("slider", rect, "Slider", value, 0.0f, 10.0f, 1.0f);
    expect(value == 3.0f, "a slider rounds its value to its step");
}

void testProgressBarAndLayout() {
    GameUiContext ui;
    ui.beginFrame({1920, 1080}, {});
    ui.progressBar({{0.0f, 0.0f}, {200.0f, 20.0f}}, 0.25f);
    const auto* fill = std::get_if<UiPanelCommand>(&ui.commands().back().payload);
    expect(ui.commands().size() == 2 && fill != nullptr && fill->rect.size.x == 50.0f,
           "a progress bar fills its fraction of the width");
    ui.progressBar({{0.0f, 0.0f}, {200.0f, 20.0f}}, 2.0f);
    fill = std::get_if<UiPanelCommand>(&ui.commands().back().payload);
    expect(fill != nullptr && fill->rect.size.x == 200.0f, "a progress bar clamps its fraction");
    ui.progressBar({{0.0f, 0.0f}, {200.0f, 20.0f}}, std::nanf(""));
    expect(ui.commands().size() == 5, "an empty or invalid progress bar draws only its background");

    UiLayout column = UiLayout::column({10.0f, 20.0f}, 5.0f);
    const UiRect first = column.next({100.0f, 30.0f});
    const UiRect second = column.next({100.0f, 30.0f});
    expect(first.position == glm::vec2(10.0f, 20.0f) && second.position == glm::vec2(10.0f, 55.0f),
           "a column places widgets downward with spacing");
    UiLayout row = UiLayout::row({0.0f, 0.0f}, 4.0f);
    row.next({50.0f, 10.0f});
    expect(row.next({20.0f, 10.0f}).position == glm::vec2(54.0f, 0.0f) && row.cursor() == glm::vec2(78.0f, 0.0f),
           "a row places widgets rightward with spacing");
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
    ui.pixelText({100.0f, 50.0f}, "label", 18.0f, glm::vec4(1.0f), 7);
    const auto* pixel = std::get_if<UiTextCommand>(&ui.commands().back().payload);
    expect(pixel != nullptr && pixel->position == glm::vec2(100.0f, 50.0f) && std::abs(pixel->size - 18.0f) < 1e-4f &&
               ui.commands().back().drawOrder == 7,
           "pixel text keeps its pixel position and size whatever the design scale");

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

    testToggle();
    testSlider();
    testProgressBarAndLayout();

    return failures == 0 ? 0 : 1;
}
