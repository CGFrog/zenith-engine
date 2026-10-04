#include "engine/platform/FileWatcher.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {
int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

using Events = std::vector<FileWatchEvent>;
using Clock = FileWatchCoalescer::Clock;

std::string describe(const Events& events) {
    std::string text;
    for (const FileWatchEvent& event : events) {
        text += "\n  ";
        text += fileWatchEventKindName(event.kind);
        text += ' ' + event.path;
        if (!event.oldPath.empty()) {
            text += " (from " + event.oldPath + ")";
        }
        if (event.isDirectory) {
            text += " [dir]";
        }
    }
    return text.empty() ? std::string(" (none)") : text;
}

bool contains(const Events& events, FileWatchEventKind kind, const std::string& path) {
    return std::ranges::any_of(events,
                               [&](const FileWatchEvent& event) { return event.kind == kind && event.path == path; });
}

const FileWatchEvent* findEvent(const Events& events, FileWatchEventKind kind, const std::string& path) {
    const auto found = std::ranges::find_if(
        events, [&](const FileWatchEvent& event) { return event.kind == kind && event.path == path; });
    return found == events.end() ? nullptr : &*found;
}

bool mentions(const Events& events, const std::string& fragment) {
    return std::ranges::any_of(events, [&](const FileWatchEvent& event) {
        return event.path.find(fragment) != std::string::npos || event.oldPath.find(fragment) != std::string::npos;
    });
}

// Polls until `done` accepts the accumulated events or the bounded wait expires.
Events collectUntil(FileWatcher& watcher, const std::function<bool(const Events&)>& done) {
    Events events;
    const auto deadline = Clock::now() + 5s;
    while (Clock::now() < deadline) {
        Events batch = watcher.poll();
        events.insert(events.end(), batch.begin(), batch.end());
        if (done(events)) {
            return events;
        }
        std::this_thread::sleep_for(10ms);
    }
    return events;
}

Events collectFor(FileWatcher& watcher, std::chrono::milliseconds duration) {
    Events events;
    const auto deadline = Clock::now() + duration;
    while (Clock::now() < deadline) {
        Events batch = watcher.poll();
        events.insert(events.end(), batch.begin(), batch.end());
        std::this_thread::sleep_for(10ms);
    }
    return events;
}

void writeFile(const fs::path& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
}

void appendFile(const fs::path& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary | std::ios::app);
    output << contents;
}

// Runs a rename or delete, retrying while it fails. On Windows the polling
// backend's directory walk holds search handles that can briefly refuse a
// change in that directory; the throwing overloads would abort the whole run
// (and a Debug abort dialog looks like a hang). A failure that lasts is a test
// failure.
void changeFiles(const std::string& what, const std::function<void(std::error_code&)>& change) {
    std::error_code error;
    const auto deadline = Clock::now() + 2s;
    do {
        error.clear();
        change(error);
        if (!error) {
            return;
        }
        std::this_thread::sleep_for(10ms);
    } while (Clock::now() < deadline);
    expect(false, what + " failed: " + error.message());
}

void renamePath(const fs::path& from, const fs::path& to) {
    changeFiles("renaming " + from.generic_string(), [&](std::error_code& error) { fs::rename(from, to, error); });
}

void removePath(const fs::path& path) {
    changeFiles("removing " + path.generic_string(), [&](std::error_code& error) { fs::remove_all(path, error); });
}

// ---- Coalescer rules (deterministic, no file system) ----

Events coalesce(const Events& input) {
    FileWatchCoalescer coalescer(50ms, 1000ms);
    const Clock::time_point start{};
    for (const FileWatchEvent& event : input) {
        coalescer.add(event, start);
    }
    return coalescer.takeReady(start + 50ms);
}

FileWatchEvent added(std::string path, bool isDirectory = false) {
    return {.kind = FileWatchEventKind::Added, .path = std::move(path), .isDirectory = isDirectory};
}

FileWatchEvent modifiedEvent(std::string path) {
    return {.kind = FileWatchEventKind::Modified, .path = std::move(path)};
}

FileWatchEvent removed(std::string path, bool isDirectory = false) {
    return {.kind = FileWatchEventKind::Removed, .path = std::move(path), .isDirectory = isDirectory};
}

FileWatchEvent renamed(std::string from, std::string to, bool isDirectory = false) {
    return {.kind = FileWatchEventKind::Renamed,
            .path = std::move(to),
            .oldPath = std::move(from),
            .isDirectory = isDirectory};
}

