#pragma once

#include "editor/theme/EditorPalette.hpp"
#include "editor/theme/PixelIcons.hpp"

#include <imgui.h>

// Pixel-theme versions of common ImGui widgets. Each calls the ImGui widget
// with the same label (so the same id, navigation, and test hooks) drawn
// invisibly, then paints the themed look over the exact item rectangle.
namespace widgets {

// A palette colour for drawing, faded with ImGui's style alpha (so disabled
// items fade too).
ImU32 color(ThemeColor themeColor);

// A split-gloss button: bevelled, a mint ring when hovered, pressed in while
// held. An active button is drawn lit (the current tool or tab).
bool Button(const char* label, ImVec2 size = {0.0f, 0.0f}, bool active = false);
// A square tool button showing an icon; label is its id and tooltip-free
// name, as with ImGui::Button(label).
bool ToolButton(const char* label, EditorIcon icon, bool active, ImVec2 size);
// A collapsing section header drawn as a split-gloss bar with a pixel
// chevron and the title font. Returns whether the section is open.
bool SectionHeader(const char* label, ImGuiTreeNodeFlags flags = 0);
// Draws icon in the label slot of the last item, offset by labelOffset from
// the item's left edge and centred vertically.
void ItemIcon(EditorIcon icon, float labelOffset, IconState state = {});
// Leading spaces that reserve room for an ItemIcon at the start of a label.
const char* IconLabelPadding();
// A lit pill with an icon and caption, as in the status bar.
void StatusPill(EditorIcon icon, const char* text, ThemeColor top, ThemeColor bottom, ThemeColor ink);
// Caption text in the title font with a one-pixel outline, for glass.
void CaptionText(const char* text, ThemeColor ink);

} // namespace widgets
