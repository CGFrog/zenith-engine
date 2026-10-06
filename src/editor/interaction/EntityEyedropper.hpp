#pragma once

// The entity eyedropper: pick an entity in a viewport to fill an entity reference field.

#include "engine/reflection/Reflection.hpp"
#include "engine/scene/Scene.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class ComponentType;
class ComponentTypeRegistry;
class EditorDocument;

// Names one entity reference field by stable keys only, so it stays valid
// across undo, redo, and game module reloads: the entity owning the
// component, the component's type key, and the field keys from the
// component's fields down to the reference (more than one for a field in a
// nested struct).
struct EntityReferenceTarget {
    SceneEntityId entity;
    std::string componentKey;
    std::vector<std::string> fieldPath;

    bool operator==(const EntityReferenceTarget&) const = default;
};

// "settings.target" <-> {"settings", "target"}. Parsing rejects empty keys
// by returning an empty path.
std::string joinFieldPath(std::span<const std::string> path);
std::vector<std::string> splitFieldPath(std::string_view text);

// The keys leading from fields to field, found by identity (field must be one
// of the descriptors in the tree, not a copy); nullopt when it is not there.
std::optional<std::vector<std::string>> findFieldPath(std::span<const FieldDescriptor> fields,
                                                      const FieldDescriptor& field);

// A target looked up in a scene: the reference value to edit in place, or
// what made the target unusable.
struct ResolvedEntityReference {
    SceneEntityId* value{nullptr};
    const ComponentType* type{nullptr};
    void* component{nullptr};
    const FieldDescriptor* field{nullptr};
    std::string error;

    explicit operator bool() const {
        return value != nullptr;
    }
};

// Looks target up now. The component type is read from types on every call;
// nothing is cached, so the result is valid only until the scene or registry
// changes.
ResolvedEntityReference resolveEntityReference(Scene& scene, const ComponentTypeRegistry& types,
                                               const EntityReferenceTarget& target);

// The armed state of the entity eyedropper: a pick request for one entity
// reference field. While armed, the next entity clicked in a viewport or the
// outliner of the field's document fills the field (through the
// ui.eyedropper_pick operator) instead of changing the selection. The session
// owns one; operators arm, pick, and cancel it.
//
// Entity ids are per scene, so the eyedropper remembers the document whose
// field it picks for (owner) and only that document's clicks pick.
class EntityEyedropper {
  public:
    void arm(EntityReferenceTarget target, const EditorDocument* owner);
    void cancel();

    bool armed() const {
        return m_target.has_value();
    }

    // Whether it is armed for a field of document.
    bool armedFor(const EditorDocument& document) const {
        return armed() && m_owner == &document;
    }

    // The document it is armed for, or null. Compared only: it may be gone.
    const EditorDocument* owner() const {
        return m_target ? m_owner : nullptr;
    }

    // The armed target, or null.
    const EntityReferenceTarget* target() const {
        return m_target ? &*m_target : nullptr;
    }

    // A right-click that cancelled the eyedropper is still held (or released
    // this frame), so it must not also open a context menu.
    bool cancellingClickHeld() const {
        return m_cancellingClickHeld;
    }

    void setCancellingClickHeld(bool held) {
        m_cancellingClickHeld = held;
    }

  private:
    std::optional<EntityReferenceTarget> m_target;
    const EditorDocument* m_owner{nullptr};
    bool m_cancellingClickHeld{false};
};
