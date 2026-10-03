#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class Scene;
struct ApplicationContext;
struct InputState;

// Whether the host is authoring a scene (the editor with simulation stopped),
// playing it (a game, or the editor's Play mode), or simulating it (the
// editor's Simulate mode: physics and the systems the user chose run on a
// copy of the scene while the editor keeps its own camera and tools, and no
// player input reaches the scene).
enum class SimulationMode {
    Edit,
    Play,
    Simulate,
};

// The simulation modes a system runs in; combine with |. There is no
// Simulate flag: Simulate runs the Play systems its SimulateSystemMask
// enables.
enum class RunIn : uint32_t {
    Edit = 1u << 0,
    Play = 1u << 1,
    EditAndPlay = Edit | Play,
};

constexpr RunIn operator|(RunIn left, RunIn right) {
    return static_cast<RunIn>(static_cast<uint32_t>(left) | static_cast<uint32_t>(right));
}

// Whether flags admit mode before any Simulate mask is applied: Simulate
// considers the systems that run in Play.
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
// has no running application services, as in the editor's edit mode. input is
// the device state of the current frame, or null when the host routes no
// player input to the scene (for example while an editor viewport is not
// focused, and always in Simulate); systems that read it must tolerate null.
struct SystemContext {
    Scene& scene;
    ApplicationContext* application{nullptr};
    SimulationMode mode{SimulationMode::Play};
    // Seconds; the frame duration for Update, the fixed step for FixedUpdate.
    float deltaSeconds{0.0f};
    const InputState* input{nullptr};
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

// Which Play systems Simulate runs, by system name. A system the mask does
// not name uses the default: FixedUpdate systems run (they advance the
// simulated world in step with physics), Update systems do not (they usually
// react to the player: input, cameras, menus). A plain value, so a host can
// keep it across module reloads and save it; names of systems that no longer
// exist are kept and ignored.
class SimulateSystemMask {
  public:
    static bool enabledByDefault(SystemPhase phase);

    bool enabled(std::string_view name, SystemPhase phase) const;
    // Records an explicit choice for the named system.
    void set(std::string name, bool enabled);
    // Forgets every explicit choice.
    void clear();
    // The explicit choices, by name.
    const std::map<std::string, bool, std::less<>>& choices() const;

    bool operator==(const SimulateSystemMask&) const = default;

  private:
    std::map<std::string, bool, std::less<>> m_choices;
};

// CPU time of one system, in milliseconds, as SystemProfile measured it.
struct SystemTiming {
    std::string name;
    SystemPhase phase{SystemPhase::Update};
    // The last finished frame: every run summed, and how many runs (a
    // FixedUpdate system runs zero or more times per frame).
    double frameMilliseconds{0.0};
    uint32_t frameRuns{0};
    // Mean frameMilliseconds over the last averageFrames finished frames
    // (fewer until that many have finished), counting frames it did not run.
    double averageMilliseconds{0.0};
    // The largest frameMilliseconds since the profile started or was reset.
    double maximumMilliseconds{0.0};
};

// Per-system CPU timings that SystemScheduler::run records when given a
// profile. The owner calls finishFrame once per rendered frame after the
// frame's systems ran. Not thread safe; use from the thread that runs the
// systems.
class SystemProfile {
  public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    // clock defaults to std::chrono::steady_clock::now; tests pass a fake.
    // averageFrames is clamped to at least 1.
    explicit SystemProfile(uint32_t averageFrames = 60, Clock clock = {});

    std::chrono::steady_clock::time_point now() const;
    // Adds one run of the named system to the current frame.
    void record(const SystemDescriptor& system, std::chrono::nanoseconds duration);
    // Publishes the current frame into timings() and starts the next one.
    void finishFrame();
    // Restarts every maximum from the next finished frame.
    void resetMaxima();
    // Forgets every system, for example when the set of systems changes.
    void clear();

    // One entry per system that has run since the last clear, in the order
    // they first ran.
    std::span<const SystemTiming> timings() const;
    const SystemTiming* find(std::string_view name) const;
    uint32_t averageFrames() const;

  private:
    struct History {
        double currentMilliseconds{0.0};
        uint32_t currentRuns{0};
        // The last averageFrames finished frames, as a ring.
        std::vector<double> frames;
        size_t next{0};
        bool maximumReset{false};
    };

    uint32_t m_averageFrames;
    Clock m_clock;
    std::vector<SystemTiming> m_timings;
    // Parallel to m_timings.
    std::vector<History> m_history;
    std::unordered_map<std::string, size_t> m_index;
};

// Runs a registry's systems for one phase and simulation mode at a time. The
// scheduler shares ownership of the registry it was built from; that registry
// must not change afterwards. Build a new scheduler when the set of systems
// changes, for example after a game module reloads.
class SystemScheduler {
  public:
    SystemScheduler() = default;
    explicit SystemScheduler(std::shared_ptr<const SystemRegistry> registry);

    // Runs every system of phase that runs in context.mode: its RunIn flags
    // admit the mode and, in Simulate, the Simulate mask enables it. When
    // profile is not null each system's run time is recorded into it.
    void run(SystemPhase phase, const SystemContext& context, SystemProfile* profile = nullptr) const;

    // Names of the systems run() would invoke, in invocation order.
    std::vector<std::string> schedule(SystemPhase phase, SimulationMode mode) const;
    // Every system of phase in invocation order, whatever its RunIn flags.
    std::span<const SystemDescriptor* const> systems(SystemPhase phase) const;

    void setSimulateMask(SimulateSystemMask mask);
    const SimulateSystemMask& simulateMask() const;

  private:
    bool runs(const SystemDescriptor& system, SimulationMode mode) const;

    std::shared_ptr<const SystemRegistry> m_registry;
    std::vector<const SystemDescriptor*> m_update;
    std::vector<const SystemDescriptor*> m_fixedUpdate;
    SimulateSystemMask m_simulateMask;

    const std::vector<const SystemDescriptor*>& ordered(SystemPhase phase) const;
};
