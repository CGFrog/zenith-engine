#pragma once

// CPU decisions shared by Renderer and MeshPass when a frame records several views.

#include "Lighting.hpp"

#include <cstdint>
#include <span>
#include <vector>

// CPU-side decisions shared by the Renderer and MeshPass when a frame records
// several render views (see RenderRuntime). Pure functions, so the multi-view
// policy is testable without a GPU.

// Reorders the view slots rendered this frame (given in slot order) into
// recording order: the active view first when it renders, then the others in
// slot order. The first view leads the frame: its camera places the shadow
// cascades, selects the lights whose shadows are rendered, and sorts the
// particles.
void orderRenderedViews(std::vector<uint32_t>& renderedViews, uint32_t activeView);

// Shadow tiles for a view's selected lights. `firstTiles[i]` is the first
// local-shadow tile of `tiledLights[i]` (negative when unshadowed); each light
// of `viewLights` receives the entry of the tiled light with the same stable
// id, or -1 when the frame rendered no shadow for it.
std::vector<int32_t> remapLocalShadowTiles(std::span<const LocalLight> tiledLights, std::span<const int32_t> firstTiles,
                                           std::span<const LocalLight> viewLights);
