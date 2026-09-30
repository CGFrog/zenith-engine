#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

// Retires native resources only after the frame/voice serial that last used
// them has completed. Collection never waits for a device, queue, or callback.
class DeferredReleaseQueue {
  public:
    using Release = std::function<void()>;

    void defer(uint64_t completionSerial, Release release) {
        if (release) {
            m_entries.push_back({completionSerial, m_nextSequence++, std::move(release)});
        }
    }

    size_t collect(uint64_t completedSerial) {
        std::ranges::stable_sort(m_entries, [](const Entry& left, const Entry& right) {
            return left.completionSerial != right.completionSerial
                       ? left.completionSerial < right.completionSerial
                       : left.sequence < right.sequence;
        });
        size_t released = 0;
        while (released < m_entries.size() && m_entries[released].completionSerial <= completedSerial) {
            m_entries[released].release();
            ++released;
        }
        m_entries.erase(m_entries.begin(), m_entries.begin() + static_cast<std::ptrdiff_t>(released));
        return released;
    }

    size_t pending() const { return m_entries.size(); }

  private:
    struct Entry {
        uint64_t completionSerial{0};
        uint64_t sequence{0};
        Release release;
    };
    std::vector<Entry> m_entries;
    uint64_t m_nextSequence{0};
};
