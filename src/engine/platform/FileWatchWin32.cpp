// The Windows file watcher backend (ReadDirectoryChangesW).
#ifdef _WIN32
#include "engine/platform/FileWatchBackends.hpp"

#include <cstddef>
#include <optional>
#include <system_error>
#include <unordered_set>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace file_watch_detail {
namespace {
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
} // namespace

std::unique_ptr<WatchBackend> createWindowsBackend(const fs::path& root, ChangeSink& sink, std::string& error) {
    return WindowsBackend::create(root, sink, error);
}
} // namespace file_watch_detail
#endif
