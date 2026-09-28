#include "Engine.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <vector>

bool Engine::init(const PhysicsWorldConfig& physicsConfig, const AudioConfig& audioConfig) {
    if (!m_window.init()) {
        return false;
    }

    if (!m_renderer.init(m_window, m_rendering)) {
        return false;
    }
    if (const PhysicsResult result = m_physics.init(physicsConfig); !result) {
        std::cerr << "Physics initialization failed: " << result.error << '\n';
        return false;
    }
    if (const AudioResult result = m_audio.init(audioConfig); !result) {
        std::cerr << "Audio initialization failed: " << result.error << '\n';
        return false;
    }
    return true;
}

bool Engine::run(Application& application) {
    bool running = true;
    bool runSucceeded = true;
    uint64_t renderedFrames = 0;
    const uint64_t benchmarkFrames = readBenchmarkFrameCount();
    constexpr uint64_t benchmarkWarmupFrames = 1000;
    const uint64_t frameLimit = benchmarkFrames != 0 ? benchmarkWarmupFrames + benchmarkFrames : readFrameLimit();
    std::vector<double> benchmarkFrameTimes;
    std::vector<double> benchmarkGpuTimes;
    benchmarkFrameTimes.reserve(static_cast<size_t>(benchmarkFrames));
    benchmarkGpuTimes.reserve(static_cast<size_t>(benchmarkFrames));

    ApplicationSession applicationSession(application, m_scene, m_assets, m_physics, m_audio, m_rendering, m_gameUi);
    if (const SceneResult result = applicationSession.start(); !result) {
        std::cerr << "Application startup failed: " << result.error << '\n';
        return false;
    }

    m_input.initialize(m_window, false);
    m_physicsAccumulator = 0.0f;

    uint64_t previousTicks = SDL_GetTicksNS();
    uint64_t fpsStartTicks = previousTicks;
    uint64_t fpsFrames = 0;
    double maxFps = 0.0;
    while (running) {
        const uint64_t frameBeginTicks = SDL_GetTicksNS();
        float deltaSeconds = 0.0f;
        m_input.update(
            m_window, [this](const SDL_Event& event) { m_renderer.processUiEvent(event); }, m_renderer.uiInputCapture(),
            application.mouseCapturePolicy());

        if (m_input.resizeRequested()) {
            m_renderer.requestResize();
        }

        if (m_input.quitRequested()) {
            applicationSession.requestQuit();
        }

        if (benchmarkFrames == 0) {
            const uint64_t now = SDL_GetTicksNS();
            deltaSeconds = std::min(static_cast<float>(now - previousTicks) / 1'000'000'000.0f, 0.1f);
            previousTicks = now;
            applicationSession.update(m_input.state(), deltaSeconds);
            updateAnimations(m_scene, m_assets, deltaSeconds);
            if (applicationSession.fixedUpdatesEnabled()) {
                m_physicsAccumulator += deltaSeconds;
            } else {
                m_physicsAccumulator = 0.0f;
            }
            const float fixedStep = m_physics.config().fixedTimeStep;
            uint32_t subSteps = 0;
            while (m_physicsAccumulator >= fixedStep && subSteps < m_physics.config().maxSubSteps &&
                   applicationSession.beginFixedUpdate() && !applicationSession.actionPending()) {
                const InputState fixedInput = m_input.fixedState();
                applicationSession.fixedUpdate(fixedInput, fixedStep);
                if (applicationSession.actionPending()) {
                    break;
                }
                if (const PhysicsResult result = m_physics.step(m_scene, m_assets, fixedStep); !result) {
                    std::cerr << "Physics update failed: " << result.error << '\n';
                    runSucceeded = false;
                    running = false;
                    break;
                }
                m_input.consumeFixedPresses();
                m_physicsAccumulator -= fixedStep;
                ++subSteps;
            }
            if (!running) {
                break;
            }
            if (subSteps == m_physics.config().maxSubSteps) {
                m_physicsAccumulator = std::min(m_physicsAccumulator, fixedStep);
            }
            m_physics.interpolate(m_scene, m_physicsAccumulator / fixedStep);
        }

        m_scene.updateWorldTransforms();
        m_renderer.beginUiFrame(m_assets);
        int uiWidth = 1;
        int uiHeight = 1;
        SDL_GetWindowSizeInPixels(m_window.handle(), &uiWidth, &uiHeight);
        m_gameUi.beginFrame({static_cast<uint32_t>(std::max(uiWidth, 1)), static_cast<uint32_t>(std::max(uiHeight, 1))},
                            m_input.state());
        applicationSession.drawUi();
        m_renderer.drawGameUi(m_gameUi, m_assets);
        if (!m_renderer.drawFrame(m_window, m_scene, m_assets)) {
            runSucceeded = false;
            running = false;
            break;
        }
        // Rendering and audio consume the same finalized transform hierarchy.
        m_audio.syncScene(m_scene);
        m_audio.update(deltaSeconds);

        ++fpsFrames;
        const uint64_t frameEndTicks = SDL_GetTicksNS();
        if (benchmarkFrames != 0 && renderedFrames >= benchmarkWarmupFrames) {
            benchmarkFrameTimes.push_back(static_cast<double>(frameEndTicks - frameBeginTicks) / 1'000'000.0);
            benchmarkGpuTimes.push_back(m_rendering.stats().gpuFrameMilliseconds);
        }
        const uint64_t fpsElapsedTicks = frameEndTicks - fpsStartTicks;
        if (fpsElapsedTicks >= 1'000'000'000) {
            const double fps = static_cast<double>(fpsFrames) * 1'000'000'000.0 / static_cast<double>(fpsElapsedTicks);
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

        const bool lifecycleActionPending = applicationSession.actionPending();
        const ApplicationFrameResult frameResult = applicationSession.finishFrame();
        if (!frameResult) {
            std::cerr << "Application scene transition failed: " << frameResult.error << '\n';
            applicationSession.shutdown();
            return false;
        }
        if (!frameResult.keepRunning) {
            running = false;
        } else if (lifecycleActionPending) {
            m_physicsAccumulator = 0.0f;
            m_input.clearBufferedInput();
            previousTicks = SDL_GetTicksNS();
        }
    }
    applicationSession.shutdown();
    if (!benchmarkFrameTimes.empty()) {
        std::sort(benchmarkFrameTimes.begin(), benchmarkFrameTimes.end());
        std::sort(benchmarkGpuTimes.begin(), benchmarkGpuTimes.end());
        const size_t middle = benchmarkFrameTimes.size() / 2;
        const double median = benchmarkFrameTimes.size() % 2 == 0
                                  ? (benchmarkFrameTimes[middle - 1] + benchmarkFrameTimes[middle]) * 0.5
                                  : benchmarkFrameTimes[middle];
        const size_t p95Index = static_cast<size_t>(std::ceil(benchmarkFrameTimes.size() * 0.95)) - 1;
        const double p95 = benchmarkFrameTimes[std::min(p95Index, benchmarkFrameTimes.size() - 1)];
        const double gpuMedian = benchmarkGpuTimes[benchmarkGpuTimes.size() / 2];
        const double gpuP95 = benchmarkGpuTimes[std::min(p95Index, benchmarkGpuTimes.size() - 1)];
        const RenderStats& stats = m_rendering.stats();
        std::cout << std::fixed << std::setprecision(6)
                  << "{\"metric\":\"render.frame_time\",\"unit\":\"ms\",\"frames\":" << benchmarkFrameTimes.size()
                  << ",\"median\":" << median << ",\"p95\":" << p95 << ",\"median_fps\":" << (1000.0 / median)
                  << ",\"gpu_median\":" << gpuMedian << ",\"gpu_p95\":" << gpuP95
                  << ",\"render_packet_ms\":" << stats.cpuRenderPacketMilliseconds
                  << ",\"draw_calls\":" << stats.drawCalls << ",\"visible\":" << stats.visibleCount
                  << ",\"culled\":" << stats.culledCount << ",\"triangles\":" << stats.triangleCount
                  << ",\"present_mode\":\"" << m_renderer.presentModeName() << "\"}\n";
    }
    return runSucceeded;
}

void Engine::shutdown() {
    m_audio.shutdown();
    m_physics.shutdown();
    m_renderer.shutdown();
    m_input.shutdown();
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

AudioSystem& Engine::audio() {
    return m_audio;
}

RenderRuntime& Engine::rendering() {
    return m_rendering;
}

uint64_t Engine::readFrameLimit() {
    const char* value = SDL_getenv("ZENITH_SMOKE_FRAMES");
    return value == nullptr ? 0 : SDL_strtoull(value, nullptr, 10);
}

uint64_t Engine::readBenchmarkFrameCount() {
    const char* value = SDL_getenv("ZENITH_BENCHMARK_FRAMES");
    return value == nullptr ? 0 : SDL_strtoull(value, nullptr, 10);
}
