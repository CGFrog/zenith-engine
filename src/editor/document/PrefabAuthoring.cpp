#include "PrefabAuthoring.hpp"

#include "engine/assets/Assets.hpp"

#include <glm/gtc/type_ptr.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <functional>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace {
using Json = nlohmann::json;

EditorCommandResult sceneSnapshot(EditorCommandContext& context, std::string& output) {
    const SceneDocumentResult result =
        SceneDocument::serialize(context.scene, context.assets, context.documentOptions, output);
    return {.success = result.success, .error = result.error};
}

EditorCommandResult restoreScene(EditorCommandContext& context, const std::string& snapshot) {
    const SceneDocumentResult result =
        SceneDocument::deserialize(snapshot, context.assets, context.documentOptions, context.scene);
    return {.success = result.success, .error = result.error};
}

PrefabResult readFile(const std::filesystem::path& path, bool& exists, std::string& contents) {
    exists = std::filesystem::is_regular_file(path);
    if (!exists) {
        contents.clear();
        return {.success = true};
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {.error = "Could not read prefab source: " + path.string()};
    }
    std::ostringstream stream;
    stream << input.rdbuf();
    contents = stream.str();
    return {.success = true};
}

PrefabResult writeFile(const std::filesystem::path& path, bool exists, std::string_view contents) {
    std::error_code error;
    if (!exists) {
        std::filesystem::remove(path, error);
        return error ? PrefabResult{.error = "Could not remove prefab source: " + error.message()}
                     : PrefabResult{.success = true};
    }
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return {.error = "Could not create prefab directory: " + error.message()};
    }
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output || !(output << contents)) {
            return {.error = "Could not write temporary prefab source: " + temporary.string()};
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary);
        return {.error = "Could not atomically replace prefab source (Windows error " + std::to_string(GetLastError()) +
                         ")"};
    }
#else
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary);
        return {.error = "Could not atomically replace prefab source: " + error.message()};
    }
#endif
    return {.success = true};
}

bool applyOverride(PrefabDocument& document, const PrefabOverride& overrideValue) {
    const auto found = std::ranges::find_if(
        document.entities, [&](const PrefabEntity& entity) { return entity.id == overrideValue.entity; });
    if (found == document.entities.end()) {
        return false;
    }
    try {
        const Json value = Json::parse(overrideValue.jsonValue);
        if (overrideValue.property == "name" && value.is_string()) {
            found->name = value.get<std::string>();
            return true;
        }
        if (overrideValue.property == "localTransform" && value.is_array() && value.size() == 16) {
            for (size_t index = 0; index < 16; ++index) {
                glm::value_ptr(found->localTransform)[index] = value[index].get<float>();
            }
            return true;
        }
        constexpr std::string_view componentPrefix = "component.";
        if (overrideValue.property.starts_with(componentPrefix)) {
            const std::string type = overrideValue.property.substr(componentPrefix.size());
            if (type != "pointLight" && type != "camera") {
                return false;
            }
            const auto component =
                std::ranges::find_if(found->components, [&](const auto& entry) { return entry.first == type; });
            if (value.is_null()) {
                if (component != found->components.end()) {
                    found->components.erase(component);
                }
            } else if (component == found->components.end()) {
                found->components.emplace_back(type, value.dump());
            } else {
                component->second = value.dump();
            }
            return true;
        }
    } catch (const Json::exception&) {
    }
    return false;
}
} // namespace

class PrefabAuthoring::CreateCommand final : public EditorCommand {
  public:
    CreateCommand(std::filesystem::path path, std::string after)
        : m_path(std::move(path))
        , m_after(std::move(after)) {}

    std::string_view name() const override {
        return "Create Prefab";
    }

    EditorCommandResult execute(EditorCommandContext&) override {
        if (!m_captured) {
            if (PrefabResult result = readFile(m_path, m_beforeExists, m_before); !result) {
                return {.error = result.error};
            }
            m_captured = true;
        }
        const PrefabResult result = writeFile(m_path, true, m_after);
        return {.success = result.success, .error = result.error};
    }

