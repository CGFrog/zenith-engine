#include "VisibilityOperators.hpp"

#include "engine/scene/Collections.hpp"
#include "engine/scene/Scene.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace {
bool hasSelection(const OperatorContext& context) {
    return context.editMode() && !context.selection.empty();
}

bool hidden(const Scene& scene, Entity entity) {
    const auto* visibility = scene.registry().try_get<VisibilityComponent>(entity);
    return visibility != nullptr && visibility->hidden;
}

bool anyHidden(const OperatorContext& context) {
    if (!context.editMode()) {
        return false;
    }
    const auto view = context.editing.scene().registry().view<const VisibilityComponent>();
    return std::ranges::any_of(view,
                               [&view](Entity entity) { return view.get<const VisibilityComponent>(entity).hidden; });
}

// Sets the editor hide flag, keeping the component only while it differs
// from the defaults so unhidden entities save as they did before.
void setHidden(Scene& scene, Entity entity, bool value) {
    entt::registry& registry = scene.registry();
    VisibilityComponent& visibility = registry.get_or_emplace<VisibilityComponent>(entity);
    visibility.hidden = value;
    if (visibility == VisibilityComponent{}) {
        registry.remove<VisibilityComponent>(entity);
    }
    scene.markRenderableDirty(entity);
    scene.markLightDirty(entity);
}

// Hides each listed entity that is not hidden yet; returns how many changed.
size_t hideEntities(Scene& scene, const std::vector<Entity>& entities) {
    size_t changed = 0;
    for (const Entity entity : entities) {
        if (!hidden(scene, entity)) {
            setHidden(scene, entity, true);
            ++changed;
        }
    }
    return changed;
}

void deselectHidden(OperatorContext& context) {
    const Scene& scene = context.editing.scene();
    std::vector<SceneEntityId> kept;
    for (const SceneEntityId id : context.selection.ids()) {
        const Entity entity = scene.findEntity(id);
        if (entity != entt::null && !hidden(scene, entity)) {
            kept.push_back(id);
        }
    }
    context.selection.assign(std::move(kept));
}

Operator hideOperator() {
    return {.id = "object.hide",
            .label = "Hide Selected",
            .description = "Hide the selected objects in the editor viewports (the game still shows them)",
            .flags = OperatorFlags::Undo,
            .poll = hasSelection,
            .execute = [](OperatorContext& context, OperatorProperties&) {
                Scene& scene = context.editing.scene();
                std::vector<Entity> selected;
                for (const SceneEntityId id : context.selection.ids()) {
                    if (const Entity entity = scene.findEntity(id); entity != entt::null) {
                        selected.push_back(entity);
                    }
                }
                if (hideEntities(scene, selected) == 0) {
                    return OperatorResult::cancelled("The selected objects are already hidden");
                }
                deselectHidden(context);
                return OperatorResult::finished();
            }};
}

Operator isolateOperator() {
    return {.id = "object.isolate",
            .label = "Hide Unselected",
            .description = "Hide every object that is not selected in the editor viewports",
            .flags = OperatorFlags::Undo,
            .poll = hasSelection,
            .execute = [](OperatorContext& context, OperatorProperties&) {
                Scene& scene = context.editing.scene();
                std::vector<Entity> unselected;
                for (const Entity entity : scene.registry().view<const SceneEntityIdComponent>()) {
                    if (!context.selection.contains(scene.id(entity))) {
                        unselected.push_back(entity);
                    }
                }
                if (hideEntities(scene, unselected) == 0) {
                    return OperatorResult::cancelled("Every unselected object is already hidden");
                }
                return OperatorResult::finished();
            }};
}

Operator unhideOperator() {
    return {.id = "object.unhide",
            .label = "Reveal Hidden",
            .description = "Show every object hidden in the editor viewports",
            .flags = OperatorFlags::Undo | OperatorFlags::Register,
            .properties = OperatorProperties::make(
                UnhideProperties{}, std::move(Fields<UnhideProperties>().add("select", &UnhideProperties::select))),
            .poll = anyHidden,
            .execute = [](OperatorContext& context, OperatorProperties& properties) {
                const UnhideProperties* values = properties.get<UnhideProperties>();
                if (values == nullptr) {
                    return OperatorResult::failed("Operator properties have the wrong type");
                }
                Scene& scene = context.editing.scene();
                std::vector<Entity> revealed;
                for (const Entity entity : scene.registry().view<const VisibilityComponent>()) {
                    if (hidden(scene, entity)) {
                        revealed.push_back(entity);
                    }
                }
                if (revealed.empty()) {
                    return OperatorResult::cancelled("Nothing is hidden");
                }
                std::vector<SceneEntityId> ids;
                for (const Entity entity : revealed) {
                    setHidden(scene, entity, false);
                    ids.push_back(scene.id(entity));
                }
                if (values->select) {
                    std::ranges::sort(ids);
                    context.selection.assign(std::move(ids));
                }
                return OperatorResult::finished();
            }};
}
} // namespace

OperatorRegistrationResult registerVisibilityOperators(OperatorRegistry& registry) {
    for (Operator op : {hideOperator(), unhideOperator(), isolateOperator()}) {
        if (OperatorRegistrationResult result = registry.add(std::move(op)); !result) {
            return result;
        }
    }
    return {.success = true};
}
