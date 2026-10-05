#include "SelectOperators.hpp"

#include "editor/document/EditorDocument.hpp"
#include "engine/scene/Collections.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/Prefab.hpp"
#include "engine/scene/SceneVisibility.hpp"
#include "engine/scene/Tags.hpp"

#include <algorithm>
#include <functional>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
using ExecuteFunction = std::function<OperatorResult(OperatorContext&, OperatorProperties&)>;

template <typename Properties, typename Function> ExecuteFunction typed(Function run) {
    return [run = std::move(run)](OperatorContext& context, OperatorProperties& properties) {
        Properties* values = properties.get<Properties>();
        if (values == nullptr) {
            return OperatorResult::failed("Operator properties have the wrong type");
        }
        return run(context, *values);
    };
}

bool hasSelection(const OperatorContext& context) {
    return !context.selection.empty();
}

Entity activeEntity(const OperatorContext& context) {
    const std::optional<SceneEntityId> active = context.selection.active();
    return active ? context.editing.scene().findEntity(*active) : entt::null;
}

bool hasActive(const OperatorContext& context) {
    return activeEntity(context) != entt::null;
}

// Collects selectable entities once each, in the order they are offered.
class Found {
  public:
    explicit Found(const Scene& scene)
        : m_scene(scene)
        , m_visibility(scene, SceneVisibilityMode::Editor) {}

    void add(Entity entity) {
        if (entity != entt::null && m_visibility.selectable(entity) && m_listed.insert(entity).second) {
            m_ids.push_back(m_scene.id(entity));
        }
    }

    std::vector<SceneEntityId>& ids() {
        return m_ids;
    }

  private:
    const Scene& m_scene;
    SceneVisibility m_visibility;
    std::unordered_set<Entity> m_listed;
    std::vector<SceneEntityId> m_ids;
};

// Selects found (its first entry active), replacing or extending the
// selection; cancels when nothing was found.
OperatorResult selectFound(OperatorContext& context, std::vector<SceneEntityId> found, bool extend) {
    if (found.empty()) {
        return OperatorResult::cancelled("Nothing to select");
    }
    if (extend) {
        for (SceneEntityId id : context.selection.ids()) {
            if (std::ranges::find(found, id) == found.end()) {
                found.push_back(id);
            }
        }
    }
    context.selection.assign(std::move(found));
    return OperatorResult::finished();
}

std::vector<Entity> selectedEntities(const OperatorContext& context) {
    std::vector<Entity> entities;
    for (SceneEntityId id : context.selection.ids()) {
        if (const Entity entity = context.editing.scene().findEntity(id); entity != entt::null) {
            entities.push_back(entity);
        }
    }
    return entities;
}

const std::vector<Entity>& childrenOf(const Scene& scene, Entity entity) {
    return scene.registry().get<RelationshipComponent>(entity).children;
}

OperatorResult selectHierarchy(OperatorContext& context, const SelectHierarchyProperties& values) {
    const Scene& scene = context.editing.scene();
    Found found(scene);
    // The active object's relatives come first, so one of them becomes active.
    const std::vector<Entity> sources = selectedEntities(context);
    for (const Entity entity : sources) {
        if (values.direction == SelectHierarchyDirection::Children) {
            for (const Entity child : childrenOf(scene, entity)) {
                found.add(child);
            }
        } else {
            found.add(scene.registry().get<RelationshipComponent>(entity).parent);
        }
    }
    return selectFound(context, std::move(found.ids()), values.extend);
}

void addDescendants(const Scene& scene, Entity entity, Found& found) {
    for (const Entity child : childrenOf(scene, entity)) {
        found.add(child);
        addDescendants(scene, child, found);
    }
}

// Whether entity shares the active entity's group of `type`; Children and
// ImmediateChildren are collected from the selection instead.
class GroupMatch {
  public:
    GroupMatch(const OperatorContext& context, Entity active)
        : m_scene(context.editing.scene())
        , m_registry(m_scene.registry())
        , m_active(active) {
        for (const ComponentType& type : componentTypesFor(context.editing.document().options()).types()) {
            if (type.has(m_scene, active)) {
                m_activeTypes.push_back(&type);
            }
        }
        if (const Entity root = prefabInstanceRoot(m_scene, active); root != entt::null) {
            m_prefab = m_registry.get<PrefabInstanceComponent>(root).prefab;
        }
        if (const auto* model = m_registry.try_get<ModelComponent>(active)) {
            m_model = model->model;
        } else if (const auto* node = m_registry.try_get<ModelNodeComponent>(active)) {
            m_model = node->model;
        }
    }

