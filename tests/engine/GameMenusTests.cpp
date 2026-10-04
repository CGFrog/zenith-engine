// The engine's pause and settings screens: what each choice returns and
// which settings callbacks run.

#include "engine/ui/GameMenus.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

InputState clickAt(glm::vec2 position) {
    InputState input;
    input.mousePosition = position;
    input.setMouseButtonPressed(MouseButton::Left);
    input.setMouseButtonDown(MouseButton::Left);
    return input;
}

// At 1080p the menus' rows are 640 wide, centred: x 640 to 1280.
void testPauseMenu() {
    GameUiContext ui;
    ui.beginFrame({1920, 1080}, {});
    expect(drawPauseMenu(ui) == MenuAction::None, "a pause menu without input chooses nothing");
    // Three rows: resume at y 478 to 542, settings 556 to 620, quit 634 to 698.
    ui.beginFrame({1920, 1080}, clickAt({960.0f, 500.0f}));
    expect(drawPauseMenu(ui) == MenuAction::Resume, "clicking Resume resumes");
    ui.beginFrame({1920, 1080}, clickAt({960.0f, 600.0f}));
    expect(drawPauseMenu(ui) == MenuAction::OpenSettings, "clicking Settings opens the settings");
    ui.beginFrame({1920, 1080}, clickAt({960.0f, 680.0f}));
    expect(drawPauseMenu(ui) == MenuAction::Quit, "clicking Quit quits");
    // Two rows: resume at y 517 to 581, quit 595 to 659.
    ui.beginFrame({1920, 1080}, clickAt({960.0f, 620.0f}));
    expect(drawPauseMenu(ui, {.showSettings = false}) == MenuAction::Quit,
           "a pause menu without Settings moves Quit up");
}

void testSettingsMenu() {
    bool toggled = false;
    size_t choice = 0;
    float volume = 0.5f;
    const std::vector<SettingsEntry> entries{
        {.id = "toggle",
         .label = "Toggle",
         .control = SettingsToggle{.get = [&] { return toggled; }, .set = [&](bool value) { toggled = value; }}},
        {.id = "choice",
         .label = "Choice",
         .control = SettingsChoice{.options = {"Low", "Medium", "High"},
                                   .get = [&] { return choice; },
                                   .set = [&](size_t value) { choice = value; }}},
        {.id = "volume",
         .label = "Volume",
         .control = SettingsSlider{.minimum = 0.0f,
                                   .maximum = 1.0f,
                                   .step = 0.1f,
                                   .get = [&] { return volume; },
                                   .set = [&](float value) { volume = value; }}},
    };
    GameUiContext ui;
    ui.beginFrame({1920, 1080}, {});
    expect(drawSettingsMenu(ui, entries) == MenuAction::None, "a settings menu without input changes nothing");

    // Four rows: y 439 to 503, 517 to 581, 595 to 659, and Back 673 to 737.
    ui.beginFrame({1920, 1080}, clickAt({960.0f, 470.0f}));
    expect(drawSettingsMenu(ui, entries) == MenuAction::Changed && toggled, "clicking a toggle row sets it");
    ui.beginFrame({1920, 1080}, clickAt({700.0f, 550.0f}));
    expect(drawSettingsMenu(ui, entries) == MenuAction::Changed && choice == 1, "clicking a choice row cycles it");
    choice = 2;
    ui.beginFrame({1920, 1080}, clickAt({700.0f, 550.0f}));
    drawSettingsMenu(ui, entries);
    expect(choice == 0, "a choice wraps around to its first option");

    InputState hover;
    hover.mousePosition = {700.0f, 620.0f};
    ui.beginFrame({1920, 1080}, hover);
    ui.setNavigation({.rightPressed = true});
    expect(drawSettingsMenu(ui, entries) == MenuAction::Changed && std::abs(volume - 0.6f) < 1.0e-5f,
           "right navigation steps the focused slider");

    ui.beginFrame({1920, 1080}, clickAt({960.0f, 700.0f}));
    expect(drawSettingsMenu(ui, entries) == MenuAction::Back, "clicking Back goes back");
}
} // namespace

int main() {
    testPauseMenu();
    testSettingsMenu();
    return failures == 0 ? 0 : 1;
}
