#include "OutlinerOperators.hpp"

#include "editor/document/EditorDocument.hpp"
#include "editor/panels/outliner/OutlinerFilter.hpp"
#include "engine/scene/Collections.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/Prefab.hpp"
#include "engine/scene/Scene.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
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

bool inEditMode(const OperatorContext& context) {
    return context.editMode();
}

bool hasSelection(const OperatorContext& context) {
    return context.editMode() && !context.selection.empty();
}

bool& visibilityField(VisibilityComponent& visibility, EntityVisibilityFlag flag) {
    switch (flag) {
    case EntityVisibilityFlag::Selectable:
        return visibility.selectable;
    case EntityVisibilityFlag::Renderable:
        return visibility.renderable;
    case EntityVisibilityFlag::Visible:
        break;
    }
    return visibility.hidden;
}

// The stored value for a flag: Visible is kept as its inverse, `hidden`.
bool storedValue(EntityVisibilityFlag flag, bool value) {
    return flag == EntityVisibilityFlag::Visible ? !value : value;
}

Operator setVisibilityOperator() {
    return {.id = "object.set_visibility",
            .label = "Set Object Visibility",
            .description = "Show or hide an object in viewports, or change whether it is selectable or rendered",
            .flags = OperatorFlags::Undo | OperatorFlags::Internal,
            .properties = OperatorProperties::make(
                EntityVisibilityProperties{},
                std::move(
                    Fields<EntityVisibilityProperties>()
                        .add("entity", &EntityVisibilityProperties::entity)
                        .add("flag", &EntityVisibilityProperties::flag)
                        .options({{"visible", "Visible", static_cast<int64_t>(EntityVisibilityFlag::Visible)},
                                  {"selectable", "Selectable", static_cast<int64_t>(EntityVisibilityFlag::Selectable)},
                                  {"renderable", "Renderable", static_cast<int64_t>(EntityVisibilityFlag::Renderable)}})
                        .add("value", &EntityVisibilityProperties::value))),
            .poll = inEditMode,
            .execute =
                typed<EntityVisibilityProperties>([](OperatorContext& context, EntityVisibilityProperties& values) {
                    Scene& scene = context.editing.scene();
                    const Entity entity = scene.findEntity(values.entity);
                    if (entity == entt::null) {
                        return OperatorResult::failed("Unknown object " + std::to_string(values.entity.value));
                    }
                    entt::registry& registry = scene.registry();
                    const VisibilityComponent* current = registry.try_get<VisibilityComponent>(entity);
                    VisibilityComponent visibility = current != nullptr ? *current : VisibilityComponent{};
                    bool& field = visibilityField(visibility, values.flag);
                    const bool stored = storedValue(values.flag, values.value);
                    if (field == stored) {
                        return OperatorResult::cancelled("The object already has that setting");
                    }
                    field = stored;
                    // Only differences from the defaults are kept, as object.hide does.
                    if (visibility == VisibilityComponent{}) {
                        registry.remove<VisibilityComponent>(entity);
                    } else {
                        registry.emplace_or_replace<VisibilityComponent>(entity, visibility);
                    }
                    scene.markRenderableDirty(entity);
                    scene.markLightDirty(entity);
                    // Hidden objects cannot be picked, so they leave the selection.
                    if (visibility.hidden && context.selection.contains(values.entity)) {
                        std::vector<SceneEntityId> kept = context.selection.ids();
                        std::erase(kept, values.entity);
                        context.selection.assign(std::move(kept));
                    }
                    return OperatorResult::finished();
                })};
}

bool isAncestorOrSelf(const Scene& scene, Entity candidate, Entity entity) {
    for (Entity current = entity; current != entt::null;
         current = scene.registry().get<RelationshipComponent>(current).parent) {
        if (current == candidate) {
            return true;
        }
    }
    return false;
}

Operator parentDropOperator() {
    return {.id = "outliner.parent_drop",
            .label = "Drop to Set Parent",
            .description = "Parent the selected objects to an object, or to the scene root",
            .flags = OperatorFlags::Undo | OperatorFlags::Internal,
            .properties = OperatorProperties::make(
                ParentDropProperties{}, std::move(Fields<ParentDropProperties>()
                                                      .add("parent", &ParentDropProperties::parent)
                                                      .add("keep_transform", &ParentDropProperties::keepTransform))),
            .poll = hasSelection,
            .execute = typed<ParentDropProperties>([](OperatorContext& context, ParentDropProperties& values) {
                Scene& scene = context.editing.scene();
                Entity parent = entt::null;
                if (values.parent) {
                    parent = scene.findEntity(values.parent);
                    if (parent == entt::null) {
                        return OperatorResult::failed("Unknown parent " + std::to_string(values.parent.value));
                    }
                }
                const ReparentMode mode =
                    values.keepTransform ? ReparentMode::PreserveWorldTransform : ReparentMode::PreserveLocalTransform;
                // Fail before recording prefab moves: a failed operator keeps
                // its changes without an undo step.
                if (mode == ReparentMode::PreserveWorldTransform && parent != entt::null) {
                    scene.updateWorldTransforms();
                    if (std::abs(glm::determinant(scene.registry().get<TransformComponent>(parent).world)) < 1.0e-8f) {
                        return OperatorResult::failed("Cannot preserve world transform under a singular parent "
                                                      "transform");
                    }
                }
                bool changed = false;
                // Selected descendants of other selected objects move with them.
                for (const SceneEntityId id : context.selection.transformRoots(scene)) {
                    const Entity child = scene.findEntity(id);
                    // An object cannot become its own or its descendant's child.
                    if (parent != entt::null && isAncestorOrSelf(scene, child, parent)) {
                        continue;
                    }
                    if (scene.registry().get<RelationshipComponent>(child).parent == parent) {
                        continue;
                    }
                    // Leaving a prefab instance removes the object from it.
                    recordPrefabEntityMove(scene, child, parent);
                    if (const SceneResult result = scene.reparent(child, parent, mode); !result) {
                        return OperatorResult::failed(result.error);
                    }
                    changed = true;
                }
                return changed ? OperatorResult::finished()
                               : OperatorResult::cancelled("The selection already has that parent");
            })};
}

