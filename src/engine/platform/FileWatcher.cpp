#include "engine/platform/FileWatcher.hpp"

#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <stop_token>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <array>
#include <cerrno>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {
std::string toGenericUtf8(const fs::path& path) {
    const std::u8string text = path.generic_u8string();
    return {text.begin(), text.end()};
}

fs::path pathFromUtf8(std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

std::string joinRelative(const std::string& directory, std::string_view name) {
    if (directory.empty()) {
        return std::string(name);
    }
    std::string joined = directory;
    joined += '/';
    joined += name;
    return joined;
}

bool isSameOrDescendant(std::string_view path, std::string_view directory) {
    if (!path.starts_with(directory)) {
        return false;
    }
    return path.size() == directory.size() || path[directory.size()] == '/';
}

bool isDescendant(std::string_view path, std::string_view directory) {
    return path.size() > directory.size() && isSameOrDescendant(path, directory);
}

// Replaces the `from` prefix of a path known to be `from` or below it with `to`.
std::string rebase(const std::string& path, const std::string& from, const std::string& to) {
    return to + path.substr(from.size());
}

#if defined(_WIN32) || defined(__linux__)
std::string systemErrorMessage(int code) {
    return std::system_category().message(code);
}
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
                       const EntryVisitor& visit) {
    bool complete = true;
    std::vector<std::string> pendingDirectories{relativeDirectory};
    while (!pendingDirectories.empty()) {
        const std::string directory = std::move(pendingDirectories.back());
        pendingDirectories.pop_back();
        const fs::path directoryPath = directory.empty() ? root : root / pathFromUtf8(directory);
        std::error_code error;
        fs::directory_iterator iterator(directoryPath, fs::directory_options::skip_permission_denied, error);
        for (; !error && iterator != fs::directory_iterator(); iterator.increment(error)) {
            const fs::directory_entry& entry = *iterator;
            const std::string relativePath = joinRelative(directory, toGenericUtf8(entry.path().filename()));
            if (sink.isIgnored(relativePath)) {
                continue;
            }
            std::error_code typeError;
            const bool isDirectory = entry.is_directory(typeError);
            visit(relativePath, entry, isDirectory);
            if (isDirectory && !entry.is_symlink(typeError)) {
                pendingDirectories.push_back(relativePath);
            }
        }
        std::error_code existsError;
        if (error && fs::is_directory(directoryPath, existsError)) {
            complete = false;
        }
    }
    return complete;
}

class WatchBackend {
  public:
    virtual ~WatchBackend() = default;
    // Runs on the watcher thread until `stop` is requested or the backend fails.
    virtual void run(std::stop_token stop) = 0;
};

// Periodically snapshots the tree and reports the difference. Renames are inferred
// only when a snapshot contains exactly one removed and one added file with the same
// size and last write time; anything else is reported as removals and additions.
class PollingBackend final : public WatchBackend {
  public:
    PollingBackend(fs::path root, ChangeSink& sink, std::chrono::milliseconds interval)
        : m_root(std::move(root))
        , m_sink(sink)
        , m_interval(interval) {
        m_snapshot = capture();
    }

    void run(std::stop_token stop) override {
        std::mutex mutex;
        std::condition_variable_any wakeup;
        while (!stop.stop_requested()) {
            {
                std::unique_lock lock(mutex);
                wakeup.wait_for(lock, stop, m_interval, [] { return false; });
            }
            if (stop.stop_requested()) {
                return;
            }
            scan();
        }
    }

  private:
    struct EntryState {
        bool isDirectory{false};
        std::uintmax_t size{0};
        fs::file_time_type writeTime{};

        bool operator==(const EntryState&) const = default;
    };

    // Ordered so that parents precede their children.
    using Snapshot = std::map<std::string, EntryState>;

    void scan() {
        std::error_code error;
        if (!fs::is_directory(m_root, error)) {
            if (!m_rootLost) {
                m_rootLost = true;
                m_sink.reportError("watched root '" + toGenericUtf8(m_root) + "' is no longer accessible");
            }
            return;
        }
        std::optional<Snapshot> current = capture();
        if (!current) {
            return; // The tree changed mid-walk; the next interval retries.
        }
        if (m_rootLost || !m_snapshot) {
            // No trustworthy baseline to diff against: changes may have been missed.
            m_rootLost = false;
            m_sink.requestRescan();
        } else {
            report(*m_snapshot, *current);
        }
        m_snapshot = std::move(current);
    }