    EditorCommandResult undo(EditorCommandContext&) override {
        const PrefabResult result = writeFile(m_path, m_beforeExists, m_before);
        return {.success = result.success, .error = result.error};
    }

  private:
    std::filesystem::path m_path;
    std::string m_before;
    std::string m_after;
    bool m_beforeExists{false};
    bool m_captured{false};
};

class PrefabAuthoring::InstantiateCommand final : public EditorCommand {
  public:
    InstantiateCommand(PrefabAuthoring& owner, std::filesystem::path reference, PrefabInstanceId instanceId,
                       SceneEntityId parent, std::shared_ptr<PrefabAuthoringInstance> instance)
        : m_owner(owner)
        , m_reference(std::move(reference))
        , m_instanceId(instanceId)
        , m_parent(parent)
        , m_instance(std::move(instance)) {}

    std::string_view name() const override {
        return "Instantiate Prefab";
    }

    EditorCommandResult execute(EditorCommandContext& context) override {
        if (!m_after.empty()) {
            if (EditorCommandResult result = restoreScene(context, m_after); !result) {
                return result;
            }
            *m_instance = m_afterState;
            return {.success = true};
        }
        if (EditorCommandResult result = sceneSnapshot(context, m_before); !result) {
            return result;
        }
        PrefabSystem prefabs(m_owner.m_root, &context.assets);
        const Entity parent = m_parent ? context.scene.findEntity(m_parent) : entt::null;
        PrefabInstanceResult result = prefabs.instantiate(m_reference, m_instanceId, context.scene, parent);
        if (!result) {
            restoreScene(context, m_before);
            return {.error = result.error};
        }
        *m_instance = {.source = m_reference,
                       .instanceId = m_instanceId,
                       .root = context.scene.id(result.root),
                       .parent = m_parent,
                       .diagnostics = std::move(result.diagnostics),
                       .linked = true};
        m_afterState = *m_instance;
        if (EditorCommandResult snapshot = sceneSnapshot(context, m_after); !snapshot) {
            restoreScene(context, m_before);
            return snapshot;
        }
        return {.success = true};
    }

    EditorCommandResult undo(EditorCommandContext& context) override {
        if (EditorCommandResult result = restoreScene(context, m_before); !result) {
            return result;
        }
        *m_instance = {};
        return {.success = true};
    }

  private:
    PrefabAuthoring& m_owner;
    std::filesystem::path m_reference;
    PrefabInstanceId m_instanceId;
    SceneEntityId m_parent;
    std::shared_ptr<PrefabAuthoringInstance> m_instance;
    PrefabAuthoringInstance m_afterState;
    std::string m_before;
    std::string m_after;
};

class PrefabAuthoring::LinkCommand final : public EditorCommand {
  public:
    LinkCommand(std::shared_ptr<PrefabAuthoringInstance> instance, bool linked, std::string label)
        : m_instance(std::move(instance))
        , m_before(m_instance ? m_instance->linked : false)
        , m_after(linked)
        , m_label(std::move(label)) {}

    std::string_view name() const override {
        return m_label;
    }

    EditorCommandResult execute(EditorCommandContext&) override {
        if (!m_instance) {
            return {.error = "Prefab instance no longer exists"};
        }
        m_instance->linked = m_after;
        return {.success = true};
    }

    EditorCommandResult undo(EditorCommandContext&) override {
        m_instance->linked = m_before;
        return {.success = true};
    }

  private:
    std::shared_ptr<PrefabAuthoringInstance> m_instance;
    bool m_before{false};
    bool m_after{false};
    std::string m_label;
};

class PrefabAuthoring::RefreshCommand final : public EditorCommand {
  public:
    RefreshCommand(PrefabAuthoring& owner, std::shared_ptr<PrefabAuthoringInstance> instance,
                   std::optional<PrefabOverride> overrideValue = std::nullopt)
        : m_owner(owner)
        , m_instance(std::move(instance))
        , m_override(std::move(overrideValue)) {}

