#include "TextureMipStreaming.hpp"

#include <algorithm>
#include <numeric>

uint32_t textureMipTailFirstLevel(uint32_t width, uint32_t height, uint32_t mipCount, uint32_t tailMaximumExtent) {
    if (mipCount == 0) {
        return 0;
    }
    for (uint32_t level = 0; level < mipCount; ++level) {
        const uint32_t levelWidth = std::max(width >> std::min(level, 31u), 1u);
        const uint32_t levelHeight = std::max(height >> std::min(level, 31u), 1u);
        if (levelWidth <= tailMaximumExtent && levelHeight <= tailMaximumExtent) {
            return level;
        }
    }
    return mipCount - 1;
}

StreamPriority textureStreamPriority(float distanceToCenter, float boundsRadius) {
    const float radius = std::max(boundsRadius, 0.0f);
    const float center = std::max(distanceToCenter, 0.0f);
    const float surface = std::max(center - radius, 0.0f);
    const float visibility = center <= radius ? 1.0f : std::min(radius / center, 1.0f);
    return {.projectedVisibility = visibility, .distance = surface};
}

StreamPriority mostDemandingStreamPriority(const StreamPriority& left, const StreamPriority& right) {
    return {.projectedVisibility = std::max(left.projectedVisibility, right.projectedVisibility),
            .distance = std::min(left.distance, right.distance),
            .gameplayImportance = std::max(left.gameplayImportance, right.gameplayImportance),
            .deadlineFrame = std::min(left.deadlineFrame, right.deadlineFrame)};
}

std::vector<TextureMipUpload> planTextureMipUploads(std::span<const TextureMipStreamCandidate> candidates,
                                                    uint64_t budgetBytes) {
    std::vector<uint32_t> order;
    for (uint32_t index = 0; index < candidates.size(); ++index) {
        const TextureMipResidency& residency = candidates[index].residency;
        if (!residency.complete() && residency.residentFirstMip <= residency.mipCount &&
            candidates[index].levelBytes.size() >= residency.mipCount) {
            order.push_back(index);
        }
    }
    std::ranges::sort(order, [&](uint32_t left, uint32_t right) {
        const StreamPriority& a = candidates[left].priority;
        const StreamPriority& b = candidates[right].priority;
        if (a.gameplayImportance != b.gameplayImportance) {
            return a.gameplayImportance > b.gameplayImportance;
        }
        if (a.projectedVisibility != b.projectedVisibility) {
            return a.projectedVisibility > b.projectedVisibility;
        }
        if (a.distance != b.distance) {
            return a.distance < b.distance;
        }
        return candidates[left].key < candidates[right].key;
    });

    // nextFirstMip[i]: the texture's first resident level once this frame's
    // scheduled uploads land.
    std::vector<uint32_t> nextFirstMip(order.size());
    for (size_t slot = 0; slot < order.size(); ++slot) {
        nextFirstMip[slot] = candidates[order[slot]].residency.residentFirstMip;
    }
    uint64_t remaining = budgetBytes;
    bool scheduledAny = false;
    for (bool progressed = true; progressed;) {
        progressed = false;
        for (size_t slot = 0; slot < order.size(); ++slot) {
            const TextureMipStreamCandidate& candidate = candidates[order[slot]];
            if (nextFirstMip[slot] <= candidate.residency.requestedFirstMip) {
                continue;
            }
            const uint64_t bytes = candidate.levelBytes[nextFirstMip[slot] - 1];
            if (bytes > remaining && scheduledAny) {
                continue;
            }
            remaining -= std::min(bytes, remaining);
            --nextFirstMip[slot];
            scheduledAny = true;
            progressed = true;
        }
    }

    std::vector<TextureMipUpload> uploads;
    for (size_t slot = 0; slot < order.size(); ++slot) {
        const TextureMipStreamCandidate& candidate = candidates[order[slot]];
        const uint32_t first = nextFirstMip[slot];
        const uint32_t end = candidate.residency.residentFirstMip;
        if (first == end) {
            continue;
        }
        const uint64_t bytes =
            std::accumulate(candidate.levelBytes.begin() + first, candidate.levelBytes.begin() + end, uint64_t{0});
        uploads.push_back({.key = candidate.key, .firstMip = first, .levelCount = end - first, .bytes = bytes});
    }
    return uploads;
}
