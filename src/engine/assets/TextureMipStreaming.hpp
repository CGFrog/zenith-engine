#pragma once

// Renderer-independent planning for streaming a texture's mip chain into GPU memory.

#include "AssetStreaming.hpp"

#include <cstdint>
#include <span>
#include <vector>

// Renderer-independent planning for streaming a texture's mip chain into GPU
// memory. A streamed texture becomes resident with its smallest levels (the
// tail) and then gains higher-resolution levels one contiguous range at a time,
// so the resident levels are always [residentFirstMip, mipCount).

struct TextureMipResidency {
    uint32_t mipCount{0};
    // Highest-resolution resident level; mipCount when nothing is resident.
    uint32_t residentFirstMip{0};
    // Highest-resolution level the renderer wants resident.
    uint32_t requestedFirstMip{0};

    bool complete() const {
        return residentFirstMip <= requestedFirstMip;
    }

    uint32_t residentLevels() const {
        return mipCount - residentFirstMip;
    }

    uint32_t requestedLevels() const {
        return mipCount - requestedFirstMip;
    }
};

// First level of the tail a streamed texture is created with: the
// highest-resolution level whose width and height both fit in
// `tailMaximumExtent` texels. The last level always qualifies, so a texture
// never starts with nothing resident. Levels follow the usual halving rule
// (max(1, extent >> level)).
uint32_t textureMipTailFirstLevel(uint32_t width, uint32_t height, uint32_t mipCount, uint32_t tailMaximumExtent);

// How much a texture matters this frame, from the bounds of something that
// samples it: projectedVisibility approximates its on-screen size as the
// bounding radius over the distance to the camera (capped at 1), and distance
// is from the camera to the bounds' surface (zero inside them).
StreamPriority textureStreamPriority(float distanceToCenter, float boundsRadius);
// Keeps the most demanding of two uses of one texture.
StreamPriority mostDemandingStreamPriority(const StreamPriority& left, const StreamPriority& right);

struct TextureMipStreamCandidate {
    uint64_t key{0};
    TextureMipResidency residency;
    // Bytes of every level, level 0 first; at least residency.mipCount entries.
    std::span<const uint64_t> levelBytes;
    StreamPriority priority;
};

// Levels [firstMip, firstMip + levelCount) of one texture, which end where the
// texture's resident levels begin.
struct TextureMipUpload {
    uint64_t key{0};
    uint32_t firstMip{0};
    uint32_t levelCount{0};
    uint64_t bytes{0};
};

// Chooses this frame's mip uploads within `budgetBytes`. Candidates gain one
// level per round in priority order (gameplay importance, then projected
// visibility, then nearness, then key), so every visible texture sharpens a
// step before any texture spends the budget on its largest level. A level that
// does not fit is skipped for the frame, except that the first level of a frame
// is always taken so a level larger than the budget still streams. Complete
// candidates are ignored. Uploads are returned in priority order, at most one
// per candidate.
std::vector<TextureMipUpload> planTextureMipUploads(std::span<const TextureMipStreamCandidate> candidates,
                                                    uint64_t budgetBytes);
