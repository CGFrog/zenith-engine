#pragma once

#include "CommandHistory.hpp"
#include "engine/scene/Prefab.hpp"

#include <filesystem>
#include <memory>
#include <span>
#include <vector>

struct PrefabAuthoringInstance {
    std::filesystem::path source;
    PrefabInstanceId instanceId;
    SceneEntityId root;
    SceneEntityId parent;
    std::vector<PrefabOverride> overrides;
    std::vector<std::string> diagnostics;
    bool linked{false};
};

struct PrefabOverrideInspection {
    std::vector<PrefabOverride> active;
    std::vector<PrefabOverride> orphaned;
    std::vector<PrefabOverride> unsupported;
};

class PrefabAuthoring {
  public:
    explicit PrefabAuthoring(std::filesystem::path prefabRoot);

    EditorCommandResult create(CommandHistory& history, EditorCommandContext& context, Entity root,
                               const std::filesystem::path& reference);
    EditorCommandResult instantiate(CommandHistory& history, EditorCommandContext& context,
                                    const std::filesystem::path& reference, PrefabInstanceId instanceId,
                                    std::shared_ptr<PrefabAuthoringInstance>& instance, Entity parent = entt::null);
    EditorCommandResult unpack(CommandHistory& history, EditorCommandContext& context,
                               const std::shared_ptr<PrefabAuthoringInstance>& instance);
    EditorCommandResult apply(CommandHistory& history, EditorCommandContext& context,
                              const std::shared_ptr<PrefabAuthoringInstance>& instance);
    EditorCommandResult overrideProperty(CommandHistory& history, EditorCommandContext& context,
                                         const std::shared_ptr<PrefabAuthoringInstance>& instance,
                                         PrefabOverride overrideValue);
    EditorCommandResult revert(CommandHistory& history, EditorCommandContext& context,
                               const std::shared_ptr<PrefabAuthoringInstance>& instance);

    PrefabResult setOverride(PrefabAuthoringInstance& instance, PrefabOverride overrideValue) const;
    PrefabResult inspect(const PrefabAuthoringInstance& instance, PrefabOverrideInspection& inspection) const;

  private:
    class CreateCommand;
    class InstantiateCommand;
    class LinkCommand;
    class RefreshCommand;
    class ApplyCommand;

    PrefabResult resolve(const std::filesystem::path& reference, std::filesystem::path& path) const;
    PrefabResult buildDocument(const Scene& scene, const AssetManager& assets, Entity root,
                               PrefabDocument& document) const;
    std::vector<std::shared_ptr<PrefabAuthoringInstance>> linkedInstances(const std::filesystem::path& source);

    std::filesystem::path m_root;
    std::vector<std::weak_ptr<PrefabAuthoringInstance>> m_instances;
};