    std::string_view name() const override {
        return m_override ? "Set Prefab Override" : "Revert Prefab Overrides";
    }

    EditorCommandResult execute(EditorCommandContext& context) override {
        if (!m_after.empty()) {
            if (EditorCommandResult result = restoreScene(context, m_after); !result) {
                return result;
            }
            *m_instance = m_afterState;
            return {.success = true};
        }
        if (!m_instance || !m_instance->linked) {
            return {.error = "Cannot revert an unpacked prefab instance"};
        }
        m_beforeState = *m_instance;
        if (EditorCommandResult result = sceneSnapshot(context, m_before); !result) {
            return result;
        }
        const Entity root = context.scene.findEntity(m_instance->root);
        const Entity parent = m_instance->parent ? context.scene.findEntity(m_instance->parent) : entt::null;
        PrefabSystem prefabs(m_owner.m_root, &context.assets);
        if (m_override) {
            if (PrefabResult result = m_owner.setOverride(*m_instance, *m_override); !result) {
                return {.error = result.error};
            }
        } else {
            m_instance->overrides.clear();
        }
        PrefabInstanceResult refreshed = prefabs.refresh(m_instance->source, m_instance->instanceId, context.scene,
                                                         root, parent, m_instance->overrides);
        if (!refreshed) {
            restoreScene(context, m_before);
            *m_instance = m_beforeState;
            return {.error = refreshed.error};
        }
        m_instance->root = context.scene.id(refreshed.root);
        m_instance->diagnostics = std::move(refreshed.diagnostics);
        m_afterState = *m_instance;
        if (EditorCommandResult result = sceneSnapshot(context, m_after); !result) {
            restoreScene(context, m_before);
            *m_instance = m_beforeState;
            return result;
        }
        return {.success = true};
    }

    EditorCommandResult undo(EditorCommandContext& context) override {
        if (EditorCommandResult result = restoreScene(context, m_before); !result) {
            return result;
        }
        *m_instance = m_beforeState;
        return {.success = true};
    }

  private:
    PrefabAuthoring& m_owner;
    std::shared_ptr<PrefabAuthoringInstance> m_instance;
    std::optional<PrefabOverride> m_override;
    PrefabAuthoringInstance m_beforeState;
    PrefabAuthoringInstance m_afterState;
    std::string m_before;
    std::string m_after;
};

class PrefabAuthoring::ApplyCommand final : public EditorCommand {
  public:
    ApplyCommand(PrefabAuthoring& owner, std::filesystem::path path, std::string afterSource,
                 std::shared_ptr<PrefabAuthoringInstance> appliedInstance,
                 std::vector<std::shared_ptr<PrefabAuthoringInstance>> instances)
        : m_owner(owner)
        , m_path(std::move(path))
        , m_afterSource(std::move(afterSource))
        , m_appliedInstance(std::move(appliedInstance))
        , m_instances(std::move(instances)) {}

    std::string_view name() const override {
        return "Apply Prefab Overrides";
    }

    EditorCommandResult execute(EditorCommandContext& context) override {
        if (!m_afterScene.empty()) {
            if (PrefabResult write = writeFile(m_path, true, m_afterSource); !write) {
                return {.error = write.error};
            }
            if (EditorCommandResult result = restoreScene(context, m_afterScene); !result) {
                writeFile(m_path, m_beforeExists, m_beforeSource);
                return result;
            }
            restoreStates(m_afterStates);
            return {.success = true};
        }
        if (EditorCommandResult result = sceneSnapshot(context, m_beforeScene); !result) {
            return result;
        }
        if (PrefabResult read = readFile(m_path, m_beforeExists, m_beforeSource); !read) {
            return {.error = read.error};
        }
        captureStates(m_beforeStates);
        if (PrefabResult write = writeFile(m_path, true, m_afterSource); !write) {
            return {.error = write.error};
        }
        PrefabSystem prefabs(m_owner.m_root, &context.assets);
        for (const auto& instance : m_instances) {
            if (!instance || !instance->linked) {
                continue;
            }
            const Entity root = context.scene.findEntity(instance->root);
            const Entity parent = instance->parent ? context.scene.findEntity(instance->parent) : entt::null;
            const std::span<const PrefabOverride> retainedOverrides =
                instance == m_appliedInstance ? std::span<const PrefabOverride>{} : instance->overrides;
            PrefabInstanceResult refreshed =
                prefabs.refresh(instance->source, instance->instanceId, context.scene, root, parent, retainedOverrides);
            if (!refreshed) {
                rollback(context);
                return {.error = refreshed.error};
            }
            instance->root = context.scene.id(refreshed.root);
            if (instance == m_appliedInstance) {
                instance->overrides.clear();
            }
            instance->diagnostics = std::move(refreshed.diagnostics);
        }
        captureStates(m_afterStates);
        if (EditorCommandResult result = sceneSnapshot(context, m_afterScene); !result) {
            rollback(context);
            return result;
        }
        return {.success = true};
    }

