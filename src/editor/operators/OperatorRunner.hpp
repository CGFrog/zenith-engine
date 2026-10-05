#pragma once

#include "editor/operators/OperatorHistory.hpp"
#include "editor/operators/OperatorRegistry.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

class CommandHistory;

// The scene as the runner captured it around an Undo operator: only the
// operator's scope (OperatorScope) when it declares one, otherwise the whole
// serialized scene.
struct OperatorSnapshot {
    std::optional<SceneEditing::EntitySnapshot> entities;
    std::string scene;
};

// The most recent Register operator, kept for the Adjust Last Operation panel.
struct LastOperator {
    std::string id;
    std::string label;
    OperatorProperties properties;
    // Restored before a re-run so the operator sees the selection it first saw.
    std::vector<SceneEntityId> selectionBefore;
    bool recordedUndoStep{false};
    // History state and the operator's scope of the scene right after it,
    // used to make sure undoing the newest step really undoes this operator.
    uint64_t historyRevision{0};
    OperatorSnapshot after;
};

// Invokes operators: checks poll (explaining when only Edit mode allows the
// operator), runs execute, wraps Undo operators in one undo step capturing
// only the operator's declared scope (Operator::scope; the whole scene
// without one) (except on
// the runtime scene of Play or Simulate, whose edits are never recorded; the
// step is the result's undoStep when the operator gives one, so an operator
// that also writes a file still records one step), reports failures to diagnostics, records
// every invocation in a bounded history, and remembers the last Register operator so it can be re-run with edited
// properties. It holds references to the registry and the history the invocation contexts edit; both must outlive it.
class OperatorRunner {
  public:
    OperatorRunner(const OperatorRegistry& registry, const CommandHistory& history);

    // Runs id with the given properties, or the operator's defaults
    // completed by its invokeDefaults.
    OperatorResult invoke(std::string_view id, OperatorContext& context,
                          const OperatorProperties* properties = nullptr);
    // Runs id with its defaults (completed by its invokeDefaults) overridden
    // by a JSON object of property values, as keymap bindings and menu items
    // store them.
    OperatorResult invokeWithJson(std::string_view id, OperatorContext& context, std::string_view propertiesJson);

    // nullptr when there is no last operator, or when other edits, undo, or
    // redo have happened since it ran (detected with CommandHistory::revision,
    // which keeps counting at the history limit). adjustLast additionally
    // refuses to run unless the scene equals the operator's result.
    const LastOperator* lastOperator() const;
    // Undoes the last operator and runs it again with properties, as a fresh
    // invocation with those properties would.
    OperatorResult adjustLast(OperatorContext& context, const OperatorProperties& properties);
    void forgetLast();

    // Every invocation through invoke, invokeWithJson, or adjustLast's re-run,
    // whatever its outcome.
    const OperatorHistory& history() const {
        return m_invocations;
    }

    void clearHistory() {
        m_invocations.clear();
    }

  private:
    OperatorResult reportFailure(OperatorContext& context, OperatorResult result) const;
    OperatorResult run(const Operator* op, std::string_view id, OperatorContext& context,
                       const OperatorProperties* properties);
    // The operator's default properties with its invokeDefaults applied
    // (only when it can run, since defaults may read state its poll checks).
    static OperatorProperties invocationDefaults(const Operator& op, const OperatorContext& context);
    // Why op cannot run: Edit mode only, or unavailable for another reason.
    static std::string unavailableMessage(const Operator& op, const OperatorContext& context);
    // Runs op on the runtime scene of Play or Simulate (SceneEditing::runtime):
    // no undo step and no Adjust Last Operation, and an Undo operator's change
    // is noted in Info as discarded on Stop.
    OperatorResult runOnRuntimeScene(const Operator& op, OperatorContext& context, OperatorProperties& values);
    // Captures what op's scope covers (the whole scene without one).
    static bool captureBefore(const Operator& op, const OperatorContext& context, const OperatorProperties& values,
                              OperatorSnapshot& before);
    // The changes since before; after receives the scope's current state.
    static bool changesSince(const Operator& op, const OperatorContext& context, OperatorSnapshot before,
                             SceneChanges& changes, OperatorSnapshot& after);
    // Whether the scene still matches an operator's after snapshot.
    static bool unchangedSince(const OperatorContext& context, const OperatorSnapshot& after);
    static OperatorHistoryEntry pendingInvocation(std::string_view id, const Operator* op, std::string propertiesJson);
    void recordInvocation(OperatorHistoryEntry entry, const OperatorResult& result);

    const OperatorRegistry& m_registry;
    const CommandHistory& m_history;
    std::optional<LastOperator> m_last;
    OperatorHistory m_invocations;
};
