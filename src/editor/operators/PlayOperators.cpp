#include "PlayOperators.hpp"

#include "editor/application/EditorPlayMode.hpp"
#include "editor/modules/GameModuleHost.hpp"

#include <algorithm>
#include <memory>
#include <utility>

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
                        resetMaximaOperator(services.modules)}) {
        if (OperatorRegistrationResult result = registry.add(std::move(op)); !result) {
            return result;
        }
    }
    return {.success = true};
}
