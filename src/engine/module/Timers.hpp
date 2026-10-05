#pragma once

#include "engine/scene/SceneEntityId.hpp"

#include <cstdint>
#include <functional>
#include <vector>

class LogChannel;
class Scene;

struct TimerHandle {
    uint64_t value{0};

    explicit operator bool() const {
        return value != 0;
    }

    bool operator==(const TimerHandle&) const = default;
};

enum class TimerRepeat : uint8_t {
    Once,
    Repeat,
};

// What a timer callback receives.
struct TimerContext {
    Scene& scene;
    TimerHandle timer;
    // The entity the timer belongs to, or zero.
    SceneEntityId owner;
};

// Callbacks that run after an amount of simulated time. Time only advances
// through advance, which the host calls with the simulation clock's scaled
// seconds, so timers follow time scale and stop while the game is paused.
//
// Lifetime: callbacks usually hold game module code, so the owner (see
// GameServices) must be destroyed before the module unloads.
class Timers {
  public:
    using Callback = std::function<void(TimerContext&)>;

    // Runs callback after seconds of simulated time, then (for Repeat) every
    // seconds after that. A timer with an owner is dropped without running
    // once its owner no longer exists in the scene. Returns a null handle for
    // a missing callback or a Repeat interval that is not positive.
    TimerHandle start(float seconds, TimerRepeat repeat, Callback callback, SceneEntityId owner = {});
    // Whether the timer existed (and had not finished).
    bool cancel(TimerHandle timer);
    bool active(TimerHandle timer) const;

    // Advances simulated time and runs every timer that falls due, earliest
    // first (equal deadlines in start order). A repeating timer runs once per
    // elapsed interval. Callbacks may start and cancel timers; a timer started
    // with a deadline inside this advance also runs in it. A callback that
    // throws is logged as an error to log (stderr when it has no logger) and
    // its timer is dropped, even a repeating one; the other timers still run.
    void advance(Scene& scene, float simulatedSeconds, const LogChannel& log);
    void clear();
    size_t size() const;

  private:
    struct Timer {
        TimerHandle handle;
        double deadline{0.0};
        double interval{0.0};
        TimerRepeat repeat{TimerRepeat::Once};
        SceneEntityId owner;
        Callback callback;
    };

    // The index of the earliest timer due by m_now, or size when none is due.
    size_t nextDue() const;

    double m_now{0.0};
    uint64_t m_nextHandle{1};
    std::vector<Timer> m_timers;
};
