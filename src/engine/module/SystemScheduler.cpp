#include "SystemScheduler.hpp"

#include <algorithm>
#include <numeric>
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

bool SimulateSystemMask::enabledByDefault(SystemPhase phase) {
    return phase == SystemPhase::FixedUpdate;
}

bool SimulateSystemMask::enabled(std::string_view name, SystemPhase phase) const {
    const auto found = m_choices.find(name);
    return found == m_choices.end() ? enabledByDefault(phase) : found->second;
}

void SimulateSystemMask::set(std::string name, bool enabled) {
    m_choices.insert_or_assign(std::move(name), enabled);
}

void SimulateSystemMask::clear() {
    m_choices.clear();
}

const std::map<std::string, bool, std::less<>>& SimulateSystemMask::choices() const {
    return m_choices;
}

SystemProfile::SystemProfile(uint32_t averageFrames, Clock clock)
    : m_averageFrames(std::max(averageFrames, 1u))
    , m_clock(std::move(clock)) {}

std::chrono::steady_clock::time_point SystemProfile::now() const {
    return m_clock ? m_clock() : std::chrono::steady_clock::now();
}

void SystemProfile::record(const SystemDescriptor& system, std::chrono::nanoseconds duration) {
    auto [found, inserted] = m_index.try_emplace(system.name, m_timings.size());
    if (inserted) {
        m_timings.push_back({.name = system.name, .phase = system.phase});
        m_history.emplace_back();
    }
    History& history = m_history[found->second];
    history.currentMilliseconds += std::chrono::duration<double, std::milli>(duration).count();
    ++history.currentRuns;
}

void SystemProfile::finishFrame() {
    for (size_t index = 0; index < m_timings.size(); ++index) {
        SystemTiming& timing = m_timings[index];
        History& history = m_history[index];
        timing.frameMilliseconds = history.currentMilliseconds;
        timing.frameRuns = history.currentRuns;
        if (history.frames.size() < m_averageFrames) {
            history.frames.push_back(history.currentMilliseconds);
        } else {
            history.frames[history.next] = history.currentMilliseconds;
        }
        history.next = (history.next + 1) % m_averageFrames;
        timing.averageMilliseconds = std::accumulate(history.frames.begin(), history.frames.end(), 0.0) /
                                     static_cast<double>(history.frames.size());
        timing.maximumMilliseconds = history.maximumReset
                                         ? history.currentMilliseconds
                                         : std::max(timing.maximumMilliseconds, history.currentMilliseconds);
        history.maximumReset = false;
        history.currentMilliseconds = 0.0;
        history.currentRuns = 0;
    }
}

void SystemProfile::resetMaxima() {
    for (size_t index = 0; index < m_timings.size(); ++index) {
        m_timings[index].maximumMilliseconds = 0.0;
        m_history[index].maximumReset = true;
    }
}

void SystemProfile::clear() {
    m_timings.clear();
    m_history.clear();
    m_index.clear();
}

std::span<const SystemTiming> SystemProfile::timings() const {
    return m_timings;
}

const SystemTiming* SystemProfile::find(std::string_view name) const {
    const auto found = std::ranges::find(m_timings, name, &SystemTiming::name);
    return found == m_timings.end() ? nullptr : &*found;
}

uint32_t SystemProfile::averageFrames() const {
    return m_averageFrames;
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

void SystemScheduler::run(SystemPhase phase, const SystemContext& context, SystemProfile* profile) const {
    for (const SystemDescriptor* system : ordered(phase)) {
        if (!runs(*system, context.mode)) {
            continue;
        }
        if (profile == nullptr) {
            system->run(context);
            continue;
        }
        const auto begin = profile->now();
        system->run(context);
        profile->record(*system, profile->now() - begin);
    }
}

std::vector<std::string> SystemScheduler::schedule(SystemPhase phase, SimulationMode mode) const {
    std::vector<std::string> names;
    for (const SystemDescriptor* system : ordered(phase)) {
        if (runs(*system, mode)) {
            names.push_back(system->name);
        }
    }
    return names;
}

std::span<const SystemDescriptor* const> SystemScheduler::systems(SystemPhase phase) const {
    return ordered(phase);
}

void SystemScheduler::setSimulateMask(SimulateSystemMask mask) {
    m_simulateMask = std::move(mask);
}

const SimulateSystemMask& SystemScheduler::simulateMask() const {
    return m_simulateMask;
}

bool SystemScheduler::runs(const SystemDescriptor& system, SimulationMode mode) const {
    if (!runsIn(system.runIn, mode)) {
        return false;
    }
    return mode != SimulationMode::Simulate || m_simulateMask.enabled(system.name, system.phase);
}

const std::vector<const SystemDescriptor*>& SystemScheduler::ordered(SystemPhase phase) const {
    return phase == SystemPhase::Update ? m_update : m_fixedUpdate;
}
