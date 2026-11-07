#include "AnimationOperators.hpp"

#include "editor/operators/EditorOperatorFamilies.hpp"
#include "engine/animation/Animator.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/scene/Scene.hpp"

#include <utility>

namespace {
Operator previewStateOperator() {
    using P = AnimatorPreviewProperties;
    return {.id = "animation.preview_state",
            .label = "Preview Animator State",
            .description = "Play an animator state's clip on its object from the start",
            .flags = OperatorFlags::Undo | OperatorFlags::Internal,
            .properties =
                OperatorProperties::make(P{}, std::move(Fields<P>().add("entity", &P::entity).add("state", &P::state))),
            .execute =
                [](OperatorContext& context, OperatorProperties& properties) {
                    const auto* values = properties.get<P>();
                    if (values == nullptr) {
                        return OperatorResult::failed("Operator properties have the wrong type");
                    }
                    Scene& scene = context.editing.scene();
                    const Entity entity = scene.findEntity(values->entity);
                    if (entity == entt::null) {
                        return OperatorResult::failed("The object does not exist");
                    }
                    entt::registry& registry = scene.registry();
                    auto* animator = registry.try_get<AnimatorComponent>(entity);
                    if (animator == nullptr) {
                        return OperatorResult::failed("The object has no Animator");
                    }
                    auto* animation = registry.try_get<AnimationComponent>(entity);
                    const auto* model = registry.try_get<ModelComponent>(entity);
                    const ModelAsset* asset = model != nullptr ? context.editing.assets().model(model->model) : nullptr;
                    if (animation == nullptr || asset == nullptr) {
                        return OperatorResult::failed("The object needs a loaded model with an Animation component");
                    }
                    if (!enterAnimatorState(*animator, *animation, asset->animations, values->state)) {
                        return OperatorResult::failed("The animator has no state '" + values->state +
                                                      "' whose clip the model has");
                    }
                    return OperatorResult::finished();
                },
            .scope =
                [](const OperatorContext&, const OperatorProperties& properties) {
                    const auto* values = properties.get<P>();
                    return values != nullptr ? OperatorScope::of({values->entity}) : OperatorScope::whole();
                }};
}
} // namespace

OperatorRegistrationResult registerAnimationOperators(OperatorRegistry& registry) {
    return addOperators(registry, {previewStateOperator()});
}

OperatorRegistrationResult registerAnimationOperatorFamily(OperatorRegistry& registry, const EditorOperatorServices&) {
    return registerAnimationOperators(registry);
}
