#include "Logger.hpp"

#include <algorithm>
#include <cstdio>

std::string_view logLevelName(LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
        return "Debug";
    case LogLevel::Info:
        return "Info";
    case LogLevel::Warning:
        return "Warning";
    case LogLevel::Error:
        return "Error";
    }
    return "Unknown";
}

void writeLogToStderr(std::string_view message) {
    std::string line;
    line.reserve(message.size() + 1);
    line.append(message);
    line.push_back('\n');
    // One write keeps lines from concurrent threads whole.
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
}

Logger::Logger(LoggerConfig config)
    : m_config([&config] {
        config.capacity = std::max<size_t>(config.capacity, 1);
        return config;
    }()) {}

void Logger::log(LogLevel level, std::string_view category, std::string_view message) {
    if (level < m_config.minimumLevel) {
        return;
    }
    LogEntry entry{.level = level,
                   .category = std::string(category),
                   .message = std::string(message),
                   .wallTime = std::chrono::system_clock::now(),
                   .steadyTime = std::chrono::steady_clock::now()};
    {
        const std::scoped_lock lock(m_entriesMutex);
        entry.sequence = ++m_latestSequence;
        if (m_entries.size() == m_config.capacity) {
            m_entries.pop_front();
            ++m_dropped;
        }
        m_entries.push_back(entry);
    }
    if (m_config.echoToStderr) {
        writeLogToStderr(message);
    }
    dispatch(entry);
}

void Logger::dispatch(const LogEntry& entry) {
    const std::scoped_lock lock(m_sinkMutex);
    if (m_sinks.empty()) {
        return;
    }
    // A copy, so a sink that subscribes or unsubscribes does not invalidate
    // the iteration; shared ownership keeps an unsubscribed sink alive until
    // its running call returns.
    const std::vector<Subscription> sinks = m_sinks;
    for (const Subscription& subscription : sinks) {
        (*subscription.sink)(entry);
    }
}

std::vector<LogEntry> Logger::entries() const {
    const std::scoped_lock lock(m_entriesMutex);
    return {m_entries.begin(), m_entries.end()};
}

std::vector<LogEntry> Logger::entriesAfter(uint64_t sequence) const {
    return snapshotAfter(sequence).entries;
}

LogSnapshot Logger::snapshotAfter(uint64_t sequence) const {
    const std::scoped_lock lock(m_entriesMutex);
    return {.entries = heldEntriesAfter(sequence), .latestSequence = m_latestSequence};
}

std::vector<LogEntry> Logger::heldEntriesAfter(uint64_t sequence) const {
    // Sequences are contiguous and ascending, so the first newer entry is
    // found by offset rather than a search.
    if (m_entries.empty() || sequence >= m_entries.back().sequence) {
        return {};
    }
    const uint64_t first = m_entries.front().sequence;
    const size_t skip = sequence < first ? 0 : static_cast<size_t>(sequence - first + 1);
    return {m_entries.begin() + static_cast<std::ptrdiff_t>(skip), m_entries.end()};
}

uint64_t Logger::latestSequence() const {
    const std::scoped_lock lock(m_entriesMutex);
    return m_latestSequence;
}

uint64_t Logger::droppedCount() const {
    const std::scoped_lock lock(m_entriesMutex);
    return m_dropped;
}

void Logger::clear() {
    const std::scoped_lock lock(m_entriesMutex);
    m_entries.clear();
}

Logger::SinkId Logger::subscribe(LogSink sink) {
    const std::scoped_lock lock(m_sinkMutex);
    const SinkId id = m_nextSinkId++;
    m_sinks.push_back({id, std::make_shared<LogSink>(std::move(sink))});
    return id;
}

void Logger::unsubscribe(SinkId id) {
    const std::scoped_lock lock(m_sinkMutex);
    std::erase_if(m_sinks, [id](const Subscription& subscription) { return subscription.id == id; });
}

LogChannel::LogChannel(Logger* logger, std::string category)
    : m_logger(logger)
    , m_category(std::move(category)) {}

void LogChannel::log(LogLevel level, std::string_view message) const {
    if (m_logger != nullptr) {
        m_logger->log(level, m_category, message);
        return;
    }
    writeLogToStderr(message);
}

void LogChannel::debug(std::string_view message) const {
    log(LogLevel::Debug, message);
}

void LogChannel::info(std::string_view message) const {
    log(LogLevel::Info, message);
}

void LogChannel::warning(std::string_view message) const {
    log(LogLevel::Warning, message);
}

void LogChannel::error(std::string_view message) const {
    log(LogLevel::Error, message);
}