    std::optional<Snapshot> capture() const {
        Snapshot snapshot;
        const bool complete = forEachEntryBelow(
            m_root, {}, m_sink, [&](const std::string& path, const fs::directory_entry& entry, bool isDirectory) {
                EntryState state{.isDirectory = isDirectory};
                if (!isDirectory) {
                    std::error_code error;
                    state.size = entry.file_size(error);
                    state.writeTime = entry.last_write_time(error);
                }
                snapshot.emplace(path, state);
            });
        if (!complete) {
            return std::nullopt;
        }
        return snapshot;
    }

    void report(const Snapshot& previous, const Snapshot& current) {
        std::vector<std::string> removed;
        std::vector<std::string> added;
        std::vector<std::string> modified;
        for (const auto& [path, state] : previous) {
            const auto found = current.find(path);
            if (found == current.end() || found->second.isDirectory != state.isDirectory) {
                removed.push_back(path);
            }
        }
        for (const auto& [path, state] : current) {
            const auto found = previous.find(path);
            if (found == previous.end() || found->second.isDirectory != state.isDirectory) {
                added.push_back(path);
            } else if (!state.isDirectory && found->second != state) {
                modified.push_back(path);
            }
        }

        const auto renamed = findRename(previous, current, removed, added);
        for (auto path = removed.rbegin(); path != removed.rend(); ++path) {
            if (!renamed || *path != renamed->first) {
                m_sink.push({.kind = FileWatchEventKind::Removed,
                             .path = *path,
                             .isDirectory = previous.at(*path).isDirectory});
            }
        }
        for (const std::string& path : added) {
            if (renamed && path == renamed->second) {
                m_sink.push({.kind = FileWatchEventKind::Renamed, .path = path, .oldPath = renamed->first});
            } else {
                m_sink.push(
                    {.kind = FileWatchEventKind::Added, .path = path, .isDirectory = current.at(path).isDirectory});
            }
        }
        for (const std::string& path : modified) {
            m_sink.push({.kind = FileWatchEventKind::Modified, .path = path});
        }
    }

    static std::optional<std::pair<std::string, std::string>> findRename(const Snapshot& previous,
                                                                         const Snapshot& current,
                                                                         const std::vector<std::string>& removed,
                                                                         const std::vector<std::string>& added) {
        const std::string* source = nullptr;
        const std::string* target = nullptr;
        for (const std::string& path : removed) {
            if (!previous.at(path).isDirectory) {
                if (source) {
                    return std::nullopt;
                }
                source = &path;
            }
        }
        for (const std::string& path : added) {
            if (!current.at(path).isDirectory) {
                if (target) {
                    return std::nullopt;
                }
                target = &path;
            }
        }
        if (!source || !target || previous.at(*source) != current.at(*target)) {
            return std::nullopt;
        }
        return std::pair{*source, *target};
    }

    fs::path m_root;
    ChangeSink& m_sink;
    std::chrono::milliseconds m_interval;
    // Empty only when the initial walk failed; the first complete walk then becomes
    // the baseline and a rescan is requested.
    std::optional<Snapshot> m_snapshot;
    bool m_rootLost{false};
};

#ifdef _WIN32
// ReadDirectoryChangesW over the whole subtree. Windows does not say whether a
// removed or renamed entry was a directory, so the backend tracks known directories.
class WindowsBackend final : public WatchBackend {
  public:
    static std::unique_ptr<WatchBackend> create(const fs::path& root, ChangeSink& sink, std::string& error) {
        std::unique_ptr<WindowsBackend> backend(new WindowsBackend(root, sink));
        backend->m_directory =
            CreateFileW(root.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (backend->m_directory == INVALID_HANDLE_VALUE) {
            error = "could not open '" + toGenericUtf8(root) +
                    "' for change notifications: " + systemErrorMessage(static_cast<int>(GetLastError()));
            return nullptr;
        }
        backend->m_readEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        backend->m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!backend->m_readEvent || !backend->m_stopEvent) {
            error = "could not create file watcher events: " + systemErrorMessage(static_cast<int>(GetLastError()));
            return nullptr;
        }
        backend->m_overlapped.hEvent = backend->m_readEvent;
        backend->rebuildDirectories();
        if (!backend->beginRead()) {
            error = "ReadDirectoryChangesW failed for '" + toGenericUtf8(root) +
                    "': " + systemErrorMessage(static_cast<int>(GetLastError()));
            return nullptr;
        }
        return backend;
    }

    ~WindowsBackend() override {
        if (m_readPending) {
            CancelIoEx(m_directory, &m_overlapped);
            DWORD byteCount = 0;
            GetOverlappedResult(m_directory, &m_overlapped, &byteCount, TRUE);
        }
        if (m_directory != INVALID_HANDLE_VALUE) {
            CloseHandle(m_directory);
        }
        if (m_readEvent) {
            CloseHandle(m_readEvent);
        }
        if (m_stopEvent) {
            CloseHandle(m_stopEvent);
        }
    }

