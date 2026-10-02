#include "Engine.hpp"
#include "diagnostics/Benchmark.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {
enum class BenchmarkMode {
    Disabled,
    RenderOnly,
    FullRuntime
};

struct BenchmarkConfiguration {
    BenchmarkMode mode{BenchmarkMode::Disabled};
    uint64_t frames{0};
    uint64_t warmupFrames{120};
    double frameBudgetMilliseconds{0.0};
    bool includeSamples{false};
};

uint64_t environmentUnsigned(const char* name, uint64_t fallback) {
    const char* value = SDL_getenv(name);
    if (!value || *value == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    return end != value && *end == '\0' ? static_cast<uint64_t>(parsed) : fallback;
}

double environmentDouble(const char* name, double fallback) {
    const char* value = SDL_getenv(name);
    if (!value || *value == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const double parsed = std::strtod(value, &end);
    return end != value && *end == '\0' && parsed >= 0.0 ? parsed : fallback;
}

BenchmarkConfiguration benchmarkConfiguration() {
    BenchmarkConfiguration result;
    result.frames = environmentUnsigned("ZENITH_BENCHMARK_FRAMES", 0);
    if (result.frames == 0) {
        return result;
    }
    result.warmupFrames = environmentUnsigned("ZENITH_BENCHMARK_WARMUP_FRAMES", 120);
    result.frameBudgetMilliseconds = environmentDouble("ZENITH_BENCHMARK_FRAME_BUDGET_MS", 0.0);
    result.includeSamples = environmentUnsigned("ZENITH_BENCHMARK_SAMPLES", 0) != 0;
    const char* mode = SDL_getenv("ZENITH_BENCHMARK_MODE");
    result.mode = mode && std::string_view(mode) == "render" ? BenchmarkMode::RenderOnly : BenchmarkMode::FullRuntime;
    return result;
}

double elapsedMilliseconds(std::chrono::steady_clock::time_point begin) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
}

FrameTaskGraphConfig frameTaskConfiguration() {
    return {.workerCount = static_cast<size_t>(environmentUnsigned("ZENITH_FRAME_WORKERS", 0)),
            .minimumGrainSize = static_cast<size_t>(environmentUnsigned("ZENITH_FRAME_GRAIN", 128)),
            .deterministic = environmentUnsigned("ZENITH_FRAME_DETERMINISTIC", 0) != 0};
}
} // namespace

Engine::Engine()
    : m_frameTasks(frameTaskConfiguration()) {}

