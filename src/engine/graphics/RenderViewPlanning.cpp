#include "RenderViewPlanning.hpp"

#include <algorithm>
#include <unordered_map>

void orderRenderedViews(std::vector<uint32_t>& renderedViews, uint32_t activeView) {
    const auto active = std::ranges::find(renderedViews, activeView);
    if (active != renderedViews.end()) {
        std::rotate(renderedViews.begin(), active, active + 1);
    }
}

std::vector<int32_t> remapLocalShadowTiles(std::span<const LocalLight> tiledLights, std::span<const int32_t> firstTiles,
                                           std::span<const LocalLight> viewLights) {
    std::unordered_map<uint64_t, int32_t> tilesByLight;
    tilesByLight.reserve(tiledLights.size());
    for (size_t index = 0; index < tiledLights.size() && index < firstTiles.size(); ++index) {
        if (firstTiles[index] >= 0) {
            tilesByLight.emplace(tiledLights[index].stableId, firstTiles[index]);
        }
    }
    std::vector<int32_t> tiles(viewLights.size(), -1);
    for (size_t index = 0; index < viewLights.size(); ++index) {
        if (const auto found = tilesByLight.find(viewLights[index].stableId); found != tilesByLight.end()) {
            tiles[index] = found->second;
        }
    }
    return tiles;
}