    WindowsBackend(const WindowsBackend&) = delete;
    WindowsBackend& operator=(const WindowsBackend&) = delete;

    void run(std::stop_token stop) override {
        std::stop_callback wake(stop, [this] { SetEvent(m_stopEvent); });
        while (!stop.stop_requested() && m_readPending) {
            const HANDLE handles[] = {m_readEvent, m_stopEvent};
            if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) != WAIT_OBJECT_0) {
                return;
            }
            DWORD byteCount = 0;
            const BOOL completed = GetOverlappedResult(m_directory, &m_overlapped, &byteCount, FALSE);
            m_readPending = false;
            if (!completed) {
                const DWORD code = GetLastError();
                if (code != ERROR_NOTIFY_ENUM_DIR) {
                    m_sink.reportError("change notifications for '" + toGenericUtf8(m_root) +
                                       "' failed: " + systemErrorMessage(static_cast<int>(code)));
                    return;
                }
                handleOverflow();
            } else if (byteCount == 0) {
                handleOverflow();
            } else {
                dispatch();
            }
            if (!beginRead()) {
                m_sink.reportError("change notifications for '" + toGenericUtf8(m_root) +
                                   "' stopped: " + systemErrorMessage(static_cast<int>(GetLastError())));
                return;
            }
        }
    }

  private:
    WindowsBackend(fs::path root, ChangeSink& sink)
        : m_root(std::move(root))
        , m_sink(sink) {}

    bool beginRead() {
        ResetEvent(m_readEvent);
        constexpr DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                 FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;
        const BOOL started =
            ReadDirectoryChangesW(m_directory, m_buffer.data(), static_cast<DWORD>(m_buffer.size() * sizeof(DWORD)),
                                  TRUE, filter, nullptr, &m_overlapped, nullptr);
        m_readPending = started != FALSE;
        return m_readPending;
    }

    void dispatch() {
        const auto* bytes = reinterpret_cast<const std::byte*>(m_buffer.data());
        std::size_t offset = 0;
        while (true) {
            const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(bytes + offset);
            const std::wstring_view name(info->FileName, info->FileNameLength / sizeof(WCHAR));
            handle(info->Action, toGenericUtf8(fs::path(name)));
            if (info->NextEntryOffset == 0) {
                return;
            }
            offset += info->NextEntryOffset;
        }
    }

    void handle(DWORD action, std::string path) {
        if (action != FILE_ACTION_RENAMED_NEW_NAME && m_pendingRenameSource) {
            reportRemoved(*std::exchange(m_pendingRenameSource, std::nullopt));
        }
        switch (action) {
        case FILE_ACTION_ADDED:
            reportAdded(path);
            break;
        case FILE_ACTION_REMOVED:
            reportRemoved(path);
            break;
        case FILE_ACTION_MODIFIED:
            if (!m_directories.contains(path)) {
                m_sink.push({.kind = FileWatchEventKind::Modified, .path = std::move(path)});
            }
            break;
        case FILE_ACTION_RENAMED_OLD_NAME:
            m_pendingRenameSource = std::move(path);
            break;
        case FILE_ACTION_RENAMED_NEW_NAME:
            if (!m_pendingRenameSource) {
                reportAdded(path);
            } else {
                reportRenamed(*std::exchange(m_pendingRenameSource, std::nullopt), path);
            }
            break;
        default:
            break;
        }
    }

    void reportAdded(const std::string& path) {
        if (m_sink.isIgnored(path)) {
            return;
        }
        const bool isDirectory = isDirectoryOnDisk(path);
        m_sink.push({.kind = FileWatchEventKind::Added, .path = path, .isDirectory = isDirectory});
        if (isDirectory) {
            m_directories.insert(path);
            reportContents(path);
        }
    }

    void reportRemoved(const std::string& path) {
        const bool isDirectory = m_directories.contains(path);
        m_sink.push({.kind = FileWatchEventKind::Removed, .path = path, .isDirectory = isDirectory});
        if (isDirectory) {
            std::erase_if(m_directories, [&](const std::string& known) { return isSameOrDescendant(known, path); });
        }
    }

    void reportRenamed(const std::string& from, const std::string& to) {
        const bool isDirectory = m_directories.contains(from) || isDirectoryOnDisk(to);
        m_sink.push({.kind = FileWatchEventKind::Renamed, .path = to, .oldPath = from, .isDirectory = isDirectory});
        if (!isDirectory) {
            return;
        }
        if (m_sink.isIgnored(from)) {
            // Contents of an ignored directory were never tracked or reported.
            m_directories.insert(to);
            reportContents(to);
            return;
        }
        std::vector<std::string> moved;
        std::erase_if(m_directories, [&](const std::string& known) {
            if (!isSameOrDescendant(known, from)) {
                return false;
            }
            moved.push_back(rebase(known, from, to));
            return true;
        });
        m_directories.insert(moved.begin(), moved.end());
        m_directories.insert(to);
    }

    void reportContents(const std::string& directory) {
        forEachEntryBelow(
            m_root, directory, m_sink, [&](const std::string& path, const fs::directory_entry&, bool isDirectory) {
                if (isDirectory) {
                    m_directories.insert(path);
                }
                m_sink.push({.kind = FileWatchEventKind::Added, .path = path, .isDirectory = isDirectory});
            });
    }

    void handleOverflow() {
        m_pendingRenameSource.reset();
        rebuildDirectories();
        m_sink.requestRescan();
    }

    void rebuildDirectories() {
        m_directories.clear();
        forEachEntryBelow(m_root, {}, m_sink,
                          [&](const std::string& path, const fs::directory_entry&, bool isDirectory) {
                              if (isDirectory) {
                                  m_directories.insert(path);
                              }
                          });
    }

    bool isDirectoryOnDisk(const std::string& path) const {
        std::error_code error;
        return fs::is_directory(m_root / pathFromUtf8(path), error);
    }

    fs::path m_root;
    ChangeSink& m_sink;
    HANDLE m_directory{INVALID_HANDLE_VALUE};
    HANDLE m_readEvent{nullptr};
    HANDLE m_stopEvent{nullptr};
    OVERLAPPED m_overlapped{};
    bool m_readPending{false};
    // DWORD elements keep the notification buffer DWORD-aligned as the API requires.
    std::vector<DWORD> m_buffer = std::vector<DWORD>(16 * 1024);
    std::unordered_set<std::string> m_directories;
    std::optional<std::string> m_pendingRenameSource;
};
#elif defined(__linux__)
// inotify watches one directory at a time, so the backend registers every directory
// and adds watches for directories as they appear. An eventfd wakes the thread for
// shutdown. IN_MOVED_FROM/IN_MOVED_TO pairs are matched by cookie; an unmatched
// source becomes a removal shortly afterwards (it left the watched tree).
class InotifyBackend final : public WatchBackend {
  public:
    static std::unique_ptr<WatchBackend> create(const fs::path& root, ChangeSink& sink, std::string& error) {
        std::unique_ptr<InotifyBackend> backend(new InotifyBackend(root, sink));
        backend->m_inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (backend->m_inotify < 0) {
            error = "inotify_init1 failed: " + systemErrorMessage(errno);
            return nullptr;
        }
        backend->m_wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (backend->m_wake < 0) {
            error = "eventfd failed: " + systemErrorMessage(errno);
            return nullptr;
        }
        if (!backend->watchTree({}, false, error)) {
            return nullptr;
        }
        return backend;
    }