    EditorCommandResult undo(EditorCommandContext& context) override {
        if (PrefabResult write = writeFile(m_path, m_beforeExists, m_beforeSource); !write) {
            return {.error = write.error};
        }
        if (EditorCommandResult result = restoreScene(context, m_beforeScene); !result) {
            writeFile(m_path, true, m_afterSource);
            return result;
        }
        restoreStates(m_beforeStates);
        return {.success = true};
    }

  private:
    void captureStates(std::vector<PrefabAuthoringInstance>& output) const {
        output.clear();
        for (const auto& instance : m_instances) {
            output.push_back(instance ? *instance : PrefabAuthoringInstance{});
        }
    }

    void restoreStates(const std::vector<PrefabAuthoringInstance>& values) {
        for (size_t index = 0; index < std::min(values.size(), m_instances.size()); ++index) {
            if (m_instances[index]) {
                *m_instances[index] = values[index];
            }
        }
    }

    void rollback(EditorCommandContext& context) {
        restoreScene(context, m_beforeScene);
        writeFile(m_path, m_beforeExists, m_beforeSource);
        restoreStates(m_beforeStates);
    }

    PrefabAuthoring& m_owner;
    std::filesystem::path m_path;
    std::string m_beforeSource;
    std::string m_afterSource;
    std::string m_beforeScene;
    std::string m_afterScene;
    bool m_beforeExists{false};
    std::shared_ptr<PrefabAuthoringInstance> m_appliedInstance;
    std::vector<std::shared_ptr<PrefabAuthoringInstance>> m_instances;
    std::vector<PrefabAuthoringInstance> m_beforeStates;
    std::vector<PrefabAuthoringInstance> m_afterStates;
};

PrefabAuthoring::PrefabAuthoring(std::filesystem::path prefabRoot)
    : m_root(std::filesystem::absolute(std::move(prefabRoot)).lexically_normal()) {}

PrefabResult PrefabAuthoring::resolve(const std::filesystem::path& reference, std::filesystem::path& path) const {
    if (reference.empty() || reference.is_absolute()) {
        return {.error = "Prefab reference must be project-relative"};
    }
    path = (m_root / reference).lexically_normal();
    const std::filesystem::path relative = path.lexically_relative(m_root);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") {
        return {.error = "Prefab reference escapes the project root"};
    }
    return {.success = true};
}

