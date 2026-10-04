#include "EyedropperOperators.hpp"

#include "editor/document/EditorDocument.hpp"
#include "editor/interaction/EntityEyedropper.hpp"
#include "editor/operators/EditorOperatorFamilies.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include <utility>

namespace {
ResolvedEntityReference resolve(const OperatorContext& context, const EntityReferenceTarget& target) {
    return resolveEntityReference(context.editing.scene(), componentTypesFor(context.editing.document().options()),
                                  target);
}

Operator armOperator(EntityEyedropper& eyedropper) {
    return {.id = "ui.eyedropper_entity",
            .label = "Pick Entity Reference",
            .description = "Fill an entity reference field with the next entity clicked in a viewport or the outliner",
            .flags = OperatorFlags::Internal,
            .properties = OperatorProperties::make(EyedropperArmProperties{},
                                                   std::move(Fields<EyedropperArmProperties>()
                                                                 .add("entity", &EyedropperArmProperties::entity)
                                                                 .add("component", &EyedropperArmProperties::component)
                                                                 .add("field", &EyedropperArmProperties::field))),
            .poll = [](const OperatorContext& context) { return context.editMode(); },
            .execute =
                [&eyedropper](OperatorContext& context, OperatorProperties& properties) {
                    const auto* values = properties.get<EyedropperArmProperties>();
                    if (values == nullptr) {
                        return OperatorResult::failed("Operator properties have the wrong type");
                    }
                    EntityReferenceTarget target{.entity = values->entity,
                                                 .componentKey = values->component,
                                                 .fieldPath = splitFieldPath(values->field)};
                    if (const ResolvedEntityReference resolved = resolve(context, target); !resolved) {
                        return OperatorResult::failed("Cannot pick an entity reference: " + resolved.error);
                    }
                    eyedropper.arm(std::move(target));
                    return OperatorResult::finished();
                }};
}

Operator pickOperator(EntityEyedropper& eyedropper) {
    return {.id = "ui.eyedropper_pick",
            .label = "Set Entity Reference",
            .description = "Store an entity in the field the eyedropper is picking for",
            .flags = OperatorFlags::Undo | OperatorFlags::Internal,
            .properties = OperatorProperties::make(
                EyedropperPickProperties{},
                std::move(Fields<EyedropperPickProperties>().add("entity", &EyedropperPickProperties::entity))),
            .poll = [&eyedropper](const OperatorContext& context) { return context.editMode() && eyedropper.armed(); },
            .execute =
                [&eyedropper](OperatorContext& context, OperatorProperties& properties) {
                    const auto* values = properties.get<EyedropperPickProperties>();
                    if (values == nullptr) {
                        return OperatorResult::failed("Operator properties have the wrong type");
                    }
                    if (!eyedropper.armed()) {
                        return OperatorResult::cancelled("The eyedropper is not picking a field");
                    }
                    // Disarmed whatever happens: a pick is one attempt.
                    const EntityReferenceTarget target = *eyedropper.target();
                    eyedropper.cancel();
                    const ResolvedEntityReference resolved = resolve(context, target);
                    if (!resolved) {
                        return OperatorResult::failed("Cannot set the entity reference: " + resolved.error);
                    }
                    if (context.editing.scene().findEntity(values->entity) == entt::null) {
                        return OperatorResult::failed("The picked entity does not exist");
                    }
                    if (*resolved.value == values->entity) {
                        return OperatorResult::cancelled("The field already references that entity");
                    }
                    *resolved.value = values->entity;
                    resolved.type->sanitize(resolved.component);
                    return OperatorResult::finished();
                }};
}

Operator cancelOperator(EntityEyedropper& eyedropper) {
    return {.id = "ui.eyedropper_cancel",
            .label = "Cancel Entity Eyedropper",
            .description = "Stop picking an entity for a reference field",
            .poll = [&eyedropper](const OperatorContext&) { return eyedropper.armed(); },
            .execute =
                [&eyedropper](OperatorContext&, OperatorProperties&) {
                    eyedropper.cancel();
                    return OperatorResult::finished();
                }};
}
} // namespace

OperatorRegistrationResult registerEyedropperOperators(OperatorRegistry& registry, EntityEyedropper& eyedropper) {
    for (Operator op : {armOperator(eyedropper), pickOperator(eyedropper), cancelOperator(eyedropper)}) {
        if (OperatorRegistrationResult result = registry.add(std::move(op)); !result) {
            return result;
        }
    }
    return {.success = true};
}

OperatorRegistrationResult registerEyedropperOperatorFamily(OperatorRegistry& registry,
                                                            const EditorOperatorServices& services) {
    return registerEyedropperOperators(registry, services.eyedropper);
}
