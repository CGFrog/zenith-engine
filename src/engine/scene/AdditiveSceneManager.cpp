#include "AdditiveSceneManager.hpp"

#include "engine/assets/Assets.hpp"

#include <fstream>
#include <sstream>

namespace {
std::string scenePathKey(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
    return (error ? path.lexically_normal() : canonical).generic_string();
}
} // namespace

AdditiveSceneManager::AdditiveSceneManager(JobSystem& jobs, AssetManager& assets, SceneDocumentOptions options)
    : m_jobs(jobs)
    , m_assets(assets)
    , m_options(std::move(options)) {}

SceneInstanceId AdditiveSceneManager::requestLoad(const std::filesystem::path& path, int priority) {
    const SceneInstanceId id{m_nextId++};
    JobHandle job = m_jobs.submit(
        [path](JobContext& context) {
            if (context.cancellationRequested()) {
                return JobResult{.success = false, .error = "Scene load cancelled"};
            }
            context.setProgress(0.2f);
            std::ifstream input(path, std::ios::binary);
            if (!input) {
                return JobResult{.success = false, .error = "Could not open scene: " + path.string()};
            }
            std::ostringstream contents;
            contents << input.rdbuf();
            if (!input.good() && !input.eof()) {
                return JobResult{.success = false, .error = "Could not read scene: " + path.string()};
            }
            if (context.cancellationRequested()) {
                return JobResult{.success = false, .error = "Scene load cancelled"};
            }
            context.setProgress(0.5f);
            auto parsed = std::make_shared<ParsedSceneDocument>();
            if (SceneDocumentResult result = SceneDocument::parse(std::move(contents).str(), *parsed); !result) {
                return JobResult{.success = false, .error = result.error};
            }
            context.setProgress(1.0f);
            return JobResult{.object = std::move(parsed)};
        },
        priority);
    if (!job) {
        m_instances.emplace(id.value, Instance{{id, path, AdditiveSceneStatus::Failed, 0.0f,
                                                "Scene load queue is full or shutting down"},
                                               {},
                                               nullptr,
                                               {}});
        return id;
    }
    m_jobToInstance.emplace(job.id(), id.value);
    m_instances.emplace(id.value, Instance{{id, path}, job, nullptr, {}});
    return id;
}

void AdditiveSceneManager::cancel(SceneInstanceId id) {
    const auto found = m_instances.find(id.value);
    if (found != m_instances.end() && found->second.state.status == AdditiveSceneStatus::Loading) {
        found->second.job.cancel();
    }
}

void AdditiveSceneManager::synchronizeStreamingSet(std::span<const StreamingSceneRequest> requests) {
    std::unordered_map<std::string, StreamingSceneRequest> desired;
    for (const StreamingSceneRequest& request : requests) {
        const std::string key = scenePathKey(request.source);
        const auto [iterator, inserted] = desired.emplace(key, request);
        if (!inserted && request.priority > iterator->second.priority) {
            iterator->second = request;
        }
    }

    for (auto iterator = m_streamingInstances.begin(); iterator != m_streamingInstances.end();) {
        if (desired.contains(iterator->first)) {
            ++iterator;
            continue;
        }
        const SceneInstanceId instance = iterator->second;
        iterator = m_streamingInstances.erase(iterator);
        unload(instance);
    }

    for (const auto& [key, request] : desired) {
        if (const auto found = m_streamingInstances.find(key); found != m_streamingInstances.end()) {
            const auto instance = m_instances.find(found->second.value);
            if (instance != m_instances.end() && instance->second.state.status == AdditiveSceneStatus::Loading) {
                m_jobs.reprioritize(instance->second.job, request.priority);
            }
            continue;
        }
        m_streamingInstances.emplace(key, requestLoad(request.source, request.priority));
    }
}