    ~InotifyBackend() override {
        if (m_inotify >= 0) {
            close(m_inotify);
        }
        if (m_wake >= 0) {
            close(m_wake);
        }
    }

    InotifyBackend(const InotifyBackend&) = delete;
    InotifyBackend& operator=(const InotifyBackend&) = delete;

    void run(std::stop_token stop) override {
        std::stop_callback wake(stop, [this] {
            const std::uint64_t one = 1;
            [[maybe_unused]] const ssize_t written = write(m_wake, &one, sizeof(one));
        });
        while (!stop.stop_requested()) {
            pollfd descriptors[2] = {{m_inotify, POLLIN, 0}, {m_wake, POLLIN, 0}};
            const int timeout = m_pendingMoves.empty() ? -1 : static_cast<int>(pendingMoveTimeout.count());
            const int ready = ::poll(descriptors, 2, timeout);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                m_sink.reportError("poll on inotify failed: " + systemErrorMessage(errno));
                return;
            }
            if (ready > 0 && descriptors[1].revents != 0) {
                return;
            }
            if (ready > 0 && (descriptors[0].revents & POLLIN) != 0 && !readEvents()) {
                return;
            }
            flushExpiredMoves();
        }
    }

  private:
    struct PendingMove {
        std::string path;
        bool isDirectory{false};
        std::chrono::steady_clock::time_point since{};
    };

    // How long an IN_MOVED_FROM waits for its IN_MOVED_TO before it counts as a removal.
    static constexpr std::chrono::milliseconds pendingMoveTimeout{20};
    // Walk retries before giving up on registering a churning tree completely.
    static constexpr int watchTreeAttempts = 3;

    static constexpr std::uint32_t watchMask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_FROM |
                                               IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR |
                                               IN_DONT_FOLLOW | IN_EXCL_UNLINK;

    InotifyBackend(fs::path root, ChangeSink& sink)
        : m_root(std::move(root))
        , m_sink(sink) {}

    bool watchDirectory(const std::string& directory, std::string& error) {
        const fs::path path = directory.empty() ? m_root : m_root / pathFromUtf8(directory);
        const int descriptor = inotify_add_watch(m_inotify, path.c_str(), watchMask);
        if (descriptor < 0) {
            const int code = errno;
            if (code == ENOENT || code == ENOTDIR) {
                return true; // Vanished or replaced before it could be watched.
            }
            error = "inotify_add_watch failed for '" + toGenericUtf8(path) + "': " + systemErrorMessage(code);
            return false;
        }
        m_watchedDirectories[descriptor] = directory;
        return true;
    }

    bool watchTree(const std::string& directory, bool reportContents, std::string& error) {
        if (!watchDirectory(directory, error)) {
            return false;
        }
        // Re-walking is safe: watches are idempotent and repeated Added events coalesce.
        for (int attempt = 0; attempt < watchTreeAttempts; ++attempt) {
            bool watched = true;
            const bool complete = forEachEntryBelow(
                m_root, directory, m_sink, [&](const std::string& path, const fs::directory_entry&, bool isDirectory) {
                    if (isDirectory && watched) {
                        watched = watchDirectory(path, error);
                    }
                    if (reportContents) {
                        m_sink.push({.kind = FileWatchEventKind::Added, .path = path, .isDirectory = isDirectory});
                    }
                });
            if (!watched) {
                return false;
            }
            if (complete) {
                return true;
            }
        }
        error = "could not list every directory below '" + toGenericUtf8(m_root / pathFromUtf8(directory)) +
                "' while registering watches";
        return false;
    }

    void watchNewTree(const std::string& directory) {
        std::string error;
        if (!watchTree(directory, true, error)) {
            m_sink.reportError(error);
        }
    }

    bool readEvents() {
        while (true) {
            const ssize_t length = read(m_inotify, m_buffer.data(), m_buffer.size());
            if (length < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return true;
                }
                m_sink.reportError("reading inotify events failed: " + systemErrorMessage(errno));
                return false;
            }
            if (length == 0) {
                return true;
            }
            for (ssize_t offset = 0; offset < length;) {
                const auto* event = reinterpret_cast<const inotify_event*>(m_buffer.data() + offset);
                handle(*event);
                offset += static_cast<ssize_t>(sizeof(inotify_event) + event->len);
            }
        }
    }

    void handle(const inotify_event& event) {
        if ((event.mask & IN_Q_OVERFLOW) != 0) {
            resynchronize();
            return;
        }
        const auto watched = m_watchedDirectories.find(event.wd);
        if (watched == m_watchedDirectories.end()) {
            return;
        }
        if ((event.mask & IN_IGNORED) != 0) {
            m_watchedDirectories.erase(watched);
            return;
        }
        const std::string directory = watched->second;
        if ((event.mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
            // Subdirectories are reported through their parent; only the root matters here.
            if (directory.empty()) {
                m_sink.reportError("watched root '" + toGenericUtf8(m_root) + "' was deleted or moved");
            }
            return;
        }
        if (event.len == 0) {
            return;
        }
        const std::string path = joinRelative(directory, event.name);
        const bool isDirectory = (event.mask & IN_ISDIR) != 0;
        if ((event.mask & IN_CREATE) != 0) {
            reportAdded(path, isDirectory);
        } else if ((event.mask & IN_DELETE) != 0) {
            m_sink.push({.kind = FileWatchEventKind::Removed, .path = path, .isDirectory = isDirectory});
        } else if ((event.mask & (IN_MODIFY | IN_CLOSE_WRITE)) != 0) {
            if (!isDirectory) {
                m_sink.push({.kind = FileWatchEventKind::Modified, .path = path});
            }
        } else if ((event.mask & IN_MOVED_FROM) != 0) {
            m_pendingMoves[event.cookie] = {path, isDirectory, std::chrono::steady_clock::now()};
        } else if ((event.mask & IN_MOVED_TO) != 0) {
            const auto source = m_pendingMoves.find(event.cookie);
            if (source == m_pendingMoves.end()) {
                reportAdded(path, isDirectory);
            } else {
                const std::string from = std::move(source->second.path);
                m_pendingMoves.erase(source);
                reportRenamed(from, path, isDirectory);
            }
        }
    }

    void reportAdded(const std::string& path, bool isDirectory) {
        m_sink.push({.kind = FileWatchEventKind::Added, .path = path, .isDirectory = isDirectory});
        if (isDirectory && !m_sink.isIgnored(path)) {
            watchNewTree(path);
        }
    }

    void reportRenamed(const std::string& from, const std::string& to, bool isDirectory) {
        m_sink.push({.kind = FileWatchEventKind::Renamed, .path = to, .oldPath = from, .isDirectory = isDirectory});
        if (!isDirectory) {
            return;
        }
        if (m_sink.isIgnored(from)) {
            if (!m_sink.isIgnored(to)) {
                watchNewTree(to); // Ignored directories were never watched or reported.
            }
            return;
        }
        for (auto& [descriptor, directory] : m_watchedDirectories) {
            if (isSameOrDescendant(directory, from)) {
                directory = rebase(directory, from, to);
            }
        }
    }

    // Reports moves whose destination never arrived as removals: they left the tree.
    void flushExpiredMoves() {
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(m_pendingMoves, [&](const auto& entry) {
            const PendingMove& pending = entry.second;
            if (now - pending.since < pendingMoveTimeout) {
                return false;
            }
            m_sink.push(
                {.kind = FileWatchEventKind::Removed, .path = pending.path, .isDirectory = pending.isDirectory});
            if (pending.isDirectory) {
                unwatchTree(pending.path);
            }
            return true;
        });
    }

    void unwatchTree(const std::string& path) {
        std::erase_if(m_watchedDirectories, [&](const auto& watched) {
            if (!isSameOrDescendant(watched.second, path)) {
                return false;
            }
            inotify_rm_watch(m_inotify, watched.first);
            return true;
        });
    }

    void resynchronize() {
        m_pendingMoves.clear();
        std::string error;
        if (!watchTree({}, false, error)) {
            m_sink.reportError(error);
            return;
        }
        m_sink.requestRescan();
    }

    fs::path m_root;
    ChangeSink& m_sink;
    int m_inotify{-1};
    int m_wake{-1};
    std::unordered_map<int, std::string> m_watchedDirectories;
    std::unordered_map<std::uint32_t, PendingMove> m_pendingMoves;
    alignas(inotify_event) std::array<char, 64 * 1024> m_buffer{};
};
#endif

