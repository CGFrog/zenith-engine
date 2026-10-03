#include "EntityEyedropper.hpp"

#include "engine/scene/ComponentTypes.hpp"

#include <algorithm>
#include <utility>

namespace {
const FieldDescriptor* findChild(std::span<const FieldDescriptor> fields, std::string_view key) {
    for (const FieldDescriptor& field : fields) {
        if (field.key == key) {
            return &field;
        }
    }
    return nullptr;
}
} // namespace

std::string joinFieldPath(std::span<const std::string> path) {
    std::string text;
    for (const std::string& key : path) {
        if (!text.empty()) {
            text += '.';
        }
        text += key;
    }
    return text;
}

std::vector<std::string> splitFieldPath(std::string_view text) {
    std::vector<std::string> path;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = std::min(text.find('.', start), text.size());
        if (end == start) {
            return {};
        }
        path.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    return path;
}

std::optional<std::vector<std::string>> findFieldPath(std::span<const FieldDescriptor> fields,
                                                      const FieldDescriptor& field) {
    for (const FieldDescriptor& candidate : fields) {
        if (&candidate == &field) {
            return std::vector<std::string>{candidate.key};
        }
        if (std::optional<std::vector<std::string>> nested = findFieldPath(candidate.children, field)) {
            nested->insert(nested->begin(), candidate.key);
            return nested;
        }
    }
    return std::nullopt;
}

ResolvedEntityReference resolveEntityReference(Scene& scene, const ComponentTypeRegistry& types,
                                               const EntityReferenceTarget& target) {
    ResolvedEntityReference resolved;
    resolved.type = types.find(target.componentKey);
    if (resolved.type == nullptr) {
        resolved.error = "No component type '" + target.componentKey + "' is registered";
        return resolved;
    }
    const Entity entity = scene.findEntity(target.entity);
    if (entity == entt::null) {
        resolved.error = "The entity owning the reference no longer exists";
        return resolved;
    }
    resolved.component = resolved.type->find(scene, entity);
    if (resolved.component == nullptr) {
        resolved.error = "The entity no longer has a " + resolved.type->displayName() + " component";
        return resolved;
    }
    const std::string pathText = joinFieldPath(target.fieldPath);
    void* object = resolved.component;
    std::span<const FieldDescriptor> fields = resolved.type->fields();
    for (size_t index = 0; index < target.fieldPath.size(); ++index) {
        const FieldDescriptor* field = findChild(fields, target.fieldPath[index]);
        const bool last = index + 1 == target.fieldPath.size();
        if (field == nullptr || (!last && field->kind != FieldKind::Struct)) {
            break;
        }
        if (last) {
            if (field->kind == FieldKind::EntityReference) {
                resolved.field = field;
                resolved.value = &field->value<SceneEntityId>(object);
                return resolved;
            }
            break;
        }
        object = field->address(object);
        fields = field->children;
    }
    resolved.error = resolved.type->displayName() + " has no entity reference field '" + pathText + "'";
    return resolved;
}

void EntityEyedropper::arm(EntityReferenceTarget target) {
    m_target = std::move(target);
}

void EntityEyedropper::cancel() {
    m_target.reset();
}
