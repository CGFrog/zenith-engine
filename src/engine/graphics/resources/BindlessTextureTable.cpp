#include "BindlessTextureTable.hpp"

#include <algorithm>
#include <cassert>

BindlessTextureTable::BindlessTextureTable(uint32_t capacity, uint32_t frameCount, VkImageView fallback)
    : m_views(capacity, fallback)
    , m_fallback(fallback) {
    assert(frameCount > 0 && frameCount <= 32);
    m_allFramesMask = frameCount == 32 ? ~0u : (1u << frameCount) - 1u;
    // Every set starts empty, so every slot must be written once per frame.
    m_dirtyFrames.assign(capacity, m_allFramesMask);
    m_freeSlots.reserve(capacity);
    // Hand out low slots first so live content stays compact.
    for (uint32_t slot = capacity; slot > 0; --slot) {
        m_freeSlots.push_back(slot - 1);
    }
}

std::optional<uint32_t> BindlessTextureTable::acquire(VkImageView view) {
    if (m_freeSlots.empty() || view == VK_NULL_HANDLE) {
        return std::nullopt;
    }
    const uint32_t slot = m_freeSlots.back();
    m_freeSlots.pop_back();
    m_views[slot] = view;
    markDirty(slot);
    ++m_liveCount;
    return slot;
}

void BindlessTextureTable::release(uint32_t slot) {
    if (slot >= m_views.size() || m_views[slot] == m_fallback) {
        return;
    }
    m_views[slot] = m_fallback;
    markDirty(slot);
    m_freeSlots.push_back(slot);
    --m_liveCount;
}

std::vector<uint32_t> BindlessTextureTable::takePendingWrites(uint32_t frame) {
    std::vector<uint32_t> pending;
    const uint32_t frameBit = 1u << frame;
    if ((m_allFramesMask & frameBit) == 0) {
        return pending;
    }
    for (uint32_t slot = 0; slot < m_dirtyFrames.size(); ++slot) {
        if ((m_dirtyFrames[slot] & frameBit) != 0) {
            m_dirtyFrames[slot] &= ~frameBit;
            pending.push_back(slot);
        }
    }
    return pending;
}

VkImageView BindlessTextureTable::view(uint32_t slot) const {
    return slot < m_views.size() ? m_views[slot] : m_fallback;
}

void BindlessTextureTable::markDirty(uint32_t slot) {
    m_dirtyFrames[slot] = m_allFramesMask;
}