void testCoalescerRules() {
    expect(coalesce({added("a"), modifiedEvent("a"), modifiedEvent("a")}) == Events{added("a")},
           "coalescer: Added then Modified stays Added");
    expect(coalesce({added("a"), modifiedEvent("a"), removed("a")}).empty(),
           "coalescer: Added then Removed cancels out");
    expect(coalesce({modifiedEvent("a"), removed("a")}) == Events{removed("a")},
           "coalescer: Modified then Removed becomes Removed");
    expect(coalesce({removed("a"), added("a")}) == Events{modifiedEvent("a")},
           "coalescer: a replaced file becomes Modified");
    expect(coalesce({added("a"), renamed("a", "b")}) == Events{added("b")},
           "coalescer: Added then Renamed becomes Added at the new path");
    expect(coalesce({renamed("a", "b"), renamed("b", "c")}) == Events{renamed("a", "c")},
           "coalescer: rename chains collapse");
    expect(coalesce({renamed("a", "b"), renamed("b", "a")}).empty(), "coalescer: renaming back cancels out");
    expect(coalesce({renamed("a", "b"), modifiedEvent("b")}) == Events{renamed("a", "b"), modifiedEvent("b")},
           "coalescer: a modified rename target reports both");
    expect(coalesce({renamed("a", "b"), removed("b")}) == Events{removed("a")},
           "coalescer: removing a rename target removes the original");
    expect(coalesce({modifiedEvent("x"), {.kind = FileWatchEventKind::RescanRequired}, added("y")}) ==
               Events{{.kind = FileWatchEventKind::RescanRequired}, added("y")},
           "coalescer: a rescan discards earlier pending changes");

    const Events directoryRename = coalesce({added("dir", true), added("dir/file"), renamed("dir", "moved", true)});
    expect(directoryRename == Events{added("moved", true), added("moved/file")},
           "coalescer: renaming a new directory moves its pending children after it" + describe(directoryRename));
    const Events existingDirectoryRename = coalesce({added("dir/file"), renamed("dir", "moved", true)});
    expect(existingDirectoryRename == Events{renamed("dir", "moved", true), added("moved/file")},
           "coalescer: pending children follow their renamed directory" + describe(existingDirectoryRename));
    const Events directoryRemoval = coalesce({added("dir/new"), modifiedEvent("dir/old"), removed("dir", true)});
    expect(directoryRemoval == Events{removed("dir/old"), removed("dir", true)},
           "coalescer: removing a directory resolves pending children" + describe(directoryRemoval));

    FileWatchCoalescer coalescer(100ms, 300ms);
    const Clock::time_point start{};
    coalescer.add(added("a"), start);
    expect(coalescer.takeReady(start + 50ms).empty(), "coalescer: waits for the quiet period");
    coalescer.add(modifiedEvent("a"), start + 90ms);
    expect(coalescer.takeReady(start + 150ms).empty(), "coalescer: new changes extend the quiet period");
    expect(coalescer.takeReady(start + 190ms) == Events{added("a")}, "coalescer: delivers after the quiet period");
    expect(coalescer.takeReady(start + 400ms).empty(), "coalescer: a delivered burst is not repeated");

    for (int step = 0; step <= 6; ++step) {
        const Clock::time_point now = start + 1000ms + step * 50ms;
        coalescer.add(modifiedEvent("busy"), now);
        const Events ready = coalescer.takeReady(now);
        if (step < 6) {
            expect(ready.empty(), "coalescer: a busy burst is held until maxDelay");
        } else {
            expect(ready == Events{modifiedEvent("busy")},
                   "coalescer: maxDelay bounds latency under continuous changes");
        }
    }
}

// ---- Real file system behavior ----

void testCreateFailures(const fs::path& base) {
    std::unique_ptr<FileWatcher> watcher;
    const FileWatcherResult missing = FileWatcher::create(base / "does_not_exist", {}, watcher);
    expect(!missing && !watcher, "watching a missing root fails");
    expect(missing.error.find("does_not_exist") != std::string::npos, "missing root error names the path");

    writeFile(base / "plain_file.txt", "x");
    const FileWatcherResult notDirectory = FileWatcher::create(base / "plain_file.txt", {}, watcher);
    expect(!notDirectory && !watcher, "watching a file instead of a directory fails");

    FileWatcherOptions invalid;
    invalid.pollingInterval = 0ms;
    expect(!FileWatcher::create(base, invalid, watcher), "a zero polling interval is rejected");
}

