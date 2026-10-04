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
    // Stops animation, audio, and fixed-step accumulation, except for a frame
    // step (frameStepSeconds).
    bool paused{false};
    // While paused: the simulated seconds this one frame advances by (a frame
    // step). Animation and fixed-step accumulation advance by it whatever the
    // time scale, and audio plays at real-time rate for the frame; the scene
    // enables fixed updates for the frame so the steps it accumulates run.
    // Zero (the default) holds a paused simulation still. Ignored while
    // running.
    float frameStepSeconds{0.0f};
};

// Clamps into [minimumTimeScale, maximumTimeScale]; NaN becomes 1.
float clampTimeScale(float scale);

// Seconds of simulated time that frameSeconds of wall-clock time advances
// animation and Update systems by: while paused only the frame step (zero
// without one), otherwise scaled.
float simulatedSeconds(const SimulationClock& clock, float frameSeconds);

// Seconds a frame adds to the fixed-step accumulator while the scene has
// fixed updates enabled; the same as simulatedSeconds, so a frame step of one
// fixed time step from an empty accumulator runs exactly one fixed step.
float fixedStepAccumulation(const SimulationClock& clock, float frameSeconds);

// The playback rate for audio: the time scale while running; while paused,
// real time (1) for a frame step and zero otherwise.
float audioPlaybackRate(const SimulationClock& clock);