    bool matches(SelectGroupedType type, Entity entity) const {
        switch (type) {
        case SelectGroupedType::Prefab: {
            const auto* instance = m_registry.try_get<PrefabInstanceComponent>(entity);
            return m_prefab && instance != nullptr && instance->prefab == *m_prefab;
        }
        case SelectGroupedType::Model: {
            const auto* model = m_registry.try_get<ModelComponent>(entity);
            return m_model && model != nullptr && model->model == *m_model;
        }
        case SelectGroupedType::Mesh: {
            const auto* mine = m_registry.try_get<MeshRendererComponent>(m_active);
            const auto* theirs = m_registry.try_get<MeshRendererComponent>(entity);
            return mine != nullptr && theirs != nullptr && mine->mesh == theirs->mesh;
        }
        case SelectGroupedType::Tag:
            return sharesAny<TagsComponent>(entity, &TagsComponent::tags);
        case SelectGroupedType::Collection:
            return sharesAny<CollectionMembershipComponent>(entity, &CollectionMembershipComponent::collections);
        case SelectGroupedType::Components:
            return std::ranges::all_of(
                m_activeTypes, [&](const ComponentType* componentType) { return componentType->has(m_scene, entity); });
        case SelectGroupedType::Children:
        case SelectGroupedType::ImmediateChildren:
            break;
        }
        return false;
    }

  private:
    template <typename Component, typename Value>
    bool sharesAny(Entity entity, std::vector<Value> Component::* list) const {
        const auto* mine = m_registry.try_get<Component>(m_active);
        const auto* theirs = m_registry.try_get<Component>(entity);
        if (mine == nullptr || theirs == nullptr) {
            return false;
        }
        return std::ranges::any_of(mine->*list, [&](const Value& value) {
            return std::ranges::find(theirs->*list, value) != (theirs->*list).end();
        });
    }

    const Scene& m_scene;
    const entt::registry& m_registry;
    Entity m_active;
    std::vector<const ComponentType*> m_activeTypes;
    std::optional<std::string> m_prefab;
    std::optional<ModelHandle> m_model;
};

OperatorResult selectGrouped(OperatorContext& context, const SelectGroupedProperties& values) {
    const Scene& scene = context.editing.scene();
    const Entity active = activeEntity(context);
    Found found(scene);
    if (values.type == SelectGroupedType::Children || values.type == SelectGroupedType::ImmediateChildren) {
        for (const Entity entity : selectedEntities(context)) {
            if (values.type == SelectGroupedType::Children) {
                addDescendants(scene, entity, found);
            } else {
                for (const Entity child : childrenOf(scene, entity)) {
                    found.add(child);
                }
            }
        }
        return selectFound(context, std::move(found.ids()), values.extend);
    }
    // The active object stays active and selected.
    found.add(active);
    const GroupMatch match(context, active);
    for (const Entity entity : scene.registry().view<const SceneEntityIdComponent>()) {
        if (entity != active && match.matches(values.type, entity)) {
            found.add(entity);
        }
    }
    std::vector<SceneEntityId>& ids = found.ids();
    if (ids.size() <= 1) {
        return OperatorResult::cancelled("Nothing else shares that with the active object");
    }
    return selectFound(context, std::move(ids), values.extend);
}

Operator selectHierarchyOperator() {
    return {
        .id = "object.select_hierarchy",
        .label = "Select Hierarchy",
        .description = "Select the children or the parents of the selected objects",
        .properties = OperatorProperties::make(
            SelectHierarchyProperties{},
            std::move(Fields<SelectHierarchyProperties>()
                          .add("direction", &SelectHierarchyProperties::direction)
                          .options({{"children", "Children", static_cast<int64_t>(SelectHierarchyDirection::Children)},
                                    {"parent", "Parent", static_cast<int64_t>(SelectHierarchyDirection::Parent)}})
                          .add("extend", &SelectHierarchyProperties::extend))),
        .poll = hasSelection,
        .execute = typed<SelectHierarchyProperties>(selectHierarchy)};
}

Operator selectGroupedOperator() {
    const auto option = [](const char* key, const char* label, SelectGroupedType type) {
        return EnumOption{.key = key, .label = label, .value = static_cast<int64_t>(type)};
    };
    return {.id = "object.select_grouped",
            .label = "Select Grouped",
            .description = "Select the objects that share a property with the active object",
            .properties = OperatorProperties::make(
                SelectGroupedProperties{},
                std::move(Fields<SelectGroupedProperties>()
                              .add("type", &SelectGroupedProperties::type)
                              .options({option("children", "Children", SelectGroupedType::Children),
                                        option("immediate_children", "Immediate Children",
                                               SelectGroupedType::ImmediateChildren),
                                        option("prefab", "Prefab", SelectGroupedType::Prefab),
                                        option("model", "Model", SelectGroupedType::Model),
                                        option("mesh", "Mesh", SelectGroupedType::Mesh),
                                        option("tag", "Tag", SelectGroupedType::Tag),
                                        option("collection", "Collection", SelectGroupedType::Collection),
                                        option("components", "Components", SelectGroupedType::Components)})
                              .add("extend", &SelectGroupedProperties::extend))),
            .poll = hasActive,
            .execute = typed<SelectGroupedProperties>(selectGrouped)};
}
} // namespace

OperatorRegistrationResult registerSelectOperators(OperatorRegistry& registry) {
    for (Operator& op : std::vector<Operator>{selectHierarchyOperator(), selectGroupedOperator()}) {
        if (OperatorRegistrationResult result = registry.add(std::move(op)); !result) {
            return result;
        }
    }
    return {.success = true};
}

OperatorRegistrationResult registerSelectOperatorFamily(OperatorRegistry& registry, const EditorOperatorServices&) {
    return registerSelectOperators(registry);
}
