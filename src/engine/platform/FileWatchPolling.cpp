// The snapshot-polling file watcher backend, available on every platform.
#include "engine/platform/FileWatchBackends.hpp"

#include <condition_variable>
#include <cstdint>
#include <map>
#include <optional>
#include <system_error>
#include <utility>

namespace file_watch_detail {
namespace {
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
} // namespace

std::unique_ptr<WatchBackend> createPollingBackend(fs::path root, ChangeSink& sink,
                                                   std::chrono::milliseconds interval) {
    return std::make_unique<PollingBackend>(std::move(root), sink, interval);
}
} // namespace file_watch_detail
