#pragma once
#include <volk.h>

#include <cstdint>
#include <optional>
#include <vector>

// Assigns stable array slots to sampled image views for a bindless descriptor
// array and tracks which slots each per-frame descriptor set still has to
// rewrite. Every frame in flight owns its own set, so a slot is only written
// into a set while that set's previous submission has retired; no update-after-
// bind semantics are required.
class BindlessTextureTable {
  public:
    // `fallback` fills every unassigned slot so partially-bound arrays never
    // expose a stale view to a shader indexing past live content.
    BindlessTextureTable(uint32_t capacity, uint32_t frameCount, VkImageView fallback);

    std::optional<uint32_t> acquire(VkImageView view);
    // Points the slot back at the fallback view and makes it reusable.
    void release(uint32_t slot);

    // Slots whose current view must be written into `frame`'s descriptor set.
    // Calling this marks them as written for that frame.
    std::vector<uint32_t> takePendingWrites(uint32_t frame);
    VkImageView view(uint32_t slot) const;

    uint32_t capacity() const {
        return static_cast<uint32_t>(m_views.size());
    }

    uint32_t liveCount() const {
        return m_liveCount;
    }

  private:
    void markDirty(uint32_t slot);

    std::vector<VkImageView> m_views;
    // One bit per frame in flight; a set bit means that frame's set is stale.
    std::vector<uint32_t> m_dirtyFrames;
    std::vector<uint32_t> m_freeSlots;
    VkImageView m_fallback{VK_NULL_HANDLE};
    uint32_t m_allFramesMask{0};
    uint32_t m_liveCount{0};
};