std::unique_ptr<WatchBackend> createNativeBackend(const fs::path& root, ChangeSink& sink, std::string& error) {
#if defined(_WIN32)
    return WindowsBackend::create(root, sink, error);
#elif defined(__linux__)
    return InotifyBackend::create(root, sink, error);
#else
    (void)root;
    (void)sink;
    error = "no native file watching backend is available on this platform";
    return nullptr;
#endif
}
} // namespace

std::string_view fileWatchEventKindName(FileWatchEventKind kind) {
    switch (kind) {
    case FileWatchEventKind::Added:
        return "Added";
    case FileWatchEventKind::Modified:
        return "Modified";
    case FileWatchEventKind::Removed:
        return "Removed";
    case FileWatchEventKind::Renamed:
        return "Renamed";
    case FileWatchEventKind::RescanRequired:
        return "RescanRequired";
    }
    return "Unknown";
}

FileWatchCoalescer::FileWatchCoalescer(std::chrono::milliseconds quietPeriod, std::chrono::milliseconds maxDelay)
    : m_quietPeriod(quietPeriod)
    , m_maxDelay(maxDelay) {}

void FileWatchCoalescer::add(FileWatchEvent event, Clock::time_point now) {
    if (!m_firstChange) {
        m_firstChange = now;
    }
    m_lastChange = now;
    switch (event.kind) {
    case FileWatchEventKind::Added:
        mergeAdded(std::move(event));
        break;
    case FileWatchEventKind::Modified:
        mergeModified(std::move(event));
        break;
    case FileWatchEventKind::Removed:
        mergeRemoved(std::move(event));
        break;
    case FileWatchEventKind::Renamed:
        mergeRenamed(std::move(event));
        break;
    case FileWatchEventKind::RescanRequired:
        m_entries.clear();
        m_latestEntryByPath.clear();
        m_rescanRequired = true;
        break;
    }
}