// The object's position among its parent's children.
size_t siblingIndex(const Scene& scene, Entity entity) {
    const Entity parent = scene.registry().get<RelationshipComponent>(entity).parent;
    const auto& siblings = scene.registry().get<RelationshipComponent>(parent).children;
    return static_cast<size_t>(std::ranges::find(siblings, entity) - siblings.begin());
}

OperatorResult reorder(OperatorContext& context, const ReorderProperties& values) {
    Scene& scene = context.editing.scene();
    const Entity entity = scene.findEntity(values.entity);
    if (entity == entt::null) {
        return OperatorResult::failed("Unknown object " + std::to_string(values.entity.value));
    }
    if (!values.parent) {
        return OperatorResult::cancelled("Root objects are listed by id and have no order");
    }
    const Entity parent = scene.findEntity(values.parent);
    if (parent == entt::null) {
        return OperatorResult::failed("Unknown parent " + std::to_string(values.parent.value));
    }
    if (isAncestorOrSelf(scene, entity, parent)) {
        return OperatorResult::cancelled("An object cannot be placed under itself");
    }
    const bool moves = scene.registry().get<RelationshipComponent>(entity).parent != parent;
    if (!moves) {
        const size_t last = scene.registry().get<RelationshipComponent>(parent).children.size() - 1;
        if (siblingIndex(scene, entity) == std::min<size_t>(values.index, last)) {
            return OperatorResult::cancelled("The object is already there");
        }
    } else {
        const ReparentMode mode =
            values.keepTransform ? ReparentMode::PreserveWorldTransform : ReparentMode::PreserveLocalTransform;
        if (mode == ReparentMode::PreserveWorldTransform) {
            scene.updateWorldTransforms();
            if (std::abs(glm::determinant(scene.registry().get<TransformComponent>(parent).world)) < 1.0e-8f) {
                return OperatorResult::failed("Cannot preserve world transform under a singular parent transform");
            }
        }
        // Leaving a prefab instance removes the object from it.
        recordPrefabEntityMove(scene, entity, parent);
        if (const SceneResult result = scene.reparent(entity, parent, mode); !result) {
            return OperatorResult::failed(result.error);
        }
    }
    if (const SceneResult result = scene.setSiblingIndex(entity, values.index); !result) {
        return OperatorResult::failed(result.error);
    }
    return OperatorResult::finished();
}

Operator reorderOperator() {
    return {.id = "outliner.reorder",
            .label = "Reorder Object",
            .description = "Move an object to a position among a parent's children",
            .flags = OperatorFlags::Undo | OperatorFlags::Internal,
            .properties = OperatorProperties::make(
                ReorderProperties{}, std::move(Fields<ReorderProperties>()
                                                   .add("entity", &ReorderProperties::entity)
                                                   .add("parent", &ReorderProperties::parent)
                                                   .add("index", &ReorderProperties::index)
                                                   .add("keep_transform", &ReorderProperties::keepTransform))),
            .poll = inEditMode,
            .execute = typed<ReorderProperties>(reorder)};
}

Operator selectMatchingOperator() {
    return {.id = "outliner.select_matching",
            .label = "Select All Matching",
            .description = "Select every object the outliner search matches",
            .properties = OperatorProperties::make(
                SelectMatchingProperties{},
                std::move(Fields<SelectMatchingProperties>().add("filter", &SelectMatchingProperties::filter))),
            .execute = typed<SelectMatchingProperties>([](OperatorContext& context, SelectMatchingProperties& values) {
                const OutlinerFilter filter = parseOutlinerFilter(values.filter);
                if (filter.empty()) {
                    return OperatorResult::cancelled("Type a search to select what it matches");
                }
                const ComponentTypeRegistry& types = componentTypesFor(context.editing.document().options());
                std::vector<SceneEntityId> matches = findOutlinerMatches(context.editing.scene(), filter, types);
                if (matches.empty()) {
                    return OperatorResult::cancelled("Nothing matches the search");
                }
                context.selection.assign(std::move(matches));
                return OperatorResult::finished();
            })};
}
} // namespace

OperatorRegistrationResult registerOutlinerOperators(OperatorRegistry& registry) {
    for (Operator op : {setVisibilityOperator(), parentDropOperator(), reorderOperator(), selectMatchingOperator()}) {
        if (OperatorRegistrationResult result = registry.add(std::move(op)); !result) {
            return result;
        }
    }
    return {.success = true};
}
