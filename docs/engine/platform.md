# Platform services (`src/engine/platform/`)

Part of the [repository guide](../REPOSITORY_GUIDE.md).

## `src/engine/platform/PlatformServices.*`

Resolves platform writable locations, performs atomic user-data writes and
backup recovery, writes local-only structured diagnostics, and serializes exact
release provenance.

## `src/engine/platform/FileContents.*`

Reads a whole file into bytes or text with one sized read, reporting whether
opening or reading failed. Asset loaders use it instead of per-byte stream
iterators, which dominated Debug-build load times.

## `src/engine/platform/FileWatcher.*`

Watches a directory tree recursively on a background thread
(`ReadDirectoryChangesW` on Windows, `inotify` on Linux, snapshot polling
elsewhere or on request). `poll()` returns debounced, coalesced
Added/Modified/Removed/Renamed events with root-relative `/` paths; buffer
overflow or watch failure yields `RescanRequired`. Ignored suffixes and an
ignore predicate hide temporary files. `FileWatchCoalescer` holds the merge
rules so they can be tested without the file system.

Each backend has its own file: `FileWatchPolling.cpp`, `FileWatchWin32.cpp`
and `FileWatchInotify.cpp`; the native ones compile only on their platform.
The internal `FileWatchBackends.hpp` holds what they share: the `ChangeSink`
that filters and coalesces their events, the tree walk, and the path helpers.

## `src/engine/platform/ChildProcess.*`

Starts a program (exact arguments, no shell) or a platform-shell command line
in a working directory, captures combined stdout/stderr as lines on a
background thread, and reports exit codes. Cancelling or destroying a running
process terminates its whole tree (a Windows job object, a POSIX process
group). `launchDetached()` starts a program that outlives the caller, such as
an IDE.
