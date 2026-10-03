#include "OperatorHistory.hpp"

#include <algorithm>
#include <utility>

OperatorHistory::OperatorHistory(size_t capacity)
    : m_capacity(std::max<size_t>(capacity, 1)) {}

void OperatorHistory::record(OperatorHistoryEntry entry) {
    entry.sequence = ++m_latestSequence;
    if (entry.wallTime == std::chrono::system_clock::time_point{}) {
        entry.wallTime = std::chrono::system_clock::now();
    }
    if (entry.steadyTime == std::chrono::steady_clock::time_point{}) {
        entry.steadyTime = std::chrono::steady_clock::now();
    }
    if (m_entries.size() == m_capacity) {
        m_entries.pop_front();
    }
    m_entries.push_back(std::move(entry));
}

void OperatorHistory::clear() {
    m_entries.clear();
}
