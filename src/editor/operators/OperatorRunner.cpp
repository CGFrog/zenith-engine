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

OperatorProperties OperatorRunner::invocationDefaults(const Operator& op, const OperatorContext& context) {
    OperatorProperties values = op.properties;
    if (op.invokeDefaults && op.canRun(context)) {
        op.invokeDefaults(context, values);
    }
    return values;
}

OperatorHistoryEntry OperatorRunner::pendingInvocation(std::string_view id, const Operator* op,
                                                       std::string propertiesJson) {
    return {.id = std::string(id),
            .label = op != nullptr ? op->label : std::string(),
            .propertiesJson = std::move(propertiesJson)};
}

void OperatorRunner::recordInvocation(OperatorHistoryEntry entry, const OperatorResult& result) {
    entry.status = result.status;
    entry.message = result.message;
    m_invocations.record(std::move(entry));
}

OperatorResult OperatorRunner::invoke(std::string_view id, OperatorContext& context,
                                      const OperatorProperties* properties) {
    const Operator* op = m_registry.find(id);
    if (properties == nullptr && op != nullptr && op->invokeDefaults) {
        const OperatorProperties prepared = invocationDefaults(*op, context);
        return invoke(id, context, &prepared);
    }
    // Copied before running: an operator may unregister operators (a module
    // reload) or change what id and properties refer to.
    OperatorHistoryEntry entry = pendingInvocation(id, op,
                                                   properties != nullptr ? properties->toJson()
                                                   : op != nullptr       ? op->properties.toJson()
                                                                         : std::string());
    OperatorResult result = run(op, id, context, properties);
    recordInvocation(std::move(entry), result);
    return result;
}

OperatorResult OperatorRunner::run(const Operator* op, std::string_view id, OperatorContext& context,
                                   const OperatorProperties* properties) {
    if (op == nullptr) {
        return reportFailure(context, OperatorResult::failed("Unknown operator '" + std::string(id) + "'"));
    }
    if (!op->canRun(context)) {
        return OperatorResult::cancelled(unavailableMessage(*op, context));
    }
    OperatorProperties values = properties != nullptr ? *properties : op->properties;
    if (!values.sameType(op->properties)) {
        return reportFailure(context,
                             OperatorResult::failed("Properties given to '" + op->id + "' do not match the operator"));
    }
    if (context.editing.runtime()) {
        return runOnRuntimeScene(*op, context, values);
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
        // The runner keeps after for Adjust Last Operation, so it is passed
        // rather than captured again.
        if (result.undoStep) {
            // One step for the scene change and what the operator did
            // outside the scene.
            context.editing.commit(op->label, before, after, std::exchange(result.undoStep, {}));
            recorded = true;
        } else if (after != before) {
            context.editing.commit(op->label, {}, before, after);
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

std::string OperatorRunner::unavailableMessage(const Operator& op, const OperatorContext& context) {
    if (!context.editMode()) {
        // Polls are pure, so asking again as Edit mode tells whether the mode
        // is what blocks the operator.
        OperatorContext asEdit = context;
        asEdit.mode = EditorMode::Edit;
        if (op.canRun(asEdit)) {
            return op.label + " works only in Edit mode; stop Play or Simulate first";
        }
    }
    return op.label + " is not available now";
}

OperatorResult OperatorRunner::runOnRuntimeScene(const Operator& op, OperatorContext& context,
                                                 OperatorProperties& values) {
    // The snapshot only tells the runtime commit what to mark changed for
    // the renderer and physics; nothing is recorded.
    const bool changesScene = hasFlag(op.flags, OperatorFlags::Undo);
    std::string before;
    if (changesScene && !context.editing.capture(before)) {
        return OperatorResult::failed("Could not run " + op.label + " on the running scene");
    }
    OperatorResult result = op.execute(context, values);
    if (result.status == OperatorStatus::Failed && result.message.empty()) {
        result.message = op.label + " failed";
    }
    if (!result) {
        return reportFailure(context, std::move(result));
    }
    // Nothing is recorded, so nothing can be adjusted afterwards either.
    m_last.reset();
    if (changesScene) {
        context.editing.commit(op.label, {}, std::move(before));
        context.editing.diagnostics().add(EditorDiagnosticSeverity::Info, "Play",
                                          op.label + " changed the running scene only; Stop discards it.");
    }
    return result;
}

OperatorResult OperatorRunner::invokeWithJson(std::string_view id, OperatorContext& context,
                                              std::string_view propertiesJson) {
    const Operator* op = m_registry.find(id);
    if (op == nullptr || propertiesJson.empty()) {
        return invoke(id, context);
    }
    OperatorProperties values = invocationDefaults(*op, context);
    if (const std::string error = values.applyJson(propertiesJson); !error.empty()) {
        OperatorResult result = reportFailure(context, OperatorResult::failed(op->label + ": " + error));
        recordInvocation(pendingInvocation(id, op, std::string(propertiesJson)), result);
        return result;
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