PrefabResult PrefabAuthoring::buildDocument(const Scene& scene, const AssetManager& assets, Entity root,
                                            PrefabDocument& document) const {
    if (root == entt::null || !scene.registry().valid(root)) {
        return {.error = "Cannot create a prefab from an invalid entity"};
    }
    PrefabDocument result;
    uint64_t nextId = 1;
    std::function<PrefabResult(Entity, std::optional<PrefabEntityId>)> visit =
        [&](Entity entity, std::optional<PrefabEntityId> parent) -> PrefabResult {
        const PrefabEntityId id{nextId++};
        PrefabEntity value{id, scene.registry().get<NameComponent>(entity).name, parent,
                           scene.registry().get<TransformComponent>(entity).local};
        if (const auto* model = scene.registry().try_get<ModelComponent>(entity)) {
            const auto source = assets.modelSource(model->model);
            if (!source) {
                return {.error = "Cannot author a prefab from an unknown model handle"};
            }
            const std::filesystem::path relative = source->path.lexically_relative(m_root);
            if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") {
                return {.error = "Prefab model source is outside the project root"};
            }
            value.modelAsset = relative;
        }
        if (const auto* light = scene.registry().try_get<PointLightComponent>(entity)) {
            value.components.emplace_back("pointLight",
                                          Json({{"color", {light->color.x, light->color.y, light->color.z}},
                                                {"intensity", light->intensity},
                                                {"range", light->range},
                                                {"castsShadows", light->castsShadows},
                                                {"shadowResolution", light->shadowResolution},
                                                {"shadowPriority", light->shadowPriority}})
                                              .dump());
        }
        if (const auto* camera = scene.registry().try_get<CameraComponent>(entity)) {
            value.components.emplace_back("camera", Json({{"verticalFov", camera->verticalFovRadians},
                                                          {"nearPlane", camera->nearPlane},
                                                          {"farPlane", camera->farPlane},
                                                          {"primary", camera->primary}})
                                                        .dump());
        }
        result.entities.push_back(std::move(value));
        const auto& relationship = scene.registry().get<RelationshipComponent>(entity);
        for (Entity child : relationship.children) {
            if (scene.registry().all_of<ModelNodeComponent>(child)) {
                continue;
            }
            if (PrefabResult childResult = visit(child, id); !childResult) {
                return childResult;
            }
        }
        return {.success = true};
    };
    if (PrefabResult resultVisit = visit(root, std::nullopt); !resultVisit) {
        return resultVisit;
    }
    document = std::move(result);
    return {.success = true};
}

EditorCommandResult PrefabAuthoring::create(CommandHistory& history, EditorCommandContext& context, Entity root,
                                            const std::filesystem::path& reference) {
    std::filesystem::path path;
    if (PrefabResult result = resolve(reference, path); !result) {
        return {.error = result.error};
    }
    PrefabDocument document;
    if (PrefabResult result = buildDocument(context.scene, context.assets, root, document); !result) {
        return {.error = result.error};
    }
    std::string serialized;
    if (PrefabResult result = serializePrefab(document, serialized); !result) {
        return {.error = result.error};
    }
    return history.execute(std::make_unique<CreateCommand>(path, std::move(serialized)), context);
}

EditorCommandResult PrefabAuthoring::instantiate(CommandHistory& history, EditorCommandContext& context,
                                                 const std::filesystem::path& reference, PrefabInstanceId instanceId,
                                                 std::shared_ptr<PrefabAuthoringInstance>& instance, Entity parent) {
    std::filesystem::path path;
    if (PrefabResult result = resolve(reference, path); !result) {
        return {.error = result.error};
    }
    if (!instanceId) {
        return {.error = "Prefab instance ID must be non-zero"};
    }
    instance = std::make_shared<PrefabAuthoringInstance>();
    m_instances.push_back(instance);
    const SceneEntityId parentId = parent == entt::null ? SceneEntityId{} : context.scene.id(parent);
    return history.execute(std::make_unique<InstantiateCommand>(*this, reference, instanceId, parentId, instance),
                           context);
}

EditorCommandResult PrefabAuthoring::unpack(CommandHistory& history, EditorCommandContext& context,
                                            const std::shared_ptr<PrefabAuthoringInstance>& instance) {
    if (!instance || !instance->linked) {
        return {.error = "Cannot unpack an invalid or already unpacked prefab instance"};
    }
    return history.execute(std::make_unique<LinkCommand>(instance, false, "Unpack Prefab"), context);
}