void testWatcher(const fs::path& base, FileWatchBackendPreference preference, const std::string& name) {
    const std::string label = name + ": ";
    const fs::path root = base / name;
    fs::create_directories(root);
    writeFile(root / "existing.txt", "before the watcher started");

    FileWatcherOptions options;
    options.debounce = 150ms;
    options.pollingInterval = 40ms;
    options.backend = preference;
    options.ignoredSuffixes = {".tmp"};
    options.ignore = [](std::string_view path) { return path == "skipped"; };

    std::unique_ptr<FileWatcher> watcher;
    const FileWatcherResult created = FileWatcher::create(root, options, watcher);
    expect(static_cast<bool>(created), label + "create succeeds: " + created.error);
    if (!watcher) {
        return;
    }
    const bool polling = watcher->backend() == FileWatchBackend::Polling;
    if (preference == FileWatchBackendPreference::Polling) {
        expect(polling, label + "polling was requested");
    } else {
#if defined(_WIN32) || defined(__linux__)
        expect(!polling, label + "native backend is used: " + watcher->fallbackReason());
#endif
    }
    expect(collectFor(*watcher, 250ms).empty(), label + "pre-existing files are not reported");

    writeFile(root / "a.txt", "first");
    Events events =
        collectUntil(*watcher, [](const Events& all) { return contains(all, FileWatchEventKind::Added, "a.txt"); });
    const Events settled = collectFor(*watcher, 300ms);
    events.insert(events.end(), settled.begin(), settled.end());
    const FileWatchEvent* addedFile = findEvent(events, FileWatchEventKind::Added, "a.txt");
    expect(addedFile && !addedFile->isDirectory, label + "new file is Added" + describe(events));
    expect(!contains(events, FileWatchEventKind::Modified, "a.txt"),
           label + "writes during creation coalesce into Added" + describe(events));

    appendFile(root / "a.txt", " and more");
    events =
        collectUntil(*watcher, [](const Events& all) { return contains(all, FileWatchEventKind::Modified, "a.txt"); });
    expect(contains(events, FileWatchEventKind::Modified, "a.txt"),
           label + "changed file is Modified" + describe(events));

    fs::create_directories(root / "sub" / "deeper");
    writeFile(root / "sub" / "deeper" / "b.txt", "nested");
    events = collectUntil(*watcher, [](const Events& all) {
        return contains(all, FileWatchEventKind::Added, "sub") &&
               contains(all, FileWatchEventKind::Added, "sub/deeper") &&
               contains(all, FileWatchEventKind::Added, "sub/deeper/b.txt");
    });
    const FileWatchEvent* addedDirectory = findEvent(events, FileWatchEventKind::Added, "sub/deeper");
    const FileWatchEvent* nestedFile = findEvent(events, FileWatchEventKind::Added, "sub/deeper/b.txt");
    expect(addedDirectory && addedDirectory->isDirectory,
           label + "nested directory is Added as a directory" + describe(events));
    expect(nestedFile && !nestedFile->isDirectory, label + "nested file is Added" + describe(events));

    renamePath(root / "a.txt", root / "renamed.txt");
    events = collectUntil(*watcher,
                          [](const Events& all) { return contains(all, FileWatchEventKind::Renamed, "renamed.txt"); });
    const FileWatchEvent* renamedFile = findEvent(events, FileWatchEventKind::Renamed, "renamed.txt");
    expect(renamedFile && renamedFile->oldPath == "a.txt" && !renamedFile->isDirectory,
           label + "file rename is reported with its old path" + describe(events));

    renamePath(root / "sub", root / "moved");
    if (polling) {
        events = collectUntil(*watcher, [](const Events& all) {
            return contains(all, FileWatchEventKind::Removed, "sub") &&
                   contains(all, FileWatchEventKind::Added, "moved");
        });
        const FileWatchEvent* removedDirectory = findEvent(events, FileWatchEventKind::Removed, "sub");
        expect(removedDirectory && removedDirectory->isDirectory,
               label + "polling reports the old directory removed" + describe(events));
        expect(mentions(events, "moved/deeper/b.txt"), label + "polling reports the moved contents" + describe(events));
    } else {
        events = collectUntil(*watcher,
                              [](const Events& all) { return contains(all, FileWatchEventKind::Renamed, "moved"); });
        const FileWatchEvent* renamedDirectory = findEvent(events, FileWatchEventKind::Renamed, "moved");
        expect(renamedDirectory && renamedDirectory->oldPath == "sub" && renamedDirectory->isDirectory,
               label + "directory rename is reported once" + describe(events));
    }

    writeFile(root / "scratch.tmp", "ignored by suffix");
    fs::create_directories(root / "skipped" / "inner");
    writeFile(root / "skipped" / "inner" / "x.txt", "ignored by predicate");
    events = collectFor(*watcher, 400ms);
    expect(!mentions(events, "scratch") && !mentions(events, "skipped"),
           label + "ignored paths are not reported" + describe(events));

    writeFile(root / "doc.txt", "version 1");
    collectUntil(*watcher, [](const Events& all) { return contains(all, FileWatchEventKind::Added, "doc.txt"); });
    writeFile(root / "doc.txt.tmp", "version 2, longer");
    renamePath(root / "doc.txt.tmp", root / "doc.txt");
    events = collectUntil(*watcher, [](const Events& all) {
        return contains(all, FileWatchEventKind::Modified, "doc.txt") ||
               contains(all, FileWatchEventKind::Added, "doc.txt");
    });
    expect(contains(events, FileWatchEventKind::Modified, "doc.txt") ||
               contains(events, FileWatchEventKind::Added, "doc.txt"),
           label + "a save through a temporary file reports the saved file" + describe(events));
    expect(!mentions(events, ".tmp") && !contains(events, FileWatchEventKind::Removed, "doc.txt"),
           label + "the temporary file of a save is hidden" + describe(events));

    writeFile(root / "transient.txt", "short lived");
    removePath(root / "transient.txt");
    events = collectFor(*watcher, 400ms);
    expect(!mentions(events, "transient"),
           label + "a file created and removed within the debounce is not reported" + describe(events));

    removePath(root / "renamed.txt");
    events = collectUntil(*watcher,
                          [](const Events& all) { return contains(all, FileWatchEventKind::Removed, "renamed.txt"); });
    expect(contains(events, FileWatchEventKind::Removed, "renamed.txt"),
           label + "deleted file is Removed" + describe(events));

    removePath(root / "moved");
    events =
        collectUntil(*watcher, [](const Events& all) { return contains(all, FileWatchEventKind::Removed, "moved"); });
    const FileWatchEvent* removedTree = findEvent(events, FileWatchEventKind::Removed, "moved");
    expect(removedTree && removedTree->isDirectory,
           label + "deleted directory is Removed as a directory" + describe(events));
    expect(watcher->lastError().empty(), label + "no runtime error: " + watcher->lastError());

    const auto stopStart = Clock::now();
    watcher.reset();
    expect(Clock::now() - stopStart < 1s, label + "destruction stops the watcher promptly");
}

