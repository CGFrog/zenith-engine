// CPU-only coverage of the system scheduler's Simulate mode and mask, the
// per-system timing profile, and the simulation clock the engine loop applies
// to fixed steps, animation, and audio.

#include "engine/application/SimulationClock.hpp"
#include "engine/input/InputManager.hpp"
#include "engine/module/SystemScheduler.hpp"
#include "engine/scene/Scene.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

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

void testSimulateMask() {
    SimulateSystemMask mask;
    expect(mask.enabled("anything", SystemPhase::FixedUpdate) && !mask.enabled("anything", SystemPhase::Update),
           "Simulate runs FixedUpdate systems and skips Update systems by default");
    mask.set("player_input", true);
    mask.set("platforms", false);
    expect(mask.enabled("player_input", SystemPhase::Update) && !mask.enabled("platforms", SystemPhase::FixedUpdate),
           "explicit choices override the defaults");
    expect(mask.choices().size() == 2, "only explicit choices are kept");
    mask.clear();
    expect(mask.choices().empty() && mask.enabled("platforms", SystemPhase::FixedUpdate),
           "clearing returns every system to its default");
}

void testSimulateScheduling() {
    Recorded recorded;
    FakeClock clock;
    SystemScheduler scheduler(testRegistry(recorded, clock));
    expect(runsIn(RunIn::Play, SimulationMode::Simulate) && !runsIn(RunIn::Edit, SimulationMode::Simulate),
           "Simulate considers the systems that run in Play");
    expect(scheduler.schedule(SystemPhase::Update, SimulationMode::Simulate).empty(),
           "Simulate runs no Update system by default");
    expect(scheduler.schedule(SystemPhase::FixedUpdate, SimulationMode::Simulate) ==
               std::vector<std::string>{"platforms", "spinners"},
           "Simulate runs every Play FixedUpdate system by default");
    expect(scheduler.schedule(SystemPhase::Update, SimulationMode::Play) == std::vector<std::string>{"player_input"},
           "the Simulate mask does not affect Play");

    SimulateSystemMask mask;
    mask.set("player_input", true);
    mask.set("platforms", false);
    mask.set("edit_preview", true);
    scheduler.setSimulateMask(mask);
    expect(scheduler.simulateMask() == mask, "the scheduler keeps the mask it is given");
    expect(scheduler.schedule(SystemPhase::Update, SimulationMode::Simulate) ==
               std::vector<std::string>{"player_input"},
           "an enabled Update system runs in Simulate, an Edit-only one never does");
    expect(scheduler.schedule(SystemPhase::FixedUpdate, SimulationMode::Simulate) ==
               std::vector<std::string>{"spinners"},
           "a disabled FixedUpdate system does not run in Simulate");
    expect(scheduler.systems(SystemPhase::Update).size() == 2 &&
               scheduler.systems(SystemPhase::FixedUpdate).size() == 2,
           "systems() lists every system of a phase whatever its flags");

    Scene scene;
    scheduler.run(SystemPhase::FixedUpdate, {.scene = scene, .mode = SimulationMode::Simulate, .deltaSeconds = 0.016f});
    expect(recorded.calls == std::vector<std::string>{"spinners"}, "run follows the mask in Simulate");
}