EditorCommandResult PrefabAuthoring::apply(CommandHistory& history, EditorCommandContext& context,
                                           const std::shared_ptr<PrefabAuthoringInstance>& instance) {
    if (!instance || !instance->linked) {
        return {.error = "Cannot apply overrides from an invalid prefab instance"};
    }
    std::filesystem::path path;
    if (PrefabResult result = resolve(instance->source, path); !result) {
        return {.error = result.error};
    }
    PrefabDocument document;
    if (PrefabResult result = loadPrefab(path, document); !result) {
        return {.error = result.error};
    }
    for (const PrefabOverride& overrideValue : instance->overrides) {
        if (!applyOverride(document, overrideValue)) {
            return {.error = "Cannot apply orphaned or unsupported prefab override"};
        }
    }
    std::string serialized;
    if (PrefabResult result = serializePrefab(document, serialized); !result) {
        return {.error = result.error};
    }
    return history.execute(
        std::make_unique<ApplyCommand>(*this, path, std::move(serialized), instance, linkedInstances(instance->source)),
        context);
}

EditorCommandResult PrefabAuthoring::revert(CommandHistory& history, EditorCommandContext& context,
                                            const std::shared_ptr<PrefabAuthoringInstance>& instance) {
    return history.execute(std::make_unique<RefreshCommand>(*this, instance), context);
}

EditorCommandResult PrefabAuthoring::overrideProperty(CommandHistory& history, EditorCommandContext& context,
                                                      const std::shared_ptr<PrefabAuthoringInstance>& instance,
                                                      PrefabOverride overrideValue) {
    if (!instance || !instance->linked) {
        return {.error = "Cannot override an invalid or unpacked prefab instance"};
    }
    return history.execute(std::make_unique<RefreshCommand>(*this, instance, std::move(overrideValue)), context);
}

PrefabResult PrefabAuthoring::setOverride(PrefabAuthoringInstance& instance, PrefabOverride overrideValue) const {
    if (!instance.linked || !overrideValue.entity || overrideValue.property.empty()) {
        return {.error = "Prefab override requires a linked instance, entity ID, and property"};
    }
    try {
        const Json parsed = Json::parse(overrideValue.jsonValue);
        (void)parsed;
    } catch (const Json::exception&) {
        return {.error = "Prefab override value is not valid JSON"};
    }
    const auto found = std::ranges::find_if(instance.overrides, [&](const PrefabOverride& existing) {
        return existing.entity == overrideValue.entity && existing.property == overrideValue.property;
    });
    if (found == instance.overrides.end()) {
        instance.overrides.push_back(std::move(overrideValue));
    } else {
        *found = std::move(overrideValue);
    }
    return {.success = true};
}

PrefabResult PrefabAuthoring::inspect(const PrefabAuthoringInstance& instance,
                                      PrefabOverrideInspection& inspection) const {
    std::filesystem::path path;
    if (PrefabResult result = resolve(instance.source, path); !result) {
        return result;
    }
    PrefabDocument document;
    if (PrefabResult result = loadPrefab(path, document); !result) {
        return result;
    }
    inspection = {};
    for (const PrefabOverride& overrideValue : instance.overrides) {
        const bool entityExists = std::ranges::any_of(
            document.entities, [&](const PrefabEntity& entity) { return entity.id == overrideValue.entity; });
        if (!entityExists) {
            inspection.orphaned.push_back(overrideValue);
        } else if (overrideValue.property != "name" && overrideValue.property != "localTransform" &&
                   overrideValue.property != "component.pointLight" && overrideValue.property != "component.camera") {
            inspection.unsupported.push_back(overrideValue);
        } else {
            inspection.active.push_back(overrideValue);
        }
    }
    return {.success = true};
}

std::vector<std::shared_ptr<PrefabAuthoringInstance>>
PrefabAuthoring::linkedInstances(const std::filesystem::path& source) {
    std::vector<std::shared_ptr<PrefabAuthoringInstance>> result;
    std::erase_if(m_instances, [](const auto& instance) { return instance.expired(); });
    for (const auto& weak : m_instances) {
        if (auto instance = weak.lock(); instance && instance->linked && instance->source == source) {
            result.push_back(std::move(instance));
        }
    }
    return result;
}
