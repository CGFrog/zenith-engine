#pragma once

#include "engine/scene/SceneDocument.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class AssetManager;

struct EditorCommandContext {
    Scene& scene;
    AssetManager& assets;
    const SceneDocumentOptions& documentOptions;
};

struct EditorCommandResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

class EditorCommand {
  public:
    virtual ~EditorCommand() = default;

    virtual std::string_view name() const = 0;
    virtual EditorCommandResult execute(EditorCommandContext& context) = 0;
    virtual EditorCommandResult undo(EditorCommandContext& context) = 0;
    virtual bool mergeWith(const EditorCommand& newer);
    // Approximate memory the command keeps to undo and redo itself; zero for
    // commands that do not report it.
    virtual size_t storedBytes() const;
};

// Replaces the whole scene with a serialized document on each side. Simple
// but costly for large scenes: it stores two full documents and rebuilds
// every entity, so scene edits are recorded as EntityDiffCommand instead.
class SceneStateCommand final : public EditorCommand {
  public:
    SceneStateCommand(std::string name, std::string before, std::string after, std::string coalesceKey = {});

    std::string_view name() const override;
    EditorCommandResult execute(EditorCommandContext& context) override;
    EditorCommandResult undo(EditorCommandContext& context) override;
    bool mergeWith(const EditorCommand& newer) override;
    size_t storedBytes() const override;

  private:
    std::string m_name;
    std::string m_before;
    std::string m_after;
    std::string m_coalesceKey;
};

// Records an edit as the document form of only the entities it added,
// removed, or changed, plus the collection definitions when it changed them
// (SceneDocument::diff), and undoes or redoes it by patching those in place
// (SceneDocument::applyChanges). Memory and time scale with the size of the
// edit rather than the scene, and entities the edit did not touch keep their
// EnTT handles across undo and redo.
//
// Commands with the same non-empty coalesce key merge: the merged command
// keeps each entity's (and the collections') state before the first edit and
// after the last one, dropping what ended where it started.
class EntityDiffCommand final : public EditorCommand {
  public:
    EntityDiffCommand(std::string name, SceneChanges changes, std::string coalesceKey = {});

    std::string_view name() const override;
    EditorCommandResult execute(EditorCommandContext& context) override;
    EditorCommandResult undo(EditorCommandContext& context) override;
    bool mergeWith(const EditorCommand& newer) override;
    size_t storedBytes() const override;

    // Entities sorted by id.
    const SceneChanges& changes() const;

  private:
    std::string m_name;
    SceneChanges m_changes;
    std::string m_coalesceKey;
};

// Builds the undo step for an edit from its scene changes (already applied),
// for an edit whose step must also undo something outside the scene, such as
// a prefab file write (PrefabWriteCommand). The step's execute must reproduce
// the edit on a scene that has the changes undone, and leave anything outside
// the scene that already matches alone.
using EditorCommandFactory = std::function<std::unique_ptr<EditorCommand>(std::string name, SceneChanges changes)>;

// Document replacement starts a new history timeline, saving establishes a
// clean point on the current timeline, and play mode only operates on a
// separate runtime copy so entering or leaving it preserves editor history.
enum class CommandHistoryBoundary {
    NewDocument,
    OpenDocument,
    SaveDocument,
    EnterPlayMode,
    LeavePlayMode,
};

// A point on a history's timeline (CommandHistory::marker), for undoing
// exactly the steps recorded after it.
struct CommandHistoryMarker {
    uint64_t revision{0};
};

class CommandHistory {
  public:
    explicit CommandHistory(size_t limit = 256);

    EditorCommandResult execute(std::unique_ptr<EditorCommand> command, EditorCommandContext& context);
    EditorCommandResult undo(EditorCommandContext& context);
    EditorCommandResult redo(EditorCommandContext& context);
    void clear();
    void markSaved();
    void crossBoundary(CommandHistoryBoundary boundary);

    bool canUndo() const;
    bool canRedo() const;
    bool dirty() const;
    size_t size() const;
    // Sum of the recorded commands' storedBytes().
    size_t storedBytes() const;
    // Increases on every change to the timeline (execute, including merges,
    // undo, redo, and clear) and never repeats, unlike size(), which stops
    // growing at the limit. Callers compare it to detect any later edit.
    uint64_t revision() const;
    // The current point of the timeline.
    CommandHistoryMarker marker() const;
    // How many undo steps take the timeline back to marker: the undoable
    // steps recorded after it. Unlike a difference of size(), this stays
    // right when the new steps discarded redo steps or the limit dropped old
    // ones. A step that merged edits from before the marker with later ones
    // started before it, so it is not counted.
    size_t stepsSince(CommandHistoryMarker marker) const;

  private:
    void enforceLimit();

    size_t m_limit;
    std::vector<std::unique_ptr<EditorCommand>> m_commands;
    // The revision at which each command was recorded, parallel to
    // m_commands (and so increasing).
    std::vector<uint64_t> m_recordedAt;
    size_t m_position{0};
    std::optional<size_t> m_savedPosition{0};
    uint64_t m_revision{0};
};