std::vector<FileWatchEvent> FileWatchCoalescer::takeReady(Clock::time_point now) {
    if (!m_firstChange) {
        return {};
    }
    if (now - m_lastChange < m_quietPeriod && now - *m_firstChange < m_maxDelay) {
        return {};
    }
    std::vector<FileWatchEvent> ready;
    if (m_rescanRequired) {
        ready.push_back({.kind = FileWatchEventKind::RescanRequired});
    }
    for (Entry& entry : m_entries) {
        if (!entry.live) {
            continue;
        }
        ready.push_back(std::move(entry.event));
        if (entry.modifiedAfterRename) {
            ready.push_back({.kind = FileWatchEventKind::Modified, .path = ready.back().path});
        }
    }
    m_entries.clear();
    m_latestEntryByPath.clear();
    m_rescanRequired = false;
    m_firstChange.reset();
    return ready;
}

void FileWatchCoalescer::mergeAdded(FileWatchEvent event) {
    const std::optional<std::size_t> existing = find(event.path);
    if (!existing || m_entries[*existing].event.isDirectory != event.isDirectory) {
        append(std::move(event));
        return;
    }
    Entry& entry = m_entries[*existing];
    switch (entry.event.kind) {
    case FileWatchEventKind::Removed:
        if (event.isDirectory) {
            // Keep both: the old directory's contents are gone, the new one is reported separately.
            append(std::move(event));
        } else {
            entry.event.kind = FileWatchEventKind::Modified;
        }
        break;
    case FileWatchEventKind::Renamed:
        entry.modifiedAfterRename = !entry.event.isDirectory;
        break;
    default:
        break; // Added or Modified already describe the entry.
    }
}

