#include "CommandHistory.hpp"

#include "engine/assets/Assets.hpp"

#include <algorithm>
#include <utility>

namespace {
size_t optionalBytes(const std::optional<std::string>& value) {
    return value ? value->capacity() : 0;
}
} // namespace

bool EditorCommand::mergeWith(const EditorCommand&) {
    return false;
}

size_t EditorCommand::storedBytes() const {
    return 0;
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

size_t SceneStateCommand::storedBytes() const {
    return sizeof(*this) + m_name.capacity() + m_before.capacity() + m_after.capacity() + m_coalesceKey.capacity();
}

EntityDiffCommand::EntityDiffCommand(std::string name, std::vector<SceneEntityChange> changes, std::string coalesceKey)
    : m_name(std::move(name))
    , m_changes(std::move(changes))
    , m_coalesceKey(std::move(coalesceKey)) {
    std::ranges::sort(m_changes, {}, [](const SceneEntityChange& change) { return change.id.value; });
}

std::string_view EntityDiffCommand::name() const {
    return m_name;
}

EditorCommandResult EntityDiffCommand::execute(EditorCommandContext& context) {
    const SceneDocumentResult result = SceneDocument::applyChanges(
        m_changes, SceneChangeDirection::Forward, context.assets, context.documentOptions, context.scene);
    return {.success = result.success, .error = result.error};
}

EditorCommandResult EntityDiffCommand::undo(EditorCommandContext& context) {
    const SceneDocumentResult result = SceneDocument::applyChanges(
        m_changes, SceneChangeDirection::Backward, context.assets, context.documentOptions, context.scene);
    return {.success = result.success, .error = result.error};
}

bool EntityDiffCommand::mergeWith(const EditorCommand& newer) {
    const auto* diff = dynamic_cast<const EntityDiffCommand*>(&newer);
    if (diff == nullptr || m_coalesceKey.empty() || diff->m_coalesceKey != m_coalesceKey) {
        return false;
    }
    // Both lists are sorted by id. An entity keeps this command's before
    // state when it already changed here, and takes the newer after state
    // when the newer edit touched it.
    std::vector<SceneEntityChange> merged;
    merged.reserve(m_changes.size() + diff->m_changes.size());
    auto older = m_changes.begin();
    auto latest = diff->m_changes.begin();
    while (older != m_changes.end() || latest != diff->m_changes.end()) {
        if (latest == diff->m_changes.end() || (older != m_changes.end() && older->id < latest->id)) {
            merged.push_back(std::move(*older++));
            continue;
        }
        if (older == m_changes.end() || latest->id < older->id) {
            merged.push_back(*latest++);
            continue;
        }
        SceneEntityChange change{.id = older->id, .before = std::move(older->before), .after = latest->after};
        ++older;
        ++latest;
        if (change.before != change.after) {
            merged.push_back(std::move(change));
        }
    }
    m_changes = std::move(merged);
    return true;
}

size_t EntityDiffCommand::storedBytes() const {
    size_t bytes =
        sizeof(*this) + m_name.capacity() + m_coalesceKey.capacity() + m_changes.capacity() * sizeof(SceneEntityChange);
    for (const SceneEntityChange& change : m_changes) {
        bytes += optionalBytes(change.before) + optionalBytes(change.after);
    }
    return bytes;
}

const std::vector<SceneEntityChange>& EntityDiffCommand::changes() const {
    return m_changes;
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

size_t CommandHistory::storedBytes() const {
    size_t bytes = 0;
    for (const std::unique_ptr<EditorCommand>& command : m_commands) {
        bytes += command->storedBytes();
    }
    return bytes;
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
