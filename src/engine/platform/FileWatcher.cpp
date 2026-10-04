#include "engine/platform/FileWatcher.hpp"

#include "engine/platform/FileWatchBackends.hpp"

#include <system_error>
#include <thread>
#include <utility>

namespace fs = std::filesystem;
using namespace file_watch_detail;

namespace {
std::unique_ptr<WatchBackend> createNativeBackend(const fs::path& root, ChangeSink& sink, std::string& error) {
#if defined(_WIN32)
    return createWindowsBackend(root, sink, error);
#elif defined(__linux__)
    return createInotifyBackend(root, sink, error);
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
        impl->watchBackend = createPollingBackend(impl->root, impl->sink, options.pollingInterval);
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
