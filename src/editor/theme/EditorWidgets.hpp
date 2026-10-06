#pragma once

// Themed wrappers for ImGui controls (buttons, section headers, status pills).

#include "editor/theme/EditorPalette.hpp"
#include "editor/theme/PixelIcons.hpp"

#include <imgui.h>

#include <optional>
#include <string>

// Pixel-theme versions of common ImGui widgets. Each calls the ImGui widget
// with the same label (so the same id, navigation, and test hooks) drawn
// invisibly, then paints the themed look over the exact item rectangle.
namespace widgets {

// A palette colour for drawing, faded with ImGui's style alpha (so disabled
// items fade too).
ImU32 color(ThemeColor themeColor);

// The look of a button-like frame: raised, lighter when hovered, sunk while
// pressed, lit in the selection blue when active (the current tool or tab),
// and flat when it cannot be used.
enum class ButtonFrame {
    Normal,
    Hovered,
    Pressed,
    Active,
    Disabled,
};

// Paints a square button frame over min-max, for custom-drawn controls (the
// pie menu) that must match Button.
void PaintButtonFrame(ImDrawList* drawList, ImVec2 min, ImVec2 max, ButtonFrame frame);

// How a StatusPill is lit: a plain raised pill, the selection blue (a running
// mode), or a plain pill with warning ink.
enum class StatusPillStyle {
    Normal,
    Lit,
    Warning,
};

// A raised square button: flat grey with a hard bevel, lighter when hovered,
// pressed in while held. An active button is drawn lit in the selection blue
// (the current tool or tab).
bool Button(const char* label, ImVec2 size = {0.0f, 0.0f}, bool active = false);
// A square tool button showing an icon; label is its id and tooltip-free
// name, as with ImGui::Button(label).
bool ToolButton(const char* label, EditorIcon icon, bool active, ImVec2 size);
// A collapsing section header drawn as a raised bar with a pixel chevron and
// its title in palette::sectionInk. Returns whether the section is open.
bool SectionHeader(const char* label, ImGuiTreeNodeFlags flags = 0);
// Draws icon in the label slot of the last item, offset by labelOffset from
// the item's left edge and centred vertically.
void ItemIcon(EditorIcon icon, float labelOffset, IconState state = {});
// Leading spaces that reserve room for an ItemIcon at the start of a label.
const char* IconLabelPadding();
// A raised pill with an icon and caption, as in the status bar.
void StatusPill(EditorIcon icon, const char* text, StatusPillStyle style = StatusPillStyle::Normal);
// Wrapped text the user can copy: hovering it and pressing Ctrl+C, or
// right-clicking it and choosing Copy, puts the whole text on the clipboard.
// Use it for messages the user may need to paste elsewhere, such as errors.
// Returns true on the frame the text was copied.
bool CopyableText(const std::string& text, std::optional<ThemeColor> ink = std::nullopt);
// Caption text centred on a frame-high line, for header bars.
void CaptionText(const char* text, ThemeColor ink);

} // namespace widgets
