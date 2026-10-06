#pragma once

// CPU milliseconds the engine loop spent in each phase of one frame, measured on the main thread with a steady clock.

#include <cstdint>

// CPU milliseconds the engine loop spent in each phase of one frame, measured
// on the main thread with a steady clock. Engine::run fills it every frame
// (benchmark mode additionally samples the same values into its report) and
// shares the last completed frame with the application through
// ApplicationContext::frameTimings.
struct EngineFrameTimings {
    // The frame these values describe, counting from 1; 0 before the first.
    uint64_t frame{0};
    double totalMilliseconds{0.0};
    double inputMilliseconds{0.0};
    // ApplicationScene::update, including the systems a scene runs there.
    double updateMilliseconds{0.0};
    double animationMilliseconds{0.0};
    // Every fixed step: ApplicationScene::fixedUpdate plus the physics step.
    double fixedUpdateMilliseconds{0.0};
    // The physics steps alone.
    double physicsMilliseconds{0.0};
    double transformsMilliseconds{0.0};
    double uiBuildMilliseconds{0.0};
    double rendererMilliseconds{0.0};
    double audioMilliseconds{0.0};
    uint32_t fixedSteps{0};
};
