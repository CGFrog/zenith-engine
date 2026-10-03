#pragma once

#include "engine/reflection/Reflection.hpp"
#include "engine/scene/Scene.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class ComponentType;
class ComponentTypeRegistry;

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
// outliner fills the field (through the ui.eyedropper_pick operator) instead
// of changing the selection. The session owns one; operators arm, pick, and
// cancel it.
class EntityEyedropper {
  public:
    void arm(EntityReferenceTarget target);
    void cancel();

    bool armed() const {
        return m_target.has_value();
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
    bool m_cancellingClickHeld{false};
};
