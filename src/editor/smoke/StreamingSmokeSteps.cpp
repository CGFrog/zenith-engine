// The editor smoke's streaming check: a large scene loads as an additive layer
// on a worker thread while the other steps present frames, and must commit.

#include "editor/smoke/EditorSmokeHooks.hpp"
#include "editor/smoke/EditorSmokeSteps.hpp"
#include "engine/jobs/JobSystem.hpp"
#include "engine/scene/AdditiveSceneManager.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <optional>
#include <system_error>
#include <utility>

namespace {

// The scene file, the job system, and the layers the two steps share. The
// layers share the document's component registry, so they are released as
// soon as the load committed, before the module reload.
class StreamingSmokeFixture {
  public:
    StreamingSmokeFixture(AssetManager& assets, const SceneDocumentOptions& options, std::filesystem::path scenePath)
        : m_scenePath(std::move(scenePath))
        , m_jobs(std::make_unique<JobSystem>(JobSystemConfig{.workerCount = 1, .queueCapacity = 4}))
        , m_layers(std::make_unique<AdditiveSceneManager>(*m_jobs, assets, options)) {}

    StreamingSmokeFixture(const StreamingSmokeFixture&) = delete;
    StreamingSmokeFixture& operator=(const StreamingSmokeFixture&) = delete;

    ~StreamingSmokeFixture() {
        release();
        std::error_code ignored;
        std::filesystem::remove(m_scenePath, ignored);
    }

    void startLoad() {
        m_instance = m_layers->requestLoad(m_scenePath, 100);
    }

    // Done once the load committed (and the layers are released), Running
    // while it loads, Failed when it failed or was cancelled.
    SmokeStepResult poll(AssetManager& assets) {
        if (!m_layers) {
            return SmokeStepResult::done();
        }
        if (!m_instance) {
            return SmokeStepResult::failed("the load was never started");
        }
        m_layers->update();
        const std::optional<AdditiveSceneState> state = m_layers->state(*m_instance);
        if (!state) {
            return SmokeStepResult::failed("the streamed layer disappeared");
        }
        if (state->status == AdditiveSceneStatus::Failed || state->status == AdditiveSceneStatus::Cancelled) {
            return SmokeStepResult::failed(state->error);
        }
        if (state->status != AdditiveSceneStatus::Committed) {
            return SmokeStepResult::running();
        }
        m_layers->unload(*m_instance);
        assets.collectUnused(true);
        release();
        return SmokeStepResult::done();
    }

  private:
    void release() {
        if (m_layers) {
            m_layers->clear();
            m_layers.reset();
        }
        if (m_jobs) {
            m_jobs->shutdown(true);
            m_jobs.reset();
        }
    }

    std::filesystem::path m_scenePath;
    std::unique_ptr<JobSystem> m_jobs;
    std::unique_ptr<AdditiveSceneManager> m_layers;
    std::optional<SceneInstanceId> m_instance;
};

class StartStreamLoadSmokeStep final : public SmokeStep {
  public:
    explicit StartStreamLoadSmokeStep(std::shared_ptr<StreamingSmokeFixture> fixture)
        : m_fixture(std::move(fixture)) {}

    std::string_view name() const override {
        return "start streaming load";
    }

    SmokeStepResult update(SmokeStepContext& context, uint32_t) override {
        context.app.rendering.requestViewportSize(800, 450);
        m_fixture->startLoad();
        return SmokeStepResult::done();
    }

  private:
    std::shared_ptr<StreamingSmokeFixture> m_fixture;
};

class WaitForStreamCommitSmokeStep final : public SmokeStep {
  public:
    explicit WaitForStreamCommitSmokeStep(std::shared_ptr<StreamingSmokeFixture> fixture)
        : m_fixture(std::move(fixture)) {}

    std::string_view name() const override {
        return "wait for streaming commit";
    }

    SmokeStepResult update(SmokeStepContext& context, uint32_t) override {
        // The layer only commits here, on the main thread; the worker loads it
        // while the steps in between run.
        return m_fixture->poll(context.app.assets);
    }

    uint32_t frameBudget() const override {
        return 400;
    }

  private:
    std::shared_ptr<StreamingSmokeFixture> m_fixture;
};

} // namespace

StreamingSmokeSteps makeStreamingSmokeSteps(ApplicationContext& app, const SceneDocumentOptions& options,
                                            std::filesystem::path scenePath) {
    Scene streamed;
    for (uint32_t index = 0; index < 5'000; ++index) {
        streamed.createEntity("Streamed entity " + std::to_string(index));
    }
    // A model from the opened scene too, when it has one.
    const auto models = app.scene.registry().view<const ModelComponent>();
    if (!models.empty()) {
        const ModelHandle model = models.get<const ModelComponent>(*models.begin()).model;
        const Entity modelRoot = streamed.createEntity("Streamed model");
        if (SceneResult result = streamed.instantiateModel(modelRoot, model, app.assets); !result) {
            return {.error = "Could not prepare streaming smoke model: " + result.error};
        }
    }
    if (SceneDocumentResult result = SceneDocument::save(scenePath, streamed, app.assets, options); !result) {
        return {.error = "Could not prepare streaming smoke scene: " + result.error};
    }
    auto fixture = std::make_shared<StreamingSmokeFixture>(app.assets, options, std::move(scenePath));
    return {.startLoad = std::make_unique<StartStreamLoadSmokeStep>(fixture),
            .waitForCommit = std::make_unique<WaitForStreamCommitSmokeStep>(fixture)};
}