bool Engine::init(const PhysicsWorldConfig& physicsConfig, const AudioConfig& audioConfig) {
    if (!m_window.init()) {
        return false;
    }

    if (!m_renderer.init(m_window, m_rendering, m_frameTasks)) {
        return false;
    }
    m_assets.setTextureFormatCapabilities(m_renderer.textureFormatCapabilities());
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
    const BenchmarkConfiguration benchmark = benchmarkConfiguration();
    const bool benchmarkEnabled = benchmark.mode != BenchmarkMode::Disabled;
    const uint64_t frameLimit = benchmarkEnabled ? benchmark.warmupFrames + benchmark.frames : readFrameLimit();
    BenchmarkReport benchmarkReport;
    addBuildMetadata(benchmarkReport);

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
    uint64_t taskGeneration = 0;
    double maxFps = 0.0;
    while (running) {
        const auto frameBegin = std::chrono::steady_clock::now();
        const bool measuring = benchmarkEnabled && renderedFrames >= benchmark.warmupFrames;
        float deltaSeconds = 0.0f;
        const auto inputBegin = std::chrono::steady_clock::now();
        m_input.update(
            m_window, [this](const SDL_Event& event) { m_renderer.processUiEvent(event); }, m_renderer.uiInputCapture(),
            application.mouseCapturePolicy());
        const double inputMilliseconds = elapsedMilliseconds(inputBegin);

        if (m_input.resizeRequested()) {
            m_renderer.requestResize();
        }

        if (m_input.quitRequested()) {
            applicationSession.requestQuit();
        }

        double updateMilliseconds = 0.0;
        double animationMilliseconds = 0.0;
        double physicsMilliseconds = 0.0;
        FrameTaskGraphStats animationTaskStats;
        if (benchmark.mode != BenchmarkMode::RenderOnly) {
            if (benchmark.mode == BenchmarkMode::FullRuntime) {
                deltaSeconds = m_physics.config().fixedTimeStep;
            } else {
                const uint64_t now = SDL_GetTicksNS();
                deltaSeconds = std::min(static_cast<float>(now - previousTicks) / 1'000'000'000.0f, 0.1f);
            }
            const uint64_t now = SDL_GetTicksNS();
            previousTicks = now;
            const auto updateBegin = std::chrono::steady_clock::now();
            applicationSession.update(m_input.state(), deltaSeconds);
            updateMilliseconds = elapsedMilliseconds(updateBegin);
            const auto animationBegin = std::chrono::steady_clock::now();
            const AnimationTaskResult animationResult =
                updateAnimations(m_scene, m_assets, deltaSeconds, m_frameTasks, ++taskGeneration);
            if (!animationResult) {
                std::cerr << "Animation task graph failed: " << animationResult.error << '\n';
                runSucceeded = false;
                running = false;
                break;
            }
            animationTaskStats = animationResult.stats;
            animationMilliseconds = elapsedMilliseconds(animationBegin);
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
                const auto physicsBegin = std::chrono::steady_clock::now();
                if (const PhysicsResult result = m_physics.step(m_scene, m_assets, fixedStep); !result) {
                    std::cerr << "Physics update failed: " << result.error << '\n';
                    runSucceeded = false;
                    running = false;
                    break;
                }
                physicsMilliseconds += elapsedMilliseconds(physicsBegin);
                if (measuring) {
                    benchmarkReport.physics(m_physics.stats());
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

        const auto transformsBegin = std::chrono::steady_clock::now();
        m_scene.updateWorldTransforms();
        const double transformsMilliseconds = elapsedMilliseconds(transformsBegin);
        const auto uiBegin = std::chrono::steady_clock::now();
        m_renderer.beginUiFrame(m_assets);
        int uiWidth = 1;
        int uiHeight = 1;
        SDL_GetWindowSizeInPixels(m_window.handle(), &uiWidth, &uiHeight);
        m_gameUi.beginFrame({static_cast<uint32_t>(std::max(uiWidth, 1)), static_cast<uint32_t>(std::max(uiHeight, 1))},
                            m_input.state());
        applicationSession.drawUi();
        m_renderer.drawGameUi(m_gameUi, m_assets);
        const double uiBuildMilliseconds = elapsedMilliseconds(uiBegin);
        const auto renderBegin = std::chrono::steady_clock::now();
        if (!m_renderer.drawFrame(m_window, m_scene, m_assets)) {
            runSucceeded = false;
            running = false;
            break;
        }
        const double renderMilliseconds = elapsedMilliseconds(renderBegin);
        // Rendering and audio consume the same finalized transform hierarchy.
        const auto audioBegin = std::chrono::steady_clock::now();
        if (benchmark.mode != BenchmarkMode::RenderOnly) {
            m_audio.syncScene(m_scene);
            m_audio.update(deltaSeconds);
        }
        const double audioMilliseconds = elapsedMilliseconds(audioBegin);

        ++fpsFrames;
        const uint64_t frameEndTicks = SDL_GetTicksNS();
        if (measuring) {
            benchmarkReport.sample("cpu.frame", elapsedMilliseconds(frameBegin), "ms",
                                   benchmark.frameBudgetMilliseconds);
            benchmarkReport.sample("cpu.input", inputMilliseconds);
            benchmarkReport.sample("cpu.application_update", updateMilliseconds);
            benchmarkReport.sample("cpu.animation", animationMilliseconds);
            benchmarkReport.sample("animation_tasks.queue_depth", animationTaskStats.maximumQueueDepth, "count");
            benchmarkReport.sample("animation_tasks.worker_utilization", animationTaskStats.workerUtilization, "ratio");
            benchmarkReport.sample("animation_tasks.main_thread_assist",
                                   animationTaskStats.mainThreadAssistMilliseconds);
            benchmarkReport.sample("animation_tasks.graph_latency", animationTaskStats.latencyMilliseconds);
            benchmarkReport.sample("cpu.physics_total", physicsMilliseconds);
            benchmarkReport.sample("cpu.transforms", transformsMilliseconds);
            benchmarkReport.sample("cpu.ui_build", uiBuildMilliseconds);
            benchmarkReport.sample("cpu.renderer", renderMilliseconds);
            benchmarkReport.sample("cpu.audio", audioMilliseconds);
            const RenderStats& stats = m_rendering.stats();
            const RenderPassBudgets& budgets = m_rendering.settings().budgets;
            benchmarkReport.renderCpu(stats, budgets);
            benchmarkReport.renderGpu(stats, budgets, benchmark.warmupFrames + 1,
                                      benchmark.warmupFrames + benchmark.frames);
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
    if (benchmarkEnabled) {
        uint64_t estimatedGpuAssetBytes = 0;
        for (const AssetDiagnostic& diagnostic : m_assets.diagnostics()) {
            estimatedGpuAssetBytes += diagnostic.gpuBytes;
        }
        const VkExtent2D extent = m_renderer.outputExtent();
        benchmarkReport.metadata("mode", benchmark.mode == BenchmarkMode::RenderOnly ? "render" : "full");
        benchmarkReport.metadata("requested_frames", std::to_string(benchmark.frames));
        benchmarkReport.metadata("warmup_frames", std::to_string(benchmark.warmupFrames));
        benchmarkReport.metadata("fixed_timestep_seconds", std::to_string(m_physics.config().fixedTimeStep));
        benchmarkReport.metadata("benchmark_objects",
                                 SDL_getenv("ZENITH_BENCHMARK_OBJECTS") ? SDL_getenv("ZENITH_BENCHMARK_OBJECTS") : "0");
        benchmarkReport.metadata("gpu", m_renderer.gpuName());
        benchmarkReport.metadata("frame_workers", std::to_string(m_frameTasks.workerCount()));
        benchmarkReport.metadata("resolution", std::to_string(extent.width) + "x" + std::to_string(extent.height));
        benchmarkReport.metadata("present_mode", m_renderer.presentModeName());
        benchmarkReport.metadata("cpu_asset_bytes", std::to_string(m_assets.cpuBytes()));
        benchmarkReport.metadata("estimated_gpu_asset_bytes", std::to_string(estimatedGpuAssetBytes));
        benchmarkReport.metadata("gpu_allocation_bytes", std::to_string(m_renderer.gpuAllocationBytes()));
        benchmarkReport.metadata("gpu_timing_note", "retired submissions only; sample count may be below CPU count");
        std::cout << benchmarkReport.json(benchmark.includeSamples) << '\n';
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
