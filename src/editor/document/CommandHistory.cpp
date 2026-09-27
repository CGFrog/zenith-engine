#include "CommandHistory.hpp"

#include "engine/assets/Assets.hpp"

#include <algorithm>
#include <utility>

bool EditorCommand::mergeWith(const EditorCommand&) {
    return false;
}

SceneStateCommand::SceneStateCommand(std::string name, std::string before, std::string after, std::string coalesceKey)
    : m_name(std::move(name))
    , m_before(std::move(before))
    , m_after(std::move(after))
    , m_coalesceKey(std::move(coalesceKey)) {}

std::string_view SceneStateCommand::name() const {
    return m_name;
}

EditorCommandResult SceneStateCommand::execute(EditorCommandContext& context) {
    const SceneDocumentResult result =
        SceneDocument::deserialize(m_after, context.assets, context.documentOptions, context.scene);
    return {.success = result.success, .error = result.error};
}

EditorCommandResult SceneStateCommand::undo(EditorCommandContext& context) {
    const SceneDocumentResult result =
        SceneDocument::deserialize(m_before, context.assets, context.documentOptions, context.scene);
    return {.success = result.success, .error = result.error};
}

bool SceneStateCommand::mergeWith(const EditorCommand& newer) {
    const auto* state = dynamic_cast<const SceneStateCommand*>(&newer);
    if (state == nullptr || m_coalesceKey.empty() || state->m_coalesceKey != m_coalesceKey) {
        return false;
    }
    m_after = state->m_after;
    return true;
}

CommandHistory::CommandHistory(size_t limit)
    : m_limit(std::max<size_t>(limit, 1)) {}

EditorCommandResult CommandHistory::execute(std::unique_ptr<EditorCommand> command, EditorCommandContext& context) {
    if (!command) {
        return {.error = "Cannot execute a null editor command"};
    }
    if (EditorCommandResult result = command->execute(context); !result) {
        return result;
    }
    if (m_position < m_commands.size()) {
        if (m_savedPosition && *m_savedPosition > m_position) {
            m_savedPosition.reset();
        }
        m_commands.erase(m_commands.begin() + static_cast<std::ptrdiff_t>(m_position), m_commands.end());
    }
    if (m_position > 0 && m_commands[m_position - 1]->mergeWith(*command)) {
        if (m_savedPosition && *m_savedPosition == m_position) {
            m_savedPosition.reset();
        }
        return {.success = true};
    }
    m_commands.push_back(std::move(command));
    ++m_position;
    enforceLimit();
    return {.success = true};
}

EditorCommandResult CommandHistory::undo(EditorCommandContext& context) {
    if (!canUndo()) {
        return {.error = "There is no command to undo"};
    }
    if (EditorCommandResult result = m_commands[m_position - 1]->undo(context); !result) {
        return result;
    }
    --m_position;
    return {.success = true};
}

EditorCommandResult CommandHistory::redo(EditorCommandContext& context) {
    if (!canRedo()) {
        return {.error = "There is no command to redo"};
    }
    if (EditorCommandResult result = m_commands[m_position]->execute(context); !result) {
        return result;
    }
    ++m_position;
    return {.success = true};
}

void CommandHistory::clear() {
    m_commands.clear();
    m_position = 0;
    m_savedPosition = 0;
}

void CommandHistory::markSaved() {
    m_savedPosition = m_position;
}

void CommandHistory::crossBoundary(CommandHistoryBoundary boundary) {
    switch (boundary) {
    case CommandHistoryBoundary::NewDocument:
    case CommandHistoryBoundary::OpenDocument:
        clear();
        break;
    case CommandHistoryBoundary::SaveDocument:
        markSaved();
        break;
    case CommandHistoryBoundary::EnterPlayMode:
    case CommandHistoryBoundary::LeavePlayMode:
        // Play mode owns a separate runtime scene. Its mutations never enter
        // the editable document's history, so the timeline remains intact.
        break;
    }
}

bool CommandHistory::canUndo() const {
    return m_position != 0;
}

bool CommandHistory::canRedo() const {
    return m_position < m_commands.size();
}

bool CommandHistory::dirty() const {
    return !m_savedPosition || *m_savedPosition != m_position;
}

size_t CommandHistory::size() const {
    return m_commands.size();
}

void CommandHistory::enforceLimit() {
    if (m_commands.size() <= m_limit) {
        return;
    }
    const size_t removeCount = m_commands.size() - m_limit;
    m_commands.erase(m_commands.begin(), m_commands.begin() + static_cast<std::ptrdiff_t>(removeCount));
    m_position -= std::min(m_position, removeCount);
    if (m_savedPosition) {
        if (*m_savedPosition < removeCount) {
            m_savedPosition.reset();
        } else {
            *m_savedPosition -= removeCount;
        }
    }
}