void FileWatchCoalescer::mergeModified(FileWatchEvent event) {
    const std::optional<std::size_t> existing = find(event.path);
    if (!existing || m_entries[*existing].event.isDirectory != event.isDirectory) {
        append(std::move(event));
        return;
    }
    Entry& entry = m_entries[*existing];
    switch (entry.event.kind) {
    case FileWatchEventKind::Removed:
        entry.event.kind = FileWatchEventKind::Modified; // Recreated without an Added notification.
        break;
    case FileWatchEventKind::Renamed:
        entry.modifiedAfterRename = true;
        break;
    default:
        break;
    }
}

void FileWatchCoalescer::mergeRemoved(FileWatchEvent event) {
    if (event.isDirectory) {
        for (std::size_t index = 0; index < m_entries.size(); ++index) {
            if (m_entries[index].live && isDescendant(m_entries[index].event.path, event.path)) {
                applyRemoval(index);
            }
        }
    }
    const std::optional<std::size_t> existing = find(event.path);
    if (!existing || m_entries[*existing].event.isDirectory != event.isDirectory) {
        append(std::move(event));
        return;
    }
    applyRemoval(*existing);
}

void FileWatchCoalescer::mergeRenamed(FileWatchEvent event) {
    std::string origin = event.oldPath;
    bool modified = false;
    if (const std::optional<std::size_t> source = find(event.oldPath)) {
        Entry& entry = m_entries[*source];
        switch (entry.event.kind) {
        case FileWatchEventKind::Added:
            discard(*source);
            mergeAdded({.kind = FileWatchEventKind::Added, .path = event.path, .isDirectory = event.isDirectory});
            if (event.isDirectory) {
                moveDescendants(event.oldPath, event.path);
            }
            return;
        case FileWatchEventKind::Renamed:
            origin = entry.event.oldPath;
            modified = entry.modifiedAfterRename;
            discard(*source);
            break;
        case FileWatchEventKind::Modified:
            modified = !event.isDirectory;
            discard(*source);
            break;
        default:
            break;
        }
    }
    if (const std::optional<std::size_t> target = find(event.path);
        target && m_entries[*target].event.kind == FileWatchEventKind::Removed) {
        discard(*target); // The rename replaced the removed entry.
    }
    if (origin != event.path) {
        append({.kind = FileWatchEventKind::Renamed,
                .path = event.path,
                .oldPath = std::move(origin),
                .isDirectory = event.isDirectory},
               modified);
    } else if (modified) {
        append({.kind = FileWatchEventKind::Modified, .path = event.path});
    }
    if (event.isDirectory) {
        moveDescendants(event.oldPath, event.path);
    }
}

void FileWatchCoalescer::applyRemoval(std::size_t index) {
    Entry& entry = m_entries[index];
    switch (entry.event.kind) {
    case FileWatchEventKind::Added:
        discard(index);
        break;
    case FileWatchEventKind::Modified:
        entry.event.kind = FileWatchEventKind::Removed;
        break;
    case FileWatchEventKind::Renamed: {
        std::string origin = std::move(entry.event.oldPath);
        entry.event.oldPath.clear();
        entry.event.kind = FileWatchEventKind::Removed;
        entry.modifiedAfterRename = false;
        setPath(index, std::move(origin));
        break;
    }
    default:
        break;
    }
}

