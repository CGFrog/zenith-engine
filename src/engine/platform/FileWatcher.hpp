#pragma once

// Watches a directory tree recursively and reports coalesced changes.

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

enum class FileWatchEventKind {
    Added,
    Modified,
    Removed,
    Renamed,
    // Changes were lost (OS buffer overflow, watch failure, root removed). Consumers
    // must rebuild their view of the tree with a full rescan.
    RescanRequired,
};

std::string_view fileWatchEventKindName(FileWatchEventKind kind);

// One coalesced change below the watched root.
//
// Paths are relative to the root, UTF-8, and use '/' separators. `path` is empty
// for RescanRequired. Modified is reported only for files. An Added directory is
// followed by Added events for the entries already inside it, so moving a populated
// tree into the root is reported completely. A Renamed directory is reported once;
// its descendants keep their names below the new path and are not reported. A
// Renamed or Added event may land on a path the consumer already knows when a file
// was replaced (for example an editor saving through a temporary file); treat that
// as a content change of the existing entry.
struct FileWatchEvent {
    FileWatchEventKind kind{FileWatchEventKind::Modified};
    std::string path;
    std::string oldPath; // Renamed only: the path before the rename.
    bool isDirectory{false};

    bool operator==(const FileWatchEvent&) const = default;
};

enum class FileWatchBackend {
    Native,  // ReadDirectoryChangesW on Windows, inotify on Linux.
    Polling, // Periodic snapshot diff by size and last write time.
};

enum class FileWatchBackendPreference {
    Automatic, // Native when available, otherwise Polling.
    Polling,
};

struct FileWatcherOptions {
    // A burst is delivered once no change has arrived for this long.
    std::chrono::milliseconds debounce{100};
    // A burst is delivered after this long even if changes keep arriving.
    std::chrono::milliseconds maxDelay{1000};
    // Snapshot interval for the polling backend.
    std::chrono::milliseconds pollingInterval{500};
    FileWatchBackendPreference backend{FileWatchBackendPreference::Automatic};
    // A path is ignored when it, or any of its ancestor directories, ends with one of
    // these suffixes (for example ".tmp") or matches `ignore`. Ignored directories are
    // not descended into.
    std::vector<std::string> ignoredSuffixes;
    // Receives a root-relative '/'-separated path. Called from the watcher thread, so
    // it must be thread-safe and must not call back into the watcher.
    std::function<bool(std::string_view relativePath)> ignore;
};

struct FileWatcherResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// Debounces and merges raw change notifications into a minimal ordered event list.
// Not thread-safe; FileWatcher guards it with a mutex. Exposed so that the merge
// rules can be tested deterministically:
//   Added + Modified -> Added           Added + Removed -> nothing
//   Modified + Removed -> Removed       Removed + Added (file) -> Modified
//   Added a + Renamed a->b -> Added b   Renamed a->b + Renamed b->c -> Renamed a->c
//   RescanRequired discards everything pending before it.
class FileWatchCoalescer {
  public:
    using Clock = std::chrono::steady_clock;

    FileWatchCoalescer(std::chrono::milliseconds quietPeriod, std::chrono::milliseconds maxDelay);

    void add(FileWatchEvent event, Clock::time_point now);
    // Returns the pending burst once it has been quiet for `quietPeriod` or has been
    // pending for `maxDelay`; otherwise returns nothing and keeps accumulating.
    std::vector<FileWatchEvent> takeReady(Clock::time_point now);

  private:
    struct Entry {
        FileWatchEvent event;
        bool modifiedAfterRename{false};
        bool live{true};
    };

    void mergeAdded(FileWatchEvent event);
    void mergeModified(FileWatchEvent event);
    void mergeRemoved(FileWatchEvent event);
    void mergeRenamed(FileWatchEvent event);
    void applyRemoval(std::size_t index);
    void moveDescendants(const std::string& from, const std::string& to);
    void append(FileWatchEvent event, bool modifiedAfterRename = false);
    void discard(std::size_t index);
    void setPath(std::size_t index, std::string path);
    std::optional<std::size_t> find(const std::string& path) const;

    std::chrono::milliseconds m_quietPeriod;
    std::chrono::milliseconds m_maxDelay;
    std::vector<Entry> m_entries;
    std::unordered_map<std::string, std::size_t> m_latestEntryByPath;
    bool m_rescanRequired{false};
    std::optional<Clock::time_point> m_firstChange;
    Clock::time_point m_lastChange{};
};

// Watches a directory tree recursively and reports coalesced changes.
//
// Threading: the watcher owns one background thread that receives OS notifications
// (or polls) and feeds a mutex-guarded coalescer. poll() may be called from any
// single consumer thread, typically once per frame; it never blocks on file I/O.
// Destruction stops and joins the background thread promptly. The watcher only
// observes the file system; it never modifies the watched tree.
class FileWatcher {
  public:
    // Fails with a descriptive error when `root` is missing or not a directory, or
    // the options are invalid. When the native backend cannot start (unsupported
    // platform, watch limit reached), the watcher falls back to polling and records
    // the reason in fallbackReason().
    static FileWatcherResult create(const std::filesystem::path& root, FileWatcherOptions options,
                                    std::unique_ptr<FileWatcher>& watcher);

    ~FileWatcher();
    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    // Returns the bursts that have settled since the previous call, in order.
    std::vector<FileWatchEvent> poll();

    const std::filesystem::path& root() const;
    FileWatchBackend backend() const;
    // Why the native backend was not used; empty when it is active or polling was requested.
    const std::string& fallbackReason() const;
    // The most recent runtime failure (for example the root being deleted); empty if none.
    // A failure is always accompanied by a RescanRequired event.
    std::string lastError() const;

  private:
    struct Impl;

    explicit FileWatcher(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;
};
