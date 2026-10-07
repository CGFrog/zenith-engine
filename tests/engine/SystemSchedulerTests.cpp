// CPU-only coverage of the system scheduler's Simulate mode and mask, the
// per-system timing profile, and the simulation clock the engine loop applies
// to fixed steps, animation, and audio.

#include "engine/application/SimulationClock.hpp"
#include "engine/diagnostics/Logger.hpp"
#include "engine/input/InputManager.hpp"
#include "engine/module/SystemScheduler.hpp"
#include "engine/scene/Scene.hpp"

#include "support/Doctest.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

TEST_SUITE_BEGIN("SystemScheduler");

namespace {

bool near(double value, double expected) {
    return std::abs(value - expected) < 1e-9;
}

// A clock the systems advance by a known amount while they run.
struct FakeClock {
    std::chrono::steady_clock::time_point now{};

    void advance(double milliseconds) {
        now += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double, std::milli>(milliseconds));
    }
};

struct Recorded {
    std::vector<std::string> calls;
    std::vector<bool> receivedInput;
};

std::shared_ptr<const SystemRegistry> testRegistry(Recorded& recorded, FakeClock& clock) {
    SystemRegistry registry;
    const auto system = [&](std::string name, SystemPhase phase, RunIn runIn, double milliseconds) {
        registry.add({.name = name,
                      .phase = phase,
                      .runIn = runIn,
                      .run = [&recorded, &clock, name, milliseconds](const SystemContext& context) {
                          recorded.calls.push_back(name);
                          recorded.receivedInput.push_back(context.input != nullptr);
                          clock.advance(milliseconds);
                      }});
    };
    system("player_input", SystemPhase::Update, RunIn::Play, 2.0);
    system("edit_preview", SystemPhase::Update, RunIn::Edit, 1.0);
    system("platforms", SystemPhase::FixedUpdate, RunIn::Play, 3.0);
    system("spinners", SystemPhase::FixedUpdate, RunIn::EditAndPlay, 0.5);
    return std::make_shared<const SystemRegistry>(std::move(registry));
}

TEST_CASE("SystemScheduler: simulate mask") {
    SimulateSystemMask mask;
    CHECK_MESSAGE(
        (mask.enabled("anything", SystemPhase::FixedUpdate) && !mask.enabled("anything", SystemPhase::Update)),
        "Simulate runs FixedUpdate systems and skips Update systems by default");
    mask.set("player_input", true);
    mask.set("platforms", false);
    CHECK_MESSAGE(
        (mask.enabled("player_input", SystemPhase::Update) && !mask.enabled("platforms", SystemPhase::FixedUpdate)),
        "explicit choices override the defaults");
    CHECK_MESSAGE(mask.choices().size() == 2, "only explicit choices are kept");
    mask.clear();
    CHECK_MESSAGE((mask.choices().empty() && mask.enabled("platforms", SystemPhase::FixedUpdate)),
                  "clearing returns every system to its default");
}

TEST_CASE("SystemScheduler: simulate scheduling") {
    Recorded recorded;
    FakeClock clock;
    SystemScheduler scheduler(testRegistry(recorded, clock));
    CHECK_MESSAGE((runsIn(RunIn::Play, SimulationMode::Simulate) && !runsIn(RunIn::Edit, SimulationMode::Simulate)),
                  "Simulate considers the systems that run in Play");
    CHECK_MESSAGE(scheduler.schedule(SystemPhase::Update, SimulationMode::Simulate).empty(),
                  "Simulate runs no Update system by default");
    CHECK_MESSAGE((scheduler.schedule(SystemPhase::FixedUpdate, SimulationMode::Simulate) ==
                   std::vector<std::string>{"platforms", "spinners"}),
                  "Simulate runs every Play FixedUpdate system by default");
    CHECK_MESSAGE(
        (scheduler.schedule(SystemPhase::Update, SimulationMode::Play) == std::vector<std::string>{"player_input"}),
        "the Simulate mask does not affect Play");

    SimulateSystemMask mask;
    mask.set("player_input", true);
    mask.set("platforms", false);
    mask.set("edit_preview", true);
    scheduler.setSimulateMask(mask);
    CHECK_MESSAGE(scheduler.simulateMask() == mask, "the scheduler keeps the mask it is given");
    CHECK_MESSAGE(
        (scheduler.schedule(SystemPhase::Update, SimulationMode::Simulate) == std::vector<std::string>{"player_input"}),
        "an enabled Update system runs in Simulate, an Edit-only one never does");
    CHECK_MESSAGE((scheduler.schedule(SystemPhase::FixedUpdate, SimulationMode::Simulate) ==
                   std::vector<std::string>{"spinners"}),
                  "a disabled FixedUpdate system does not run in Simulate");
    CHECK_MESSAGE(
        (scheduler.systems(SystemPhase::Update).size() == 2 && scheduler.systems(SystemPhase::FixedUpdate).size() == 2),
        "systems() lists every system of a phase whatever its flags");

    Scene scene;
    scheduler.run(SystemPhase::FixedUpdate, {.scene = scene, .mode = SimulationMode::Simulate, .deltaSeconds = 0.016f});
    CHECK_MESSAGE((recorded.calls == std::vector<std::string>{"spinners"}), "run follows the mask in Simulate");
}

