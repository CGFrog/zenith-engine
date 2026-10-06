#pragma once

// Admits render objects into a fixed per-frame item capacity in build order.

#include <cstdint>

// Admits render objects into a fixed per-frame item capacity in build order.
// An object is admitted whole (all of its primitives) or not at all, and once
// one object does not fit every later object is dropped too, so the drawn set
// is always a prefix of the build order: deterministic for a given scene and
// stable while the scene does not change.
class RenderItemBudget {
  public:
    explicit RenderItemBudget(uint32_t capacity)
        : m_capacity(capacity) {}

    // Returns whether an object of itemCount items is admitted; a rejected
    // object's items count as dropped.
    bool admit(uint32_t itemCount) {
        if (m_full || itemCount > m_capacity - m_admitted) {
            m_full = true;
            m_dropped += itemCount;
            return false;
        }
        m_admitted += itemCount;
        return true;
    }

    uint32_t admitted() const {
        return m_admitted;
    }

    uint32_t dropped() const {
        return m_dropped;
    }

  private:
    uint32_t m_capacity{0};
    uint32_t m_admitted{0};
    uint32_t m_dropped{0};
    bool m_full{false};
};