void AdditiveSceneManager::update() {
    for (auto& [_, instance] : m_instances) {
        if (instance.state.status == AdditiveSceneStatus::Loading) {
            instance.state.progress = instance.job.progress();
        }
    }
    std::vector<uint64_t> jobIds;
    jobIds.reserve(m_jobToInstance.size());
    for (const auto& [jobId, _] : m_jobToInstance) {
        jobIds.push_back(jobId);
    }
    for (CompletedJob& completed : m_jobs.takeCompleted(jobIds)) {
        const auto mapping = m_jobToInstance.find(completed.handle.id());
        if (mapping == m_jobToInstance.end()) {
            continue;
        }
        const auto found = m_instances.find(mapping->second);
        m_jobToInstance.erase(mapping);
        if (found == m_instances.end()) {
            continue;
        }
        Instance& instance = found->second;
        if (completed.handle.status() == JobStatus::Cancelled) {
            instance.state.status = AdditiveSceneStatus::Cancelled;
            instance.state.error = completed.result.error;
            continue;
        }
        if (!completed.result.success) {
            instance.state.status = AdditiveSceneStatus::Failed;
            instance.state.error = completed.result.error;
            continue;
        }
        instance.state.status = AdditiveSceneStatus::ReadyToCommit;
        auto staged = std::make_unique<Scene>();
        const auto parsed = std::static_pointer_cast<const ParsedSceneDocument>(completed.result.object);
        if (!parsed) {
            instance.state.status = AdditiveSceneStatus::Failed;
            instance.state.error = "Scene parse job completed without a parsed document";
            continue;
        }
        const SceneDocumentResult result = SceneDocument::commit(*parsed, m_assets, m_options, *staged);
        if (!result) {
            instance.state.status = AdditiveSceneStatus::Failed;
            instance.state.error = result.error;
            continue;
        }
        instance.scene = std::move(staged);
        instance.assets = m_assets.leaseSceneAssets(*instance.scene);
        instance.state.status = AdditiveSceneStatus::Committed;
        instance.state.progress = 1.0f;
        m_order.push_back(instance.state.id);
    }
}

bool AdditiveSceneManager::unload(SceneInstanceId id) {
    const auto found = m_instances.find(id.value);
    if (found == m_instances.end()) {
        return false;
    }
    if (found->second.state.status == AdditiveSceneStatus::Loading) {
        found->second.job.cancel();
    }
    std::erase(m_order, id);
    m_instances.erase(found);
    std::erase_if(m_streamingInstances, [id](const auto& entry) { return entry.second == id; });
    return true;
}

void AdditiveSceneManager::clear() {
    for (auto iterator = m_order.rbegin(); iterator != m_order.rend(); ++iterator) {
        m_instances.erase(iterator->value);
    }
    m_order.clear();
    m_streamingInstances.clear();
    for (auto& [_, instance] : m_instances) {
        instance.job.cancel();
    }
    m_instances.clear();
}

Scene* AdditiveSceneManager::scene(SceneInstanceId id) {
    const auto found = m_instances.find(id.value);
    return found == m_instances.end() ? nullptr : found->second.scene.get();
}

const Scene* AdditiveSceneManager::scene(SceneInstanceId id) const {
    const auto found = m_instances.find(id.value);
    return found == m_instances.end() ? nullptr : found->second.scene.get();
}

std::optional<AdditiveSceneState> AdditiveSceneManager::state(SceneInstanceId id) const {
    const auto found = m_instances.find(id.value);
    return found == m_instances.end() ? std::nullopt : std::optional<AdditiveSceneState>{found->second.state};
}

std::vector<SceneInstanceId> AdditiveSceneManager::committedInstances() const {
    return m_order;
}

std::optional<SceneInstanceId> AdditiveSceneManager::streamingInstance(const std::filesystem::path& source) const {
    const auto found = m_streamingInstances.find(scenePathKey(source));
    return found == m_streamingInstances.end() ? std::nullopt : std::optional<SceneInstanceId>{found->second};
}

SceneInstanceEntityResult AdditiveSceneManager::resolve(SceneInstanceEntityReference reference) const {
    const auto instance = m_instances.find(reference.instance.value);
    if (instance == m_instances.end() || instance->second.state.status != AdditiveSceneStatus::Committed ||
        instance->second.scene == nullptr) {
        return {.error = "Cross-scene reference targets an uncommitted or missing scene instance"};
    }
    const Entity entity = instance->second.scene->findEntity(reference.entity);
    if (entity == entt::null) {
        return {.error = "Cross-scene reference targets a missing stable entity ID"};
    }
    return {.scene = instance->second.scene.get(), .entity = entity};
}