void testPollingRootRemoval(const fs::path& base) {
    const fs::path root = base / "removed_root";
    fs::create_directories(root);
    FileWatcherOptions options;
    options.debounce = 20ms;
    options.pollingInterval = 30ms;
    options.backend = FileWatchBackendPreference::Polling;
    std::unique_ptr<FileWatcher> watcher;
    expect(static_cast<bool>(FileWatcher::create(root, options, watcher)), "removed root: create succeeds");
    if (!watcher) {
        return;
    }
    removePath(root);
    const Events events = collectUntil(*watcher, [](const Events& all) {
        return std::ranges::any_of(
            all, [](const FileWatchEvent& event) { return event.kind == FileWatchEventKind::RescanRequired; });
    });
    expect(std::ranges::any_of(
               events, [](const FileWatchEvent& event) { return event.kind == FileWatchEventKind::RescanRequired; }),
           "removed root: a rescan is requested" + describe(events));
    expect(!watcher->lastError().empty(), "removed root: the failure is reported");
}
} // namespace

// The optional argument selects one backend ("native" or "polling") so CTest
// can run the two timing-bound halves in parallel; no argument runs both.
int main(int argc, char** argv) {
    const std::string backend = argc > 1 ? argv[1] : "all";
    if (backend != "all" && backend != "native" && backend != "polling") {
        std::cerr << "Usage: zenith_file_watcher_tests [native|polling]\n";
        return 1;
    }
    const bool runNative = backend != "polling";
    const bool runPolling = backend != "native";

    const fs::path base = fs::temp_directory_path() / ("zenith_file_watcher_tests_" + backend);
    std::error_code error;
    fs::remove_all(base, error);
    fs::create_directories(base);

    if (runNative) {
        testCoalescerRules();
        testCreateFailures(base);
        testWatcher(base, FileWatchBackendPreference::Automatic, "native");
    }
    if (runPolling) {
        testWatcher(base, FileWatchBackendPreference::Polling, "polling");
        testPollingRootRemoval(base);
    }

    fs::remove_all(base, error);
    if (failures == 0) {
        std::cout << "File watcher tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
