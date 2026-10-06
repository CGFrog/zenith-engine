#pragma once

// Asynchronous and additive scene instances (streaming layers).

#include "SceneDocument.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/jobs/JobSystem.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

struct SceneInstanceId {
    uint64_t value{0};

    explicit operator bool() const {
        return value != 0;
    }

    auto operator<=>(const SceneInstanceId&) const = default;
};

enum class AdditiveSceneStatus : uint8_t {
    Loading,
    ReadyToCommit,
    Committed,
    Failed,
    Cancelled,
};

struct AdditiveSceneState {
    SceneInstanceId id;
    std::filesystem::path source;
    AdditiveSceneStatus status{AdditiveSceneStatus::Loading};
    float progress{0.0f};
    std::string error;
};

struct StreamingSceneRequest {
    std::filesystem::path source;
    int priority{0};
};

struct SceneInstanceEntityReference {
    SceneInstanceId instance;
    SceneEntityId entity;
};

struct SceneInstanceEntityResult {
    const Scene* scene{nullptr};
    Entity entity{entt::null};
    std::string error;

    explicit operator bool() const {
        return scene != nullptr && entity != entt::null;
    }
};

class AssetManager;

class AdditiveSceneManager {
  public:
    AdditiveSceneManager(JobSystem& jobs, AssetManager& assets, SceneDocumentOptions options);

    SceneInstanceId requestLoad(const std::filesystem::path& path, int priority = 0);
    void cancel(SceneInstanceId id);
    void synchronizeStreamingSet(std::span<const StreamingSceneRequest> requests);
    void update();
    bool unload(SceneInstanceId id);
    void clear();

    Scene* scene(SceneInstanceId id);
    const Scene* scene(SceneInstanceId id) const;
    std::optional<AdditiveSceneState> state(SceneInstanceId id) const;
    std::vector<SceneInstanceId> committedInstances() const;
    std::optional<SceneInstanceId> streamingInstance(const std::filesystem::path& source) const;
    SceneInstanceEntityResult resolve(SceneInstanceEntityReference reference) const;

  private:
    struct Instance {
        AdditiveSceneState state;
        JobHandle job;
        std::unique_ptr<Scene> scene;
        AssetLeaseSet assets;
    };

    JobSystem& m_jobs;
    AssetManager& m_assets;
    SceneDocumentOptions m_options;
    uint64_t m_nextId{1};
    std::vector<SceneInstanceId> m_order;
    std::unordered_map<uint64_t, Instance> m_instances;
    std::unordered_map<uint64_t, uint64_t> m_jobToInstance;
    std::unordered_map<std::string, SceneInstanceId> m_streamingInstances;
};
