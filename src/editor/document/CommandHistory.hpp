#pragma once

#include "engine/scene/SceneDocument.hpp"

#include <cstddef>
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
};

class SceneStateCommand final : public EditorCommand {
  public:
    SceneStateCommand(std::string name, std::string before, std::string after, std::string coalesceKey = {});

    std::string_view name() const override;
    EditorCommandResult execute(EditorCommandContext& context) override;
    EditorCommandResult undo(EditorCommandContext& context) override;
    bool mergeWith(const EditorCommand& newer) override;

  private:
    std::string m_name;
    std::string m_before;
    std::string m_after;
    std::string m_coalesceKey;
};

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

  private:
    void enforceLimit();

    size_t m_limit;
    std::vector<std::unique_ptr<EditorCommand>> m_commands;
    size_t m_position{0};
    std::optional<size_t> m_savedPosition{0};
};
