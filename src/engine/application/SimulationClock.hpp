#pragma once

// How fast an application's simulation runs relative to wall-clock time. The
// engine reads it from the active scene each frame, after update, and applies it to the
// work that advances the simulated world: fixed-step accumulation (and so
// physics and FixedUpdate systems), skeletal animation, and audio playback.
// The frame time passed to ApplicationScene::update stays unscaled, because
// tools and cameras run on wall-clock time; a scene applies the scale to its
// own simulation work (the editor scales its Update systems).
struct SimulationClock {
    static constexpr float minimumTimeScale = 0.0f;
    static constexpr float maximumTimeScale = 10.0f;

    // Multiplier on simulated time; clampTimeScale keeps it within
    // [minimumTimeScale, maximumTimeScale].
    float timeScale{1.0f};
    // Stops animation and audio and fixed-step accumulation, except for
    // single steps the scene requests through fixedUpdatesEnabled().
    bool paused{false};
};

// Clamps into [minimumTimeScale, maximumTimeScale]; NaN becomes 1.
float clampTimeScale(float scale);

// Seconds of simulated time that frameSeconds of wall-clock time advances
// animation and Update systems by: zero while paused, otherwise scaled.
float simulatedSeconds(const SimulationClock& clock, float frameSeconds);

// Seconds a frame adds to the fixed-step accumulator while the scene has
// fixed updates enabled. Paused clocks only accumulate for a requested single
// step, which advances at wall-clock rate so it completes at any time scale.
float fixedStepAccumulation(const SimulationClock& clock, float frameSeconds);

// The playback rate for audio: zero while paused, otherwise the time scale.
float audioPlaybackRate(const SimulationClock& clock);
