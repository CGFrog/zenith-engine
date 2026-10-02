#include "SystemScheduler.hpp"

#include <algorithm>
#include <utility>

SystemRegistryResult SystemRegistry::add(SystemDescriptor system) {
    if (system.name.empty()) {
        return {.error = "A system must have a name"};
    }
    if (!system.run) {
        return {.error = "System '" + system.name + "' has no callback"};
    }
    if (find(system.name) != nullptr) {
        return {.error = "A system named '" + system.name + "' is already registered"};
    }
    m_systems.push_back(std::move(system));
    return {.success = true};
}

const SystemDescriptor* SystemRegistry::find(std::string_view name) const {
    const auto found = std::find_if(m_systems.begin(), m_systems.end(),
                                    [name](const SystemDescriptor& system) { return system.name == name; });
    return found == m_systems.end() ? nullptr : &*found;
}

std::span<const SystemDescriptor> SystemRegistry::systems() const {
    return m_systems;
}

SystemScheduler::SystemScheduler(std::shared_ptr<const SystemRegistry> registry)
    : m_registry(std::move(registry)) {
    if (!m_registry) {
        return;
    }
    for (const SystemDescriptor& system : m_registry->systems()) {
        (system.phase == SystemPhase::Update ? m_update : m_fixedUpdate).push_back(&system);
    }
    // Stable, so equal orders keep registration order.
    const auto byOrder = [](const SystemDescriptor* left, const SystemDescriptor* right) {
        return left->order < right->order;
    };
    std::stable_sort(m_update.begin(), m_update.end(), byOrder);
    std::stable_sort(m_fixedUpdate.begin(), m_fixedUpdate.end(), byOrder);
}

void SystemScheduler::run(SystemPhase phase, const SystemContext& context) const {
    for (const SystemDescriptor* system : ordered(phase)) {
        if (runsIn(system->runIn, context.mode)) {
            system->run(context);
        }
    }
}

std::vector<std::string> SystemScheduler::schedule(SystemPhase phase, SimulationMode mode) const {
    std::vector<std::string> names;
    for (const SystemDescriptor* system : ordered(phase)) {
        if (runsIn(system->runIn, mode)) {
            names.push_back(system->name);
        }
    }
    return names;
}

const std::vector<const SystemDescriptor*>& SystemScheduler::ordered(SystemPhase phase) const {
    return phase == SystemPhase::Update ? m_update : m_fixedUpdate;
}
