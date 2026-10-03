#pragma once

#include "editor/operators/Operator.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

// One operator invocation as the runner finished it.
struct OperatorHistoryEntry {
    // Increases by one per recorded invocation; clear() does not reset it.
    uint64_t sequence{0};
    std::string id;
    // Empty when the id named no registered operator.
    std::string label;
    OperatorStatus status{OperatorStatus::Failed};
    std::string message;
    // The properties the operator ran with, as compact JSON ("{}" for none);
    // empty when the invocation never resolved its properties.
    std::string propertiesJson;
    std::chrono::system_clock::time_point wallTime;
    std::chrono::steady_clock::time_point steadyTime;
};

// A bounded record of operator invocations, oldest first. Single-threaded,
// like the runner that writes it.
class OperatorHistory {
  public:
    explicit OperatorHistory(size_t capacity = 512);

    void record(OperatorHistoryEntry entry);

    const std::deque<OperatorHistoryEntry>& entries() const {
        return m_entries;
    }

    // The newest recorded sequence, or 0 before the first invocation.
    uint64_t latestSequence() const {
        return m_latestSequence;
    }

    void clear();

  private:
    size_t m_capacity;
    std::deque<OperatorHistoryEntry> m_entries;
    uint64_t m_latestSequence{0};
};
