#pragma once

// Draws the renderer's projected debug labels over the game's frame.

#include "engine/graphics/Rendering.hpp"

class GameUiContext;

// Draws the debug labels the renderer projected into view at its last frame
// (RenderRuntime::viewDebugLabels: the labels of RenderRuntime::debugDraw and
// of the view's own list) as game UI text over the image, so a game shows the
// labels its systems record, as the editor's viewports do. Labels lag the
// recording by one frame, the frame they were projected in. The view's
// pixels are mapped onto the UI's viewport; call it in the UI phase, after
// GameUiContext::beginFrame.
void drawDebugLabels(const RenderRuntime& rendering, RenderViewId view, GameUiContext& ui);
