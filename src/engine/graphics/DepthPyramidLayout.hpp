#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>

// Level layout of the scene depth pyramid (see DepthPyramid). Shaders that
// build or read the pyramid mirror these rules; keep them in sync with
// shaders/depth_pyramid.comp and shaders/gpu_visibility.comp.
//
// Level 0 holds the scene depth of the rendered region (the render extent,
// which dynamic resolution may shrink below the storage size). Each further
// level halves the previous one with truncation, never below one texel, so
// level L is max(renderExtent >> L, 1). Every level occupies the top-left
// corner of its storage mip; texels beyond it hold stale data.
//
// On an odd source axis the last texel of the next level also covers the
// leftover source texel. Base pixel p therefore lies in texel
// min(p >> L, levelExtent(L) - 1) of level L.
namespace depth_pyramid {

// Number of levels down to a single texel on the longer axis.
inline uint32_t levelCount(glm::uvec2 baseExtent) {
    const uint32_t longest = std::max(std::max(baseExtent.x, baseExtent.y), 1u);
    return static_cast<uint32_t>(std::bit_width(longest));
}

inline glm::uvec2 levelExtent(glm::uvec2 baseExtent, uint32_t level) {
    return glm::max(glm::uvec2(baseExtent.x >> level, baseExtent.y >> level), glm::uvec2(1u));
}

// Half-open range of source-level texels that an output texel reduces.
struct SourceRange {
    glm::uvec2 begin{0u};
    glm::uvec2 end{0u};
};

inline SourceRange sourceTexels(glm::uvec2 outputTexel, glm::uvec2 outputExtent, glm::uvec2 sourceExtent) {
    SourceRange range{.begin = outputTexel * 2u, .end = outputTexel * 2u + 2u};
    for (int axis = 0; axis < 2; ++axis) {
        if (outputTexel[axis] + 1 == outputExtent[axis]) {
            range.end[axis] = sourceExtent[axis];
        }
        range.end[axis] = std::min(range.end[axis], sourceExtent[axis]);
    }
    return range;
}

} // namespace depth_pyramid
