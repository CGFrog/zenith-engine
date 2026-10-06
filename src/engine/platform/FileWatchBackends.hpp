// Internal to the file watcher: what FileWatcher.cpp and its backends share.
// Not part of the public engine API.
#pragma once

// Internal to the file watcher: what FileWatcher.cpp and its backends share.

#include "engine/platform/FileWatcher.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace file_watch_detail {
namespace fs = std::filesystem;

std::string toGenericUtf8(const fs::path& path);
fs::path pathFromUtf8(std::string_view text);
std::string joinRelative(const std::string& directory, std::string_view name);
bool isSameOrDescendant(std::string_view path, std::string_view directory);
bool isDescendant(std::string_view path, std::string_view directory);
// Replaces the `from` prefix of a path known to be `from` or below it with `to`.
std::string rebase(const std::string& path, const std::string& from, const std::string& to);
#if defined(_WIN32) || defined(__linux__)
std::string systemErrorMessage(int code);
#endif

// Receives raw changes from a backend thread, filters ignored paths, and feeds the
// coalescer. All members other than the ignore configuration are mutex-guarded.
class ChangeSink {
  public:
    explicit ChangeSink(const FileWatcherOptions& options)
        : m_ignoredSuffixes(options.ignoredSuffixes)
        , m_ignore(options.ignore)
        , m_coalescer(options.debounce, options.maxDelay) {}

    bool isIgnored(std::string_view relativePath) const {
        if (relativePath.empty()) {
            return false;
        }
        for (std::size_t end = relativePath.find('/'); end != std::string_view::npos;
             end = relativePath.find('/', end + 1)) {
            if (matchesIgnoreRule(relativePath.substr(0, end))) {
                return true;
            }
        }
        return matchesIgnoreRule(relativePath);
    }

    void push(FileWatchEvent event) {
        if (event.kind == FileWatchEventKind::Renamed) {
            const bool sourceIgnored = isIgnored(event.oldPath);
            const bool targetIgnored = isIgnored(event.path);
            if (sourceIgnored && targetIgnored) {
                return;
            }
            if (sourceIgnored) {
                event.kind = FileWatchEventKind::Added;
                event.oldPath.clear();
            } else if (targetIgnored) {
                event.kind = FileWatchEventKind::Removed;
                event.path = std::move(event.oldPath);
                event.oldPath.clear();
            }
        } else if (event.kind != FileWatchEventKind::RescanRequired && isIgnored(event.path)) {
            return;
        }
        std::scoped_lock lock(m_mutex);
        m_coalescer.add(std::move(event), FileWatchCoalescer::Clock::now());
    }

    void requestRescan() {
        push({.kind = FileWatchEventKind::RescanRequired});
    }

    void reportError(std::string error) {
        {
            std::scoped_lock lock(m_mutex);
            m_lastError = std::move(error);
        }
        requestRescan();
    }

    std::vector<FileWatchEvent> takeReady() {
        std::scoped_lock lock(m_mutex);
        return m_coalescer.takeReady(FileWatchCoalescer::Clock::now());
    }

    std::string lastError() const {
        std::scoped_lock lock(m_mutex);
        return m_lastError;
    }

  private:
    bool matchesIgnoreRule(std::string_view candidate) const {
        for (const std::string& suffix : m_ignoredSuffixes) {
            if (!suffix.empty() && candidate.ends_with(suffix)) {
                return true;
            }
        }
        return m_ignore && m_ignore(candidate);
    }

    const std::vector<std::string> m_ignoredSuffixes;
    const std::function<bool(std::string_view)> m_ignore;
    mutable std::mutex m_mutex;
    FileWatchCoalescer m_coalescer;
    std::string m_lastError;
};

using EntryVisitor =
    std::function<void(const std::string& relativePath, const fs::directory_entry& entry, bool isDirectory)>;

// Visits every non-ignored entry below `relativeDirectory`, parents before children.
// Symbolic links are reported but not followed. A subdirectory that vanishes during
// the walk is skipped; the walk is incomplete (returns false) only when a directory
// that still exists could not be listed.
bool forEachEntryBelow(const fs::path& root, const std::string& relativeDirectory, const ChangeSink& sink,
                       const EntryVisitor& visit);

class WatchBackend {
  public:
    virtual ~WatchBackend() = default;
    // Runs on the watcher thread until `stop` is requested or the backend fails.
    virtual void run(std::stop_token stop) = 0;
};

// The backends. Each runs on the watcher thread; the sink outlives it.
std::unique_ptr<WatchBackend> createPollingBackend(fs::path root, ChangeSink& sink, std::chrono::milliseconds interval);
#if defined(_WIN32)
std::unique_ptr<WatchBackend> createWindowsBackend(const fs::path& root, ChangeSink& sink, std::string& error);
#elif defined(__linux__)
std::unique_ptr<WatchBackend> createInotifyBackend(const fs::path& root, ChangeSink& sink, std::string& error);
#endif
} // namespace file_watch_detail