TEST_CASE("SystemScheduler: profile") {
    Recorded recorded;
    FakeClock clock;
    const SystemScheduler scheduler(testRegistry(recorded, clock));
    SystemProfile profile(4, [&clock] { return clock.now; });
    Scene scene;
    const auto frame = [&](uint32_t fixedSteps) {
        scheduler.run(SystemPhase::Update, {.scene = scene, .mode = SimulationMode::Play}, &profile);
        for (uint32_t step = 0; step < fixedSteps; ++step) {
            scheduler.run(SystemPhase::FixedUpdate, {.scene = scene, .mode = SimulationMode::Play}, &profile);
        }
        profile.finishFrame();
    };
    CHECK_MESSAGE((profile.timings().empty() && profile.find("platforms") == nullptr),
                  "a profile knows no system before one runs");

    frame(2);
    const SystemTiming* platforms = profile.find("platforms");
    const SystemTiming* input = profile.find("player_input");
    CHECK_MESSAGE((platforms != nullptr && input != nullptr && profile.timings().size() == 3),
                  "every system that ran has a timing, in the order they first ran");
    CHECK_MESSAGE(profile.find("edit_preview") == nullptr, "a system that never ran has no timing");
    if (platforms == nullptr || input == nullptr) {
        return;
    }
    CHECK_MESSAGE((platforms->phase == SystemPhase::FixedUpdate && platforms->frameRuns == 2 &&
                   near(platforms->frameMilliseconds, 6.0)),
                  "a frame's time sums every run of a system");
    CHECK_MESSAGE((near(input->frameMilliseconds, 2.0) && input->frameRuns == 1),
                  "an Update system runs once per frame");
    CHECK_MESSAGE((near(platforms->averageMilliseconds, 6.0) && near(platforms->maximumMilliseconds, 6.0)),
                  "the first frame is the average and the maximum");

    frame(0);
    CHECK_MESSAGE((platforms->frameRuns == 0 && near(platforms->frameMilliseconds, 0.0) &&
                   near(platforms->averageMilliseconds, 3.0) && near(platforms->maximumMilliseconds, 6.0)),
                  "a frame without fixed steps counts as zero in the average and keeps the maximum");

    frame(1);
    frame(1);
    frame(1);
    // The window holds the last four frames: 0, 3, 3, 3.
    CHECK_MESSAGE(near(platforms->averageMilliseconds, 9.0 / 4.0),
                  "the rolling average covers the last averageFrames frames");
    CHECK_MESSAGE(near(platforms->maximumMilliseconds, 6.0), "the maximum covers every frame since the start");

    profile.resetMaxima();
    CHECK_MESSAGE(near(platforms->maximumMilliseconds, 0.0), "resetting clears the maximum");
    frame(1);
    CHECK_MESSAGE(near(platforms->maximumMilliseconds, 3.0), "after a reset the maximum restarts from the next frame");

    profile.clear();
    CHECK_MESSAGE(profile.timings().empty(), "clearing forgets every system");
    CHECK_MESSAGE(SystemProfile(0).averageFrames() == 1, "an average needs at least one frame");
}

TEST_CASE("SystemScheduler: input reaches systems unchanged") {
    Recorded recorded;
    FakeClock clock;
    const SystemScheduler scheduler(testRegistry(recorded, clock));
    Scene scene;
    const InputState input;
    scheduler.run(SystemPhase::Update, {.scene = scene, .mode = SimulationMode::Play, .input = &input});
    CHECK_MESSAGE((recorded.receivedInput == std::vector<bool>{true}),
                  "the scheduler passes the context's input through");
}

