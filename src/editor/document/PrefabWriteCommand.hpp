#pragma once

// Undoable prefab file writes.

#include "editor/document/CommandHistory.hpp"
#include "engine/scene/Prefab.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

// Undoable prefab file writes.
//
// Prefab files are project content that several scenes (and the running game)
// read, so the editor writes them as soon as the user applies or saves, and
// undo and redo write them back. A file is only ever rewritten when it still
// holds what the editor last left there: a file changed outside the editor
// since the step is never overwritten.

class Scene;

// A prefab file the editor wrote: file is absolute; bytes is its new content,
// or nullopt when the write removed it. origin is the scene of the document
// whose operator or undo step wrote it, so the session can refresh the
// prefab's instances in every other open document; null for a prefab saved as
// a document of its own, whose saver refreshes the others itself.
using PrefabFileWritten = std::function<void(const std::filesystem::path& file, const std::optional<std::string>& bytes,
                                             const Scene* origin)>;

// What the editor tells the rest of the session about each prefab file it
// writes, removes, or rewrites on undo and redo. The session uses it to drop
// cached copies (its own and the game's PrefabSystem caches) and to note the
// content in its PrefabWriteLedger, so the asset watcher does not mistake the
// editor's own write for an outside change. Unset hooks are skipped
// (headless tests).
struct PrefabWriteHooks {
    PrefabFileWritten fileWritten;
    // The editor built instances from file, which held bytes (nullopt: it
    // did not exist): the session notes the content in its ledger unless it
    // already knows the file, so an outside change made later still knows
    // what the file held before it (notePrefabFilesRead).
    std::function<void(const std::filesystem::path& file, const std::optional<std::string>& bytes)> fileRead;

    void notify(const std::filesystem::path& file, const std::optional<std::string>& bytes, const Scene* origin) const {
        if (fileWritten) {
            fileWritten(file, bytes, origin);
        }
    }

    void notifyRead(const std::filesystem::path& file, const std::optional<std::string>& bytes) const {
        if (fileRead) {
            fileRead(file, bytes);
        }
    }
};

// One prefab file's content before and after a step; nullopt means the file
// did not exist on that side (a step that created it).
struct PrefabFileWrite {
    // Absolute path of the prefab file.
    std::filesystem::path file;
    std::optional<std::string> before;
    std::optional<std::string> after;
};

// Reports through hooks.fileRead the current content of every prefab file the
// instances in scene name (nested instances included), or with subtree set
// only those under subtree (an instance just placed), each file once. Call it
// whenever a document's scene is opened or instances are added to it, so the
// session's PrefabWriteLedger knows each file's content before any outside
// change. References resolve under assetRoot; unreadable files are skipped.
void notePrefabFilesRead(const Scene& scene, const std::filesystem::path& assetRoot, const PrefabWriteHooks& hooks,
                         Entity subtree = entt::null);

// The file's whole content, or nullopt when it does not exist. Fails (returns
// false with error set) when the file exists but cannot be read.
bool readPrefabFileBytes(const std::filesystem::path& file, std::optional<std::string>& bytes, std::string& error);

// One undo step for a prefab file write and the scene changes made with it
// (for example the instances an apply linked and refreshed): undo puts back
// both the file's previous content and the scene, redo both the new ones.
//
// Files are written atomically (a temporary file renamed over the old one, as
// savePrefab does); a step whose file did not exist removes it on undo. Before
// writing, the command checks that the file holds the content the step left
// it with (after, on undo) or found it in (before, on redo). A file already
// holding the target content is left alone, so the first execute, which
// CommandHistory runs on a write that already happened, writes nothing. Any
// other content means someone changed the file since the step: the command
// fails with "<file> changed on disk since this step; undo skipped" (or
// redo), and changes neither the file nor the scene.
//
// The scene is patched first and the file written after it, so a scene patch
// that fails leaves the file alone; a file write that fails puts the scene
// back. Every execute and undo reports the file's resulting content through
// the hooks, including the first execute, which is how the session learns of
// the write the step records.
class PrefabWriteCommand final : public EditorCommand {
  public:
    PrefabWriteCommand(std::string name, PrefabFileWrite write, SceneChanges changes, PrefabWriteHooks hooks = {});

    std::string_view name() const override;
    EditorCommandResult execute(EditorCommandContext& context) override;
    EditorCommandResult undo(EditorCommandContext& context) override;
    size_t storedBytes() const override;

    const PrefabFileWrite& write() const {
        return m_write;
    }

    const SceneChanges& changes() const {
        return m_scene.changes();
    }

  private:
    // Moves the file from `from` to `to` and the scene in direction.
    EditorCommandResult apply(EditorCommandContext& context, const std::optional<std::string>& from,
                              const std::optional<std::string>& to, bool forward, std::string_view action);

    std::string m_name;
    PrefabFileWrite m_write;
    EntityDiffCommand m_scene;
    PrefabWriteHooks m_hooks;
};

// The content the editor last wrote to (or already reconciled for) each
// prefab file, so the asset watcher can tell the editor's own writes from
// changes made outside it, and so an outside change can be compared with what
// the file held before it (PrefabAuthoring::refreshInstances' previous).
// Paths are compared absolute and lexically normalized. Holds whole contents:
// prefab files are small, and the previous content is what an outside change
// needs to keep each instance's overrides.
class PrefabWriteLedger {
  public:
    // Notes that file now holds bytes (nullopt: removed) because the editor
    // wrote it or has already reacted to the change.
    void record(const std::filesystem::path& file, std::optional<std::string> bytes);
    // Whether bytes is what was last recorded for file; false when nothing
    // was recorded.
    bool matches(const std::filesystem::path& file, const std::optional<std::string>& bytes) const;
    // The content last recorded for file: nullptr when nothing was recorded,
    // else nullopt for a removed file.
    const std::optional<std::string>* recorded(const std::filesystem::path& file) const;
    void forget(const std::filesystem::path& file);
    void clear();

  private:
    static std::string key(const std::filesystem::path& file);

    std::unordered_map<std::string, std::optional<std::string>> m_contents;
};

// How the asset watcher's report of a changed prefab file reads against the
// ledger (classifyPrefabFileChange).
struct PrefabFileChange {
    // The file holds what the editor last wrote or reconciled: only caches
    // need dropping.
    bool external{false};
    // The prefab's project-relative reference, as instances name it.
    std::filesystem::path reference;
    // The prefab's display name (its metadata, else its file name), for
    // messages and undo step names.
    std::string displayName;
    // What the file held before the outside change, when the ledger knew it;
    // pass it to PrefabAuthoring::refreshInstances.
    std::optional<PrefabDocument> previous;
};

// Reads file (absolute, under assetRoot) and compares it with the ledger. An
// outside change is recorded in the ledger, so the watcher's repeated reports
// of the same change, and the refresh it causes, run once. Fails when the file
// cannot be read or lies outside assetRoot.
PrefabResult classifyPrefabFileChange(PrefabWriteLedger& ledger, const std::filesystem::path& file,
                                      const std::filesystem::path& assetRoot, PrefabFileChange& output);

// Describes a change of file (absolute, under assetRoot) from previous to
// current content (nullopt: no file, or unknown) as a change to refresh
// instances from: its reference, display name, and previous document. For
// the editor's own writes, which other open documents catch up with. Fails
// when file lies outside assetRoot.
PrefabResult describePrefabFileChange(const std::filesystem::path& file, const std::filesystem::path& assetRoot,
                                      const std::optional<std::string>& current,
                                      const std::optional<std::string>& previous, PrefabFileChange& output);
