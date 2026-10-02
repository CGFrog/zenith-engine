#pragma once

#include "engine/graphics/Rendering.hpp"

#include <vector>

// One row of the per-pass GPU timing table. Nested rows are already included
// in the duration of the nearest preceding row that is not nested.
struct GpuPassTiming {
    const char* name{""};
    double milliseconds{0.0};
    bool nested{false};
};

namespace GraphicsSettingsEditor {
// Retired-frame GPU durations in submission order, ending with the whole frame.
// Empty while the renderer has no valid timings.
std::vector<GpuPassTiming> gpuPassTimings(const RenderStats& stats);

// Clamps every field into the range the render-settings document accepts, so
// any edited settings serialize and load back unchanged.
void clampToDocumentRanges(RenderSettings& settings);

// Draws a control for every RenderSettings field and returns true when one
// changed. Options the capabilities rule out stay visible but disabled.
bool drawSettings(RenderSettings& settings, const RenderCapabilities& capabilities);

// Draws the gpuPassTimings table.
void drawPassTimings(const RenderStats& stats);
} // namespace GraphicsSettingsEditor
