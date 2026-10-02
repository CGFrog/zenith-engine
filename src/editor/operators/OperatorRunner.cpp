#include "OperatorRunner.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/CommandHistory.hpp"

#include <utility>

OperatorRunner::OperatorRunner(const OperatorRegistry& registry, const CommandHistory& history)
    : m_registry(registry)
    , m_history(history) {}

OperatorResult OperatorRunner::reportFailure(OperatorContext& context, OperatorResult result) const {
    if (result.status == OperatorStatus::Failed) {
        context.editing.diagnostics().reportError(result.message);
    }
    return result;
}

OperatorResult OperatorRunner::invoke(std::string_view id, OperatorContext& context,
                                      const OperatorProperties* properties) {
    const Operator* op = m_registry.find(id);
    if (op == nullptr) {
        return reportFailure(context, OperatorResult::failed("Unknown operator '" + std::string(id) + "'"));
    }
    if (!op->canRun(context)) {
        return OperatorResult::cancelled(op->label + " is not available now");
    }
    OperatorProperties values = properties != nullptr ? *properties : op->properties;
    if (!values.sameType(op->properties)) {
        return reportFailure(context,
                             OperatorResult::failed("Properties given to '" + op->id + "' do not match the operator"));
    }

    const bool undoable = hasFlag(op->flags, OperatorFlags::Undo);
    std::vector<SceneEntityId> selectionBefore = context.selection.ids();
    std::string before;
    if (undoable && !context.editing.capture(before)) {
        return OperatorResult::failed("Could not record " + op->label + " for undo");
    }
    OperatorResult result = op->execute(context, values);
    if (result.status == OperatorStatus::Failed && result.message.empty()) {
        result.message = op->label + " failed";
    }
    if (!result) {
        return reportFailure(context, std::move(result));
    }

    bool recorded = false;
    std::string after;
    if (undoable) {
        if (!context.editing.capture(after)) {
            return OperatorResult::failed("Could not record " + op->label + " for undo");
        }
        if (after != before) {
            context.editing.commit(op->label, {}, std::move(before));
            recorded = true;
        }
    }
    if (hasFlag(op->flags, OperatorFlags::Register)) {
        m_last = LastOperator{.id = op->id,
                              .label = op->label,
                              .properties = std::move(values),
                              .selectionBefore = std::move(selectionBefore),
                              .recordedUndoStep = recorded,
                              .historyRevision = m_history.revision(),
                              .sceneAfter = std::move(after)};
    } else if (recorded) {
        // A newer undo step sits above the remembered operator.
        m_last.reset();
    }
    return result;
}

OperatorResult OperatorRunner::invokeWithJson(std::string_view id, OperatorContext& context,
                                              std::string_view propertiesJson) {
    const Operator* op = m_registry.find(id);
    if (op == nullptr || propertiesJson.empty()) {
        return invoke(id, context);
    }
    OperatorProperties values = op->properties;
    if (const std::string error = values.applyJson(propertiesJson); !error.empty()) {
        return reportFailure(context, OperatorResult::failed(op->label + ": " + error));
    }
    return invoke(id, context, &values);
}

const LastOperator* OperatorRunner::lastOperator() const {
    if (!m_last) {
        return nullptr;
    }
    // Any later execute, undo, or redo changes the revision, so the newest
    // undo step is no longer this operator's.
    if (m_last->recordedUndoStep && m_history.revision() != m_last->historyRevision) {
        return nullptr;
    }
    return &*m_last;
}

OperatorResult OperatorRunner::adjustLast(OperatorContext& context, const OperatorProperties& properties) {
    const LastOperator* last = lastOperator();
    if (last == nullptr) {
        m_last.reset();
        return OperatorResult::cancelled("There is no operation to adjust");
    }
    // Moved out: the re-run replaces the record.
    LastOperator previous = std::move(*m_last);
    m_last.reset();
    if (previous.recordedUndoStep) {
        std::string current;
        if (!context.editing.capture(current)) {
            return OperatorResult::failed("Could not adjust " + previous.label);
        }
        if (current != previous.sceneAfter) {
            return reportFailure(context, OperatorResult::failed("The scene changed after " + previous.label +
                                                                 ", so it can no longer be adjusted"));
        }
        context.editing.undo();
    }
    context.selection.assign(std::move(previous.selectionBefore));
    context.selection.removeMissing(context.editing.scene());
    return invoke(previous.id, context, &properties);
}

void OperatorRunner::forgetLast() {
    m_last.reset();
}
