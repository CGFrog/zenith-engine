#pragma once

#include "editor/operators/OperatorRegistry.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

class CommandHistory;

// The most recent Register operator, kept for the Adjust Last Operation panel.
struct LastOperator {
    std::string id;
    std::string label;
    OperatorProperties properties;
    // Restored before a re-run so the operator sees the selection it first saw.
    std::vector<SceneEntityId> selectionBefore;
    bool recordedUndoStep{false};
    // History state and scene snapshot right after the operator, used to make
    // sure undoing the newest step really undoes this operator.
    size_t historySize{0};
    std::string sceneAfter;
};

// Invokes operators: checks poll, runs execute, wraps Undo operators in one
// undo step, reports failures to diagnostics, and remembers the last Register
// operator so it can be re-run with edited properties. It holds references to
// the registry and the history the invocation contexts edit; both must
// outlive it.
class OperatorRunner {
  public:
    OperatorRunner(const OperatorRegistry& registry, const CommandHistory& history);

    // Runs id with the given properties, or the operator's defaults.
    OperatorResult invoke(std::string_view id, OperatorContext& context,
                          const OperatorProperties* properties = nullptr);
    // Runs id with its defaults overridden by a JSON object of property
    // values, as keymap bindings and menu items store them.
    OperatorResult invokeWithJson(std::string_view id, OperatorContext& context, std::string_view propertiesJson);

    // nullptr when there is no last operator, or when other edits, undo, or
    // redo have happened since it ran. Detection compares the history size,
    // which stops growing once the history reaches its step limit; adjustLast
    // then still refuses to run unless the scene equals the operator's result.
    const LastOperator* lastOperator() const;
    // Undoes the last operator and runs it again with properties, as a fresh
    // invocation with those properties would.
    OperatorResult adjustLast(OperatorContext& context, const OperatorProperties& properties);
    void forgetLast();

  private:
    OperatorResult reportFailure(OperatorContext& context, OperatorResult result) const;

    const OperatorRegistry& m_registry;
    const CommandHistory& m_history;
    std::optional<LastOperator> m_last;
};