void testProfile() {
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
    expect(profile.timings().empty() && profile.find("platforms") == nullptr,
           "a profile knows no system before one runs");

    frame(2);
    const SystemTiming* platforms = profile.find("platforms");
    const SystemTiming* input = profile.find("player_input");
    expect(platforms != nullptr && input != nullptr && profile.timings().size() == 3,
           "every system that ran has a timing, in the order they first ran");
    expect(profile.find("edit_preview") == nullptr, "a system that never ran has no timing");
    if (platforms == nullptr || input == nullptr) {
        return;
    }
    expect(platforms->phase == SystemPhase::FixedUpdate && platforms->frameRuns == 2 &&
               near(platforms->frameMilliseconds, 6.0),
           "a frame's time sums every run of a system");
    expect(near(input->frameMilliseconds, 2.0) && input->frameRuns == 1, "an Update system runs once per frame");
    expect(near(platforms->averageMilliseconds, 6.0) && near(platforms->maximumMilliseconds, 6.0),
           "the first frame is the average and the maximum");

    frame(0);
    expect(platforms->frameRuns == 0 && near(platforms->frameMilliseconds, 0.0) &&
               near(platforms->averageMilliseconds, 3.0) && near(platforms->maximumMilliseconds, 6.0),
           "a frame without fixed steps counts as zero in the average and keeps the maximum");

    frame(1);
    frame(1);
    frame(1);
    // The window holds the last four frames: 0, 3, 3, 3.
    expect(near(platforms->averageMilliseconds, 9.0 / 4.0), "the rolling average covers the last averageFrames frames");
    expect(near(platforms->maximumMilliseconds, 6.0), "the maximum covers every frame since the start");

    profile.resetMaxima();
    expect(near(platforms->maximumMilliseconds, 0.0), "resetting clears the maximum");
    frame(1);
    expect(near(platforms->maximumMilliseconds, 3.0), "after a reset the maximum restarts from the next frame");

    profile.clear();
    expect(profile.timings().empty(), "clearing forgets every system");
    expect(SystemProfile(0).averageFrames() == 1, "an average needs at least one frame");
}

void testInputReachesSystemsUnchanged() {
    Recorded recorded;
    FakeClock clock;
    const SystemScheduler scheduler(testRegistry(recorded, clock));
    Scene scene;
    const InputState input;
    scheduler.run(SystemPhase::Update, {.scene = scene, .mode = SimulationMode::Play, .input = &input});
    expect(recorded.receivedInput == std::vector<bool>{true}, "the scheduler passes the context's input through");
}

void testSimulationClock() {
    const SimulationClock realTime;
    expect(simulatedSeconds(realTime, 0.02f) == 0.02f && fixedStepAccumulation(realTime, 0.02f) == 0.02f &&
               audioPlaybackRate(realTime) == 1.0f,
           "the default clock runs at wall-clock rate");

    const SimulationClock doubled{.timeScale = 2.0f};
    expect(simulatedSeconds(doubled, 0.02f) == 0.04f && fixedStepAccumulation(doubled, 0.02f) == 0.04f &&
               audioPlaybackRate(doubled) == 2.0f,
           "a time scale scales animation, fixed steps, and audio");

    const SimulationClock paused{.timeScale = 0.5f, .paused = true};
    expect(simulatedSeconds(paused, 0.02f) == 0.0f && audioPlaybackRate(paused) == 0.0f,
           "a paused clock stops animation and audio");
    expect(fixedStepAccumulation(paused, 0.02f) == 0.02f,
           "a requested single step accumulates at wall-clock rate whatever the scale");

    expect(clampTimeScale(-1.0f) == 0.0f && clampTimeScale(25.0f) == 10.0f && clampTimeScale(0.25f) == 0.25f &&
               clampTimeScale(std::numeric_limits<float>::quiet_NaN()) == 1.0f,
           "time scales clamp to [0, 10] and NaN means real time");
    const SimulationClock unclamped{.timeScale = 50.0f};
    expect(simulatedSeconds(unclamped, 1.0f) == 10.0f, "an out-of-range scale is clamped where it is applied");

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
    expect(fixedSteps(realTime, 60) == 60, "real time runs one fixed step per 60 Hz frame");
    expect(fixedSteps(SimulationClock{.timeScale = 0.5f}, 60) == 30, "half speed runs half the fixed steps");
    expect(fixedSteps(doubled, 60) == 120, "double speed runs twice the fixed steps");
    expect(fixedSteps(SimulationClock{.timeScale = 0.0f}, 60) == 0, "a zero time scale runs no fixed steps");
}
} // namespace

int main() {
    testSimulateMask();
    testSimulateScheduling();
    testProfile();
    testInputReachesSystemsUnchanged();
    testSimulationClock();
    return failures == 0 ? 0 : 1;
}
