#include "PlayOperators.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/application/EditorPlayMode.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/document/RuntimeApply.hpp"
#include "editor/modules/GameModuleHost.hpp"

#include <algorithm>
#include <charconv>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace {
bool moduleHasSystem(const GameModuleHost& modules, std::string_view name) {
    for (const SystemPhase phase : {SystemPhase::Update, SystemPhase::FixedUpdate}) {
        if (std::ranges::any_of(modules.systems(phase),
                                [name](const SystemDescriptor* system) { return system->name == name; })) {
            return true;
        }
    }
    return false;
}

Operator timeScaleOperator(EditorPlayMode& playMode) {
    return {.id = "play.set_time_scale",
            .label = "Set Time Scale",
            .description = "Run Play and Simulate faster or slower (0 freezes them, 1 is real time)",
            .properties = OperatorProperties::make(
                TimeScaleProperties{},
                std::move(Fields<TimeScaleProperties>()
                              .add("scale", &TimeScaleProperties::scale)
                              .drag(0.01f, SimulationClock::minimumTimeScale, SimulationClock::maximumTimeScale))),
            .execute = [&playMode](OperatorContext&, OperatorProperties& properties) {
                TimeScaleProperties* values = properties.get<TimeScaleProperties>();
                if (values == nullptr) {
                    return OperatorResult::failed("Operator properties have the wrong type");
                }
                // Written back so Adjust Last and the caller see the value kept.
                values->scale = playMode.setTimeScale(values->scale);
                return OperatorResult::finished();
            }};
}

Operator simulateSystemOperator(GameModuleHost& modules, std::shared_ptr<const std::function<void()>> changed) {
    return {.id = "play.set_simulate_system",
            .label = "Set Simulate System",
            .description = "Choose whether Simulate runs a game module system",
            .flags = OperatorFlags::Internal,
            .properties = OperatorProperties::make(SimulateSystemProperties{},
                                                   std::move(Fields<SimulateSystemProperties>()
                                                                 .add("system", &SimulateSystemProperties::system)
                                                                 .add("enabled", &SimulateSystemProperties::enabled))),
            .execute = [&modules, changed](OperatorContext&, OperatorProperties& properties) {
                const SimulateSystemProperties* values = properties.get<SimulateSystemProperties>();
                if (values == nullptr) {
                    return OperatorResult::failed("Operator properties have the wrong type");
                }
                if (!moduleHasSystem(modules, values->system)) {
                    return OperatorResult::failed("The game module has no system named '" + values->system + "'");
                }
                modules.setSimulateSystemEnabled(values->system, values->enabled);
                if (*changed) {
                    (*changed)();
                }
                return OperatorResult::finished();
            }};
}

// The entities play.apply_to_edit acts on, or nullopt when entity is not a
// decimal id.
std::optional<std::vector<SceneEntityId>> applyTargets(const OperatorContext& context, const std::string& entity) {
    if (entity.empty()) {
        return context.selection.ids();
    }
    uint64_t value = 0;
    const auto [end, error] = std::from_chars(entity.data(), entity.data() + entity.size(), value);
    if (error != std::errc{} || end != entity.data() + entity.size() || value == 0) {
        return std::nullopt;
    }
    return std::vector<SceneEntityId>{SceneEntityId{value}};
}

std::string appliedDescription(const ApplyToEditSceneProperties& values, size_t entities) {
    const std::string what = values.component.empty() ? std::string("whole entity") : "'" + values.component + "'";
    return what + (entities == 1 ? std::string() : " on " + std::to_string(entities) + " entities");
}

Operator applyToEditOperator() {
    return {
        .id = "play.apply_to_edit",
        .label = "Apply to Edit Scene",
        .description = "Keep the running scene's values of the selected entities (or one component) after Stop, as "
                       "one undoable edit",
        .properties = OperatorProperties::make(
            ApplyToEditSceneProperties{}, std::move(Fields<ApplyToEditSceneProperties>()
                                                        .add("entity", &ApplyToEditSceneProperties::entity)
                                                        .add("component", &ApplyToEditSceneProperties::component))),
        .poll = [](const OperatorContext& context) { return context.editing.runtime(); },
        .execute =
            [](OperatorContext& context, OperatorProperties& properties) {
                const ApplyToEditSceneProperties* values = properties.get<ApplyToEditSceneProperties>();
                if (values == nullptr) {
                    return OperatorResult::failed("Operator properties have the wrong type");
                }
                const std::optional<std::vector<SceneEntityId>> entities = applyTargets(context, values->entity);
                if (!entities) {
                    return OperatorResult::failed("'" + values->entity + "' is not an entity id");
                }
                if (entities->empty()) {
                    return OperatorResult::cancelled("Select an entity to apply to the edit scene");
                }
                SceneChanges changes;
                if (const SceneDocumentResult result =
                        runtimeChangesForEditScene(context.editing.scene(), *context.editing.editScene(),
                                                   context.editing.assets(), context.editing.document().options(),
                                                   {.entities = *entities, .component = values->component}, changes);
                    !result) {
                    return OperatorResult::failed("Apply to Edit Scene: " + result.error);
                }
                EditorDiagnostics& diagnostics = context.editing.diagnostics();
                const std::string description = appliedDescription(*values, entities->size());
                if (changes.empty()) {
                    diagnostics.add(EditorDiagnosticSeverity::Info, "Play",
                                    "Apply to Edit Scene: the edit scene already matches (" + description + ").");
                    return OperatorResult::cancelled("The edit scene already matches");
                }
                if (!context.editing.applyToEditScene("Apply to Edit Scene", std::move(changes))) {
                    return OperatorResult::failed("Apply to Edit Scene failed");
                }
                diagnostics.add(EditorDiagnosticSeverity::Info, "Play",
                                "Applied the " + description + " to the edit scene; it stays after Stop (undo there).");
                return OperatorResult::finished();
            }};
}

Operator resetMaximaOperator(GameModuleHost& modules) {
    return {.id = "profiler.reset_max",
            .label = "Reset Maximum Timings",
            .description = "Restart the System Profiler's maximum system timings from the next frame",
            .execute = [&modules](OperatorContext&, OperatorProperties&) {
                modules.resetSystemTimingMaxima();
                return OperatorResult::finished();
            }};
}
} // namespace

OperatorRegistrationResult registerPlayOperators(OperatorRegistry& registry, PlayOperatorServices services) {
    const auto changed = std::make_shared<const std::function<void()>>(std::move(services.simulateMaskChanged));
    for (Operator op : {timeScaleOperator(services.playMode), simulateSystemOperator(services.modules, changed),
                        resetMaximaOperator(services.modules), applyToEditOperator()}) {
        if (OperatorRegistrationResult result = registry.add(std::move(op)); !result) {
            return result;
        }
    }
    return {.success = true};
}
