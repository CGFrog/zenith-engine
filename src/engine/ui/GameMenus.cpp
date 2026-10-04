#include "GameMenus.hpp"

#include <cstdio>
#include <type_traits>

namespace {
constexpr glm::vec2 rowSize{640.0f, 64.0f};
constexpr float rowSpacing{14.0f};
constexpr float titleHeight{96.0f};
constexpr float panelPadding{40.0f};
constexpr int32_t menuDrawOrder{100};
const UiStyle panelStyle{.color = {0.025f, 0.035f, 0.055f, 0.96f}, .cornerRadius = 18.0f};

// A centred panel tall enough for rows, with the title at its top, and the
// layout for the rows below the title.
UiLayout beginMenu(GameUiContext& ui, const std::string& title, size_t rows) {
    const float rowsHeight = static_cast<float>(rows) * (rowSize.y + rowSpacing) - rowSpacing;
    const float height = titleHeight + rowsHeight + panelPadding * 2.0f;
    const glm::vec2 topLeft{-rowSize.x * 0.5f - panelPadding, -height * 0.5f};
    ui.panel({topLeft, {rowSize.x + panelPadding * 2.0f, height}}, panelStyle, UiAnchor::Center, menuDrawOrder);
    ui.text(topLeft + glm::vec2(panelPadding, panelPadding), title, 44.0f, glm::vec4(1.0f), UiAnchor::Center,
            menuDrawOrder + 1);
    return UiLayout::column(topLeft + glm::vec2(panelPadding, panelPadding + titleHeight), rowSpacing);
}

std::string formatValue(const SettingsSlider& slider, float value) {
    if (slider.format) {
        return slider.format(value);
    }
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", static_cast<double>(value));
    return text;
}

// Draws one entry's row; true when the player changed it.
bool drawEntry(GameUiContext& ui, const SettingsEntry& entry, UiRect row) {
    const std::string id = "settings." + entry.id;
    return std::visit(
        [&](const auto& control) {
            using Control = std::decay_t<decltype(control)>;
            if constexpr (std::is_same_v<Control, SettingsToggle>) {
                bool value = control.get();
                if (!ui.toggle(id, row, entry.label, value, UiAnchor::Center, menuDrawOrder + 2)) {
                    return false;
                }
                control.set(value);
                return true;
            } else if constexpr (std::is_same_v<Control, SettingsChoice>) {
                if (control.options.empty()) {
                    return false;
                }
                const size_t current = control.get() % control.options.size();
                if (!ui.button(id, row, entry.label + ": " + control.options[current], UiAnchor::Center,
                               menuDrawOrder + 2)) {
                    return false;
                }
                control.set((current + 1) % control.options.size());
                return true;
            } else {
                float value = control.get();
                if (!ui.slider(id, row, entry.label + ": " + formatValue(control, value), value, control.minimum,
                               control.maximum, control.step, UiAnchor::Center, menuDrawOrder + 2)) {
                    return false;
                }
                control.set(value);
                return true;
            }
        },
        entry.control);
}
} // namespace

MenuAction drawPauseMenu(GameUiContext& ui, const PauseMenuOptions& options) {
    const size_t rows = 1 + (options.showSettings ? 1 : 0) + (options.showQuit ? 1 : 0);
    UiLayout layout = beginMenu(ui, options.title, rows);
    MenuAction action = MenuAction::None;
    if (ui.button("pause.resume", layout.next(rowSize), options.resumeLabel, UiAnchor::Center, menuDrawOrder + 2)) {
        action = MenuAction::Resume;
    }
    if (options.showSettings &&
        ui.button("pause.settings", layout.next(rowSize), options.settingsLabel, UiAnchor::Center, menuDrawOrder + 2)) {
        action = MenuAction::OpenSettings;
    }
    if (options.showQuit &&
        ui.button("pause.quit", layout.next(rowSize), options.quitLabel, UiAnchor::Center, menuDrawOrder + 2)) {
        action = MenuAction::Quit;
    }
    return action;
}

MenuAction drawSettingsMenu(GameUiContext& ui, std::span<const SettingsEntry> entries,
                            const SettingsMenuOptions& options) {
    UiLayout layout = beginMenu(ui, options.title, entries.size() + 1);
    MenuAction action = MenuAction::None;
    for (const SettingsEntry& entry : entries) {
        if (drawEntry(ui, entry, layout.next(rowSize))) {
            action = MenuAction::Changed;
        }
    }
    if (ui.button("settings.back", layout.next(rowSize), options.backLabel, UiAnchor::Center, menuDrawOrder + 2)) {
        action = MenuAction::Back;
    }
    return action;
}