// Each phase keeps its own systems and order; Simulate runs only FixedUpdate
// systems unless the mask says otherwise; a phase the engine does not know is
// refused.
TEST_CASE("SystemScheduler: phases") {
    std::vector<std::string> calls;
    auto registry = std::make_shared<SystemRegistry>();
    const auto record = [&calls](std::string name) {
        return [&calls, name](const SystemContext&) { calls.push_back(name); };
    };
    for (const SystemPhase phase : systemPhases) {
        const std::string name(systemPhaseName(phase));
        CHECK_MESSAGE(
            registry->add({.name = name + "_late", .phase = phase, .order = 1, .run = record(name + "_late")}).success,
            "a system registers in every phase");
        CHECK_MESSAGE(
            registry->add({.name = name + "_early", .phase = phase, .order = -1, .run = record(name + "_early")})
                .success,
            "a second system registers in every phase");
    }
    CHECK_MESSAGE(
        !registry->add({.name = "unknown", .phase = static_cast<SystemPhase>(systemPhaseCount), .run = record("x")})
             .success,
        "a system in an unknown phase is refused");
    const SystemScheduler scheduler(registry);
    Scene scene;
    for (const SystemPhase phase : systemPhases) {
        calls.clear();
        scheduler.run(phase, {.scene = scene, .mode = SimulationMode::Play});
        const std::string name(systemPhaseName(phase));
        CHECK_MESSAGE((calls == std::vector<std::string>{name + "_early", name + "_late"}),
                      "a phase runs only its own systems, by order");
        CHECK_MESSAGE(scheduler.systems(phase).size() == 2, "the scheduler lists each phase's systems");
    }
    CHECK_MESSAGE((scheduler.schedule(SystemPhase::LateUpdate, SimulationMode::Simulate).empty() &&
                   scheduler.schedule(SystemPhase::Ui, SimulationMode::Simulate).empty() &&
                   scheduler.schedule(SystemPhase::FixedUpdate, SimulationMode::Simulate).size() == 2),
                  "Simulate runs FixedUpdate systems by default, and no LateUpdate or Ui systems");
    CHECK_MESSAGE(
        (systemPhaseName(SystemPhase::LateUpdate) == "LateUpdate" && systemPhaseName(SystemPhase::Ui) == "Ui"),
        "phases have readable names");
}

// A system that throws is reported once and, with SystemFaults, skipped from
// then on; the systems around it keep running, and clearing the faults lets
// it run again.
TEST_CASE("SystemScheduler: throwing system is disabled") {
    std::vector<std::string> calls;
    bool fail = true;
    auto registry = std::make_shared<SystemRegistry>();
    registry->add(
        {.name = "before", .order = -1, .run = [&calls](const SystemContext&) { calls.push_back("before"); }});
    registry->add({.name = "faulty", .run = [&calls, &fail](const SystemContext&) {
                       calls.push_back("faulty");
                       if (fail) {
                           throw std::runtime_error("faulty system exploded");
                       }
                   }});
    registry->add({.name = "after", .order = 1, .run = [&calls](const SystemContext&) { calls.push_back("after"); }});
    registry->add({.name = "unknown", .phase = SystemPhase::LateUpdate, .run = [](const SystemContext&) { throw 7; }});
    const SystemScheduler scheduler(registry);
    Scene scene;
    Logger logger{LoggerConfig{.echoToStderr = false}};
    const SystemContext context{.scene = scene, .mode = SimulationMode::Play, .logger = &logger};
    SystemFaults faults;
    SystemProfile profile;

    scheduler.run(SystemPhase::Update, context, &profile, &faults);
    CHECK_MESSAGE((calls == std::vector<std::string>{"before", "faulty", "after"}),
                  "the systems after a throwing system still run");
    CHECK_MESSAGE((faults.disabled("faulty") && faults.faults().size() == 1 &&
                   faults.faults()[0].phase == SystemPhase::Update &&
                   faults.faults()[0].what == "faulty system exploded"),
                  "a throwing system is recorded with its phase and what it threw");
    CHECK_MESSAGE(profile.find("faulty") != nullptr, "a throwing system's run time is still recorded");
    const std::vector<LogEntry> logged = logger.entries();
    CHECK_MESSAGE((logged.size() == 1 && logged[0].level == LogLevel::Error && logged[0].category == "gameplay" &&
                   logged[0].message.find("'faulty'") != std::string::npos &&
                   logged[0].message.find("Update") != std::string::npos &&
                   logged[0].message.find("faulty system exploded") != std::string::npos &&
                   logged[0].message.find("disabled") != std::string::npos),
                  "the fault is logged as an error naming the system, its phase, and what it threw");

    calls.clear();
    scheduler.run(SystemPhase::Update, context, &profile, &faults);
    CHECK_MESSAGE((calls == std::vector<std::string>{"before", "after"}), "a disabled system is skipped");
    CHECK_MESSAGE(logger.entries().size() == 1, "a disabled system is reported only once");

    scheduler.run(SystemPhase::LateUpdate, context, nullptr, &faults);
    CHECK_MESSAGE((faults.disabled("unknown") && faults.faults()[1].what == "an unknown exception"),
                  "a system throwing something other than a std::exception is disabled too");

    faults.clear();
    fail = false;
    calls.clear();
    scheduler.run(SystemPhase::Update, context, &profile, &faults);
    CHECK_MESSAGE((calls == std::vector<std::string>{"before", "faulty", "after"} && faults.empty()),
                  "clearing the faults runs the system again");

    // Without faults a throwing system is contained and reported every run.
    fail = true;
    scheduler.run(SystemPhase::Update, context);
    scheduler.run(SystemPhase::Update, context);
    CHECK_MESSAGE(logger.entries().size() == 4, "without faults the system is reported each time it throws");
}

