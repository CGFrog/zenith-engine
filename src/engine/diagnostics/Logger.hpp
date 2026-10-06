#pragma once

// The engine Logger and LogChannel: levels, categories, snapshots.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

enum class LogLevel : uint8_t {
    Debug,
    Info,
    Warning,
    Error
};

std::string_view logLevelName(LogLevel level);

// One message. sequence increases by one per accepted message for the
// logger's lifetime (clear() does not reset it), so a reader can poll for
// entries newer than the last one it saw.
struct LogEntry {
    uint64_t sequence{0};
    LogLevel level{LogLevel::Info};
    std::string category;
    std::string message;
    // Wall-clock time for display; steadyTime orders and measures entries.
    std::chrono::system_clock::time_point wallTime;
    std::chrono::steady_clock::time_point steadyTime;
};

struct LoggerConfig {
    // Entries kept for readers; the oldest are dropped first.
    size_t capacity{4096};
    // Messages below this level are discarded entirely.
    LogLevel minimumLevel{LogLevel::Info};
    // Writes each accepted message to stderr unchanged, so command-line runs
    // and tools that read stderr see the same text as before the logger.
    bool echoToStderr{true};
};

using LogSink = std::function<void(const LogEntry&)>;

// Entries newer than a reader's last sequence, read together with the newest
// sequence under one lock, so a reader can advance past entries that were
// dropped or cleared without skipping ones logged concurrently.
struct LogSnapshot {
    std::vector<LogEntry> entries;
    uint64_t latestSequence{0};
};

// Engine-owned log of recent messages with optional stderr echo and sink
// subscriptions. There is no process-wide instance: owners pass a Logger (or a
// LogChannel) to what logs, and the logger must outlive them.
//
// Thread safety: every member may be called from any thread. Sinks run on the
// logging thread, one at a time, after the entry is stored; a sink may log
// (the nested message reaches every sink before the outer call returns) or
// unsubscribe, but must not block
// on another thread that is logging. Once unsubscribe returns, the sink is
// not running and will not be called again.
class Logger {
  public:
    using SinkId = uint64_t;

    explicit Logger(LoggerConfig config = {});
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void log(LogLevel level, std::string_view category, std::string_view message);

    // Entries currently held, oldest first.
    std::vector<LogEntry> entries() const;
    // Entries held whose sequence is greater than sequence, oldest first.
    std::vector<LogEntry> entriesAfter(uint64_t sequence) const;
    LogSnapshot snapshotAfter(uint64_t sequence) const;
    // The sequence of the newest accepted entry, or 0 before the first.
    uint64_t latestSequence() const;
    // Entries dropped from the ring because it was full.
    uint64_t droppedCount() const;
    // Drops the held entries; sequences continue from where they were.
    void clear();

    SinkId subscribe(LogSink sink);
    void unsubscribe(SinkId id);

    const LoggerConfig& config() const {
        return m_config;
    }

  private:
    struct Subscription {
        SinkId id;
        std::shared_ptr<LogSink> sink;
    };

    void dispatch(const LogEntry& entry);
    // Requires m_entriesMutex.
    std::vector<LogEntry> heldEntriesAfter(uint64_t sequence) const;

    const LoggerConfig m_config;
    mutable std::mutex m_entriesMutex;
    std::deque<LogEntry> m_entries;
    uint64_t m_latestSequence{0};
    uint64_t m_dropped{0};

    // Held while sinks run and while the subscription list changes, so
    // unsubscribe waits for a running sink. Recursive so a sink may log.
    std::recursive_mutex m_sinkMutex;
    std::vector<Subscription> m_sinks;
    SinkId m_nextSinkId{1};
};

// A category bound to an optional logger: what subsystems hold and pass down.
// Without a logger, messages go to stderr unchanged, so standalone tools and
// tests that build a subsystem directly keep their output.
class LogChannel {
  public:
    LogChannel() = default;
    LogChannel(Logger* logger, std::string category);

    void log(LogLevel level, std::string_view message) const;
    void debug(std::string_view message) const;
    void info(std::string_view message) const;
    void warning(std::string_view message) const;
    void error(std::string_view message) const;

    Logger* logger() const {
        return m_logger;
    }

    const std::string& category() const {
        return m_category;
    }

    // The same logger under another category.
    LogChannel withCategory(std::string category) const {
        return {m_logger, std::move(category)};
    }

  private:
    Logger* m_logger{nullptr};
    std::string m_category;
};

// Writes message and a newline to stderr as one write. The fallback for an
// absent logger, and the logger's own echo.
void writeLogToStderr(std::string_view message);
