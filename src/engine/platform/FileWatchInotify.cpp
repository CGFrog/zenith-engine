// The Linux file watcher backend (inotify).
#if defined(__linux__)
#include "engine/platform/FileWatchBackends.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <unordered_map>
#include <utility>

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

namespace file_watch_detail {
namespace {
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
} // namespace

std::unique_ptr<WatchBackend> createInotifyBackend(const fs::path& root, ChangeSink& sink, std::string& error) {
    return InotifyBackend::create(root, sink, error);
}
} // namespace file_watch_detail
#endif