TEST_CASE("SystemScheduler: simulation clock") {
    const SimulationClock realTime;
    CHECK_MESSAGE((simulatedSeconds(realTime, 0.02f) == 0.02f && fixedStepAccumulation(realTime, 0.02f) == 0.02f &&
                   audioPlaybackRate(realTime) == 1.0f),
                  "the default clock runs at wall-clock rate");

    const SimulationClock doubled{.timeScale = 2.0f};
    CHECK_MESSAGE((simulatedSeconds(doubled, 0.02f) == 0.04f && fixedStepAccumulation(doubled, 0.02f) == 0.04f &&
                   audioPlaybackRate(doubled) == 2.0f),
                  "a time scale scales animation, fixed steps, and audio");

    const SimulationClock paused{.timeScale = 0.5f, .paused = true};
    CHECK_MESSAGE((simulatedSeconds(paused, 0.02f) == 0.0f && audioPlaybackRate(paused) == 0.0f),
                  "a paused clock stops animation and audio");
    CHECK_MESSAGE(fixedStepAccumulation(paused, 0.02f) == 0.0f, "a paused clock accumulates no fixed steps");

    // A frame step advances one frame by its own length whatever the scale.
    const SimulationClock stepping{.timeScale = 0.25f, .paused = true, .frameStepSeconds = 1.0f / 60.0f};
    CHECK_MESSAGE((simulatedSeconds(stepping, 0.1f) == 1.0f / 60.0f &&
                   fixedStepAccumulation(stepping, 0.1f) == 1.0f / 60.0f && audioPlaybackRate(stepping) == 1.0f),
                  "a frame step advances animation, fixed steps, and audio by one step");
    const SimulationClock negativeStep{.paused = true, .frameStepSeconds = -1.0f};
    CHECK_MESSAGE((simulatedSeconds(negativeStep, 0.1f) == 0.0f && audioPlaybackRate(negativeStep) == 0.0f),
                  "a negative frame step holds the clock");
    const SimulationClock runningWithStep{.timeScale = 2.0f, .frameStepSeconds = 5.0f};
    CHECK_MESSAGE(simulatedSeconds(runningWithStep, 0.02f) == 0.04f, "a running clock ignores the frame step");

    CHECK_MESSAGE((clampTimeScale(-1.0f) == 0.0f && clampTimeScale(25.0f) == 10.0f && clampTimeScale(0.25f) == 0.25f &&
                   clampTimeScale(std::numeric_limits<float>::quiet_NaN()) == 1.0f),
                  "time scales clamp to [0, 10] and NaN means real time");
    const SimulationClock unclamped{.timeScale = 50.0f};
    CHECK_MESSAGE(simulatedSeconds(unclamped, 1.0f) == 10.0f, "an out-of-range scale is clamped where it is applied");

    // The fixed-step count a frame produces, as Engine::run accumulates it.
    const auto fixedSteps = [](const SimulationClock& clock, int frames) {
        constexpr float fixedStep = 1.0f / 60.0f;
        float accumulator = 0.0f;
        int steps = 0;
        for (int frame = 0; frame < frames; ++frame) {
            accumulator += fixedStepAccumulation(clock, fixedStep);
            while (accumulator >= fixedStep * 0.999f) {
                accumulator -= fixedStep;
                ++steps;
            }
        }
        return steps;
    };
    CHECK_MESSAGE(fixedSteps(realTime, 60) == 60, "real time runs one fixed step per 60 Hz frame");
    CHECK_MESSAGE(fixedSteps(SimulationClock{.timeScale = 0.5f}, 60) == 30, "half speed runs half the fixed steps");
    CHECK_MESSAGE(fixedSteps(doubled, 60) == 120, "double speed runs twice the fixed steps");
    CHECK_MESSAGE(fixedSteps(SimulationClock{.timeScale = 0.0f}, 60) == 0, "a zero time scale runs no fixed steps");
    CHECK_MESSAGE(fixedSteps(stepping, 1) == 1, "a frame step of one fixed time step runs exactly one fixed step");
    CHECK_MESSAGE(fixedSteps(paused, 60) == 0, "a paused clock runs no fixed steps");
}
} // namespace

TEST_SUITE_END();
