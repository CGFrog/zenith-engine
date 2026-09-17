#include "Engine.hpp"

#include <algorithm>
#include <iostream>

bool Engine::init(const PhysicsWorldConfig& physicsConfig) {
    if (!m_window.init()) {
        return false;
    }

    if (!m_renderer.init(m_window)) {
        return false;
    }
    if (const PhysicsResult result = m_physics.init(physicsConfig); !result) {
        std::cerr << "Physics initialization failed: " << result.error << '\n';
        return false;
    }
    return true;
}

bool Engine::run() {
    bool running = true;
    uint64_t renderedFrames = 0;
    const uint64_t frameLimit = readFrameLimit();

    m_input.initialize(m_window, frameLimit == 0);

    uint64_t previousTicks = SDL_GetTicksNS();
    uint64_t fpsStartTicks = previousTicks;
    uint64_t fpsFrames = 0;
    double maxFps = 0.0;
    m_jumpQueued = false;
    while (running) {
        m_input.update(m_window);
        m_jumpQueued = m_jumpQueued || m_input.state().jumpPressed;

        if (m_input.resizeRequested()) {
            m_renderer.requestResize();
        }

        if (m_input.quitRequested()) {
            break;
        }

        const uint64_t now = SDL_GetTicksNS();
        const float deltaSeconds = std::min(static_cast<float>(now - previousTicks) / 1'000'000'000.0f, 0.1f);
        previousTicks = now;
        m_firstPersonControllers.updateView(m_scene, m_input.state());
        m_physicsAccumulator += deltaSeconds;
        const float fixedStep = m_physics.config().fixedTimeStep;
        uint32_t subSteps = 0;
        InputState fixedInput = m_input.state();
        fixedInput.jumpPressed = m_jumpQueued;
        while (m_physicsAccumulator >= fixedStep && subSteps < m_physics.config().maxSubSteps) {
            m_firstPersonControllers.fixedUpdate(m_scene, m_physics, fixedInput, fixedStep);
            if (const PhysicsResult result = m_physics.step(m_scene, m_assets, fixedStep); !result) {
                std::cerr << "Physics update failed: " << result.error << '\n';
                return false;
            }
            m_jumpQueued = false;
            fixedInput.jumpPressed = false;
            m_physicsAccumulator -= fixedStep;
            ++subSteps;
        }
        if (subSteps == m_physics.config().maxSubSteps) {
            m_physicsAccumulator = std::min(m_physicsAccumulator, fixedStep);
        }
        m_physics.interpolate(m_scene, m_physicsAccumulator / fixedStep);

        if (!m_renderer.drawFrame(m_window, m_scene, m_assets)) {
            return false;
        }

        ++fpsFrames;
        const uint64_t frameEndTicks = SDL_GetTicksNS();
        const uint64_t fpsElapsedTicks = frameEndTicks - fpsStartTicks;
        if (fpsElapsedTicks >= 1'000'000'000) {
            const double fps = static_cast<double>(fpsFrames) * 1'000'000'000.0 /
                               static_cast<double>(fpsElapsedTicks);
            maxFps = std::max(maxFps, fps);
            char title[96];
            SDL_snprintf(title, sizeof(title), "Zenith | %.1f FPS | Max %.1f | %s", fps, maxFps,
                         m_renderer.presentModeName());
            SDL_SetWindowTitle(m_window.handle(), title);
            fpsStartTicks = frameEndTicks;
            fpsFrames = 0;
        }

        if (frameLimit != 0 && ++renderedFrames >= frameLimit) {
            running = false;
        }
    }
    return true;
}

void Engine::shutdown() {
    m_physics.shutdown();
    m_renderer.shutdown();
    m_window.shutdown();
}

Scene& Engine::scene() {
    return m_scene;
}

AssetManager& Engine::assets() {
    return m_assets;
}

PhysicsWorld& Engine::physics() {
    return m_physics;
}

uint64_t Engine::readFrameLimit() {
    const char* value = SDL_getenv("ZENITH_SMOKE_FRAMES");
    return value == nullptr ? 0 : SDL_strtoull(value, nullptr, 10);
}