// Re-keys pending changes below a renamed directory and moves them after the
// directory's own entry so that consumers see the parent before its contents.
void FileWatchCoalescer::moveDescendants(const std::string& from, const std::string& to) {
    const std::size_t existingCount = m_entries.size();
    for (std::size_t index = 0; index < existingCount; ++index) {
        const Entry& entry = m_entries[index];
        if (!entry.live || entry.event.kind == FileWatchEventKind::Removed || !isDescendant(entry.event.path, from)) {
            continue;
        }
        FileWatchEvent moved = entry.event;
        const bool modifiedAfterRename = entry.modifiedAfterRename;
        moved.path = rebase(moved.path, from, to);
        discard(index);
        append(std::move(moved), modifiedAfterRename);
    }
}

void FileWatchCoalescer::append(FileWatchEvent event, bool modifiedAfterRename) {
    m_latestEntryByPath[event.path] = m_entries.size();
    m_entries.push_back({.event = std::move(event), .modifiedAfterRename = modifiedAfterRename});
}

void FileWatchCoalescer::discard(std::size_t index) {
    Entry& entry = m_entries[index];
    entry.live = false;
    const auto latest = m_latestEntryByPath.find(entry.event.path);
    if (latest != m_latestEntryByPath.end() && latest->second == index) {
        m_latestEntryByPath.erase(latest);
    }
}

void FileWatchCoalescer::setPath(std::size_t index, std::string path) {
    Entry& entry = m_entries[index];
    const auto latest = m_latestEntryByPath.find(entry.event.path);
    if (latest != m_latestEntryByPath.end() && latest->second == index) {
        m_latestEntryByPath.erase(latest);
    }
    entry.event.path = std::move(path);
    m_latestEntryByPath[entry.event.path] = index;
}

std::optional<std::size_t> FileWatchCoalescer::find(const std::string& path) const {
    const auto latest = m_latestEntryByPath.find(path);
    if (latest == m_latestEntryByPath.end() || !m_entries[latest->second].live) {
        return std::nullopt;
    }
    return latest->second;
}

struct FileWatcher::Impl {
    Impl(fs::path watchedRoot, const FileWatcherOptions& options)
        : root(std::move(watchedRoot))
        , sink(options) {}

    fs::path root;
    FileWatchBackend backend{FileWatchBackend::Polling};
    std::string fallbackReason;
    ChangeSink sink;
    std::unique_ptr<WatchBackend> watchBackend;
    // Declared last so the thread stops and joins before the backend and sink it uses.
    std::jthread thread;
};

FileWatcherResult FileWatcher::create(const fs::path& root, FileWatcherOptions options,
                                      std::unique_ptr<FileWatcher>& watcher) {
    if (options.debounce.count() < 0 || options.maxDelay.count() < 0) {
        return {.error = "file watcher debounce and maximum delay must not be negative"};
    }
    if (options.pollingInterval.count() <= 0) {
        return {.error = "file watcher polling interval must be positive"};
    }
    std::error_code error;
    fs::path absoluteRoot = fs::absolute(root, error).lexically_normal();
    if (error || !fs::is_directory(absoluteRoot, error)) {
        return {.error = "file watcher root '" + toGenericUtf8(root) + "' is not an existing directory"};
    }
    if (!absoluteRoot.has_filename() && absoluteRoot.has_relative_path()) {
        absoluteRoot = absoluteRoot.parent_path(); // Drop a trailing separator.
    }

    auto impl = std::make_unique<Impl>(std::move(absoluteRoot), options);
    if (options.backend == FileWatchBackendPreference::Automatic) {
        std::string nativeError;
        impl->watchBackend = createNativeBackend(impl->root, impl->sink, nativeError);
        if (impl->watchBackend) {
            impl->backend = FileWatchBackend::Native;
        } else {
            impl->fallbackReason = std::move(nativeError);
        }
    }
    if (!impl->watchBackend) {
        impl->watchBackend = std::make_unique<PollingBackend>(impl->root, impl->sink, options.pollingInterval);
    }
    impl->thread =
        std::jthread([running = impl->watchBackend.get()](std::stop_token stop) { running->run(std::move(stop)); });
    watcher.reset(new FileWatcher(std::move(impl)));
    return {.success = true};
}

FileWatcher::FileWatcher(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl)) {}

FileWatcher::~FileWatcher() = default;

std::vector<FileWatchEvent> FileWatcher::poll() {
    return m_impl->sink.takeReady();
}

const fs::path& FileWatcher::root() const {
    return m_impl->root;
}

FileWatchBackend FileWatcher::backend() const {
    return m_impl->backend;
}

const std::string& FileWatcher::fallbackReason() const {
    return m_impl->fallbackReason;
}

std::string FileWatcher::lastError() const {
    return m_impl->sink.lastError();
}
