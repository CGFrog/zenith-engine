#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class Scene;
struct ApplicationContext;

// Whether the host is authoring a scene (the editor with simulation stopped)
// or simulating it (a game, or the editor's play mode).
enum class SimulationMode {
    Edit,
    Play,
};

// The simulation modes a system runs in; combine with |.
enum class RunIn : uint32_t {
    Edit = 1u << 0,
    Play = 1u << 1,
    EditAndPlay = Edit | Play,
};

constexpr RunIn operator|(RunIn left, RunIn right) {
    return static_cast<RunIn>(static_cast<uint32_t>(left) | static_cast<uint32_t>(right));
}

constexpr bool runsIn(RunIn flags, SimulationMode mode) {
    const RunIn required = mode == SimulationMode::Edit ? RunIn::Edit : RunIn::Play;
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(required)) != 0;
}

// The frame step a system runs in. Update runs once per rendered frame with
// the frame's duration; FixedUpdate runs zero or more times per frame with the
// fixed simulation step, before physics advances.
enum class SystemPhase {
    Update,
    FixedUpdate,
};

// What a system may touch while it runs. application is null when the host
// has no running application services, as in the editor's edit mode.
struct SystemContext {
    Scene& scene;
    ApplicationContext* application{nullptr};
    SimulationMode mode{SimulationMode::Play};
    // Seconds; the frame duration for Update, the fixed step for FixedUpdate.
    float deltaSeconds{0.0f};
};

// A named per-frame callback over a scene, usually registered by a game
// module. Systems in a phase run by ascending order; equal orders run in
// registration order.
struct SystemDescriptor {
    std::string name;
    SystemPhase phase{SystemPhase::Update};
    RunIn runIn{RunIn::Play};
    int32_t order{0};
    std::function<void(const SystemContext&)> run;
};

struct SystemRegistryResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// The systems a host knows, in registration order. Copyable, so a host can
// start from its own systems and let a module add to a copy.
class SystemRegistry {
  public:
    // Rejects empty or duplicate names and systems without a callback.
    SystemRegistryResult add(SystemDescriptor system);
    const SystemDescriptor* find(std::string_view name) const;
    std::span<const SystemDescriptor> systems() const;

  private:
    std::vector<SystemDescriptor> m_systems;
};

// Runs a registry's systems for one phase and simulation mode at a time. The
// scheduler shares ownership of the registry it was built from; that registry
// must not change afterwards. Build a new scheduler when the set of systems
// changes, for example after a game module reloads.
class SystemScheduler {
  public:
    SystemScheduler() = default;
    explicit SystemScheduler(std::shared_ptr<const SystemRegistry> registry);

    // Runs every system of phase whose RunIn flags include context.mode.
    void run(SystemPhase phase, const SystemContext& context) const;

    // Names of the systems run() would invoke, in invocation order.
    std::vector<std::string> schedule(SystemPhase phase, SimulationMode mode) const;

  private:
    std::shared_ptr<const SystemRegistry> m_registry;
    std::vector<const SystemDescriptor*> m_update;
    std::vector<const SystemDescriptor*> m_fixedUpdate;

    const std::vector<const SystemDescriptor*>& ordered(SystemPhase phase) const;
};
