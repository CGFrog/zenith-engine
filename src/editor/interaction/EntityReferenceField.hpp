#pragma once

// The Inspector's entity reference field: names the referenced entity and offers a picker and the eyedropper.

#include "engine/reflection/Reflection.hpp"
#include "engine/scene/Scene.hpp"

#include <functional>
#include <span>
#include <string>
#include <string_view>

class ComponentType;
class EditorDocument;
class EntityEyedropper;
struct EditorContext;
struct EntityReferenceTarget;

// The outliner's drag payload type; the payload is one SceneEntityId::value.
inline constexpr const char* sceneEntityDragPayload = "ZENITH_SCENE_ENTITY";

// What an entity reference field needs beyond its value: the scene, to name
// the referenced entity, and the component being drawn, to name the field
// for the eyedropper. It holds references only and lives for one draw.
struct EntityReferenceFieldContext {
    const Scene& scene;
    // The eyedropper's state, which lights the Pick button of the field it is
    // picking for; null hides the Pick button.
    const EntityEyedropper* eyedropper{nullptr};
    // The document the field belongs to: the button lights only while the
    // eyedropper is armed for it.
    const EditorDocument* document{nullptr};
    // The entity owning the component and the component's type key and
    // fields; the drawn field must be one of these descriptors.
    SceneEntityId owner;
    std::string_view componentKey;
    std::span<const FieldDescriptor> componentFields;
    // Arm the eyedropper for a field, or cancel it.
    std::function<void(const EntityReferenceTarget&)> arm;
    std::function<void()> cancel;
};

// The context for drawing type's fields on owner in the editor, arming and
// cancelling the eyedropper through the ui.eyedropper_* operators.
EntityReferenceFieldContext entityReferenceFieldContext(EditorContext& context, SceneEntityId owner,
                                                        const ComponentType& type);

enum class EntityReferenceState : uint8_t {
    None,
    Present,
    // The id names no entity in the scene, for example a deleted one. The id
    // is kept, so the reference returns if the entity does.
    Missing,
};

EntityReferenceState entityReferenceState(const Scene& scene, SceneEntityId value);
// What the field shows: "None", the entity's name, or "Missing".
std::string entityReferenceText(const Scene& scene, SceneEntityId value);

// Draws one entity reference field: a box naming the referenced entity, which
// accepts an entity dragged from the outliner, a Clear button, and a Pick
// (eyedropper) button, followed by label. Edits value in place and returns
// whether it changed; the caller records undo. A pick completes later,
// through the ui.eyedropper_pick operator. Without a context only the id is
// shown.
bool drawEntityReferenceField(const char* label, SceneEntityId& value, const FieldDescriptor& field,
                              const EntityReferenceFieldContext* context);
