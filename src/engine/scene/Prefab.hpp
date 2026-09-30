#pragma once

#include "Scene.hpp"

#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct PrefabEntityId {
    uint64_t value{0};

    explicit operator bool() const {
        return value != 0;
    }

    auto operator<=>(const PrefabEntityId&) const = default;
};

struct PrefabEntity {
    PrefabEntityId id;
    std::string name;
    std::optional<PrefabEntityId> parent;
    glm::mat4 localTransform{1.0f};
    std::optional<std::filesystem::path> modelAsset;
    // Component payloads remain distinct from property overrides so instances
    // can replace or remove one component without copying the source entity.
    std::vector<std::pair<std::string, std::string>> components;
};

struct PrefabOverride {
    PrefabEntityId entity;
    std::string property;
    std::string jsonValue;
};

struct NestedPrefab {
    uint64_t id{0};
    std::filesystem::path source;
    std::optional<PrefabEntityId> parent;
    std::vector<PrefabOverride> overrides;
};

struct PrefabDocument {
    static constexpr uint32_t currentVersion = 1;
    std::vector<PrefabEntity> entities;
    std::vector<NestedPrefab> nested;
};

struct PrefabResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

PrefabResult serializePrefab(const PrefabDocument& prefab, std::string& output);
PrefabResult deserializePrefab(std::string_view input, PrefabDocument& prefab);
PrefabResult savePrefab(const std::filesystem::path& path, const PrefabDocument& prefab);
PrefabResult loadPrefab(const std::filesystem::path& path, PrefabDocument& prefab);

struct PrefabInstanceId {
    uint64_t value{0};

    explicit operator bool() const {
        return value != 0;
    }

    auto operator<=>(const PrefabInstanceId&) const = default;
};

struct PrefabInstanceResult {
    bool success{false};
    Entity root{entt::null};
    std::vector<Entity> entities;
    std::vector<std::string> diagnostics;
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

class PrefabSystem {
  public:
    explicit PrefabSystem(std::filesystem::path prefabRoot, AssetManager* assets = nullptr);
    PrefabInstanceResult instantiate(const std::filesystem::path& reference, PrefabInstanceId instanceId, Scene& scene,
                                     Entity parent = entt::null, std::span<const PrefabOverride> overrides = {});
    PrefabInstanceResult refresh(const std::filesystem::path& reference, PrefabInstanceId instanceId, Scene& scene,
                                 Entity existingRoot, Entity parent = entt::null,
                                 std::span<const PrefabOverride> overrides = {});

  private:
    PrefabInstanceResult instantiateRecursive(const std::filesystem::path& path, PrefabInstanceId instanceId,
                                              Scene& scene, Entity parent, std::span<const PrefabOverride> overrides,
                                              std::vector<std::filesystem::path>& stack);
    PrefabResult resolve(const std::filesystem::path& reference, std::filesystem::path& path) const;

    std::filesystem::path m_root;
    AssetManager* m_assets{nullptr};
};
