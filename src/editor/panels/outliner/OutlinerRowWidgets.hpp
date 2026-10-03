#pragma once

#include "editor/theme/PixelIconBitmaps.hpp"

#include <imgui.h>

#include <cstddef>
#include <span>

// Drawing helpers for outliner rows.

// The pixel decoration of the row just drawn (call it right after the tree
// node): its icon in the label's reserved space (dithered when ghost), and
// for a selected row lit edges and, where no expand arrow is drawn, a cursor.
void decorateOutlinerRow(EditorIcon icon, bool selected, bool leaf, bool ghost = false);

// One restriction column of a row: its id (pushed under the row's id) and
// icon, whether the flag is on, and its tooltip.
struct RestrictionToggle {
    const char* id;
    EditorIcon icon;
    bool enabled;
    const char* tooltip;
};

// Draws toggles right-aligned on the row spanning rowMin..rowMax (an item
// that allows overlap), as small icon buttons: a flag that is off shows a
// dithered ghost icon. Call it while the cursor is at the start of the next
// row, which stays where it is. Returns the index of the toggle clicked
// this frame, or -1.
int drawRestrictionToggles(ImVec2 rowMin, ImVec2 rowMax, std::span<const RestrictionToggle> toggles);

// Width the restriction toggles take at the right of every row.
float restrictionColumnsWidth(size_t count);
