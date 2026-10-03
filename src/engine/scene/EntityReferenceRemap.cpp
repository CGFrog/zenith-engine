#include "EntityReferenceRemap.hpp"

#include "ComponentTypes.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <span>
#include <string>

namespace {
using Json = nlohmann::json;

// Built-in types whose custom codecs write an entity id the reflected fields
// do not describe.
struct CustomReferenceField {
    const char* component;
    const char* field;
};

constexpr std::array<CustomReferenceField, 2> customReferenceFields{{
    {"model_node", "instance_root"},
    {"skin", "instance_root"},
}};

std::string escapePointerToken(std::string_view key) {
    std::string escaped;
    for (const char character : key) {
        if (character == '~') {
            escaped += "~0";
        } else if (character == '/') {
            escaped += "~1";
        } else {
            escaped += character;
        }
    }
    return escaped;
}

void remapValue(Json& value, const EntityReferenceSite& site, const EntityReferenceMapping& mapping) {
    if (!value.is_number_unsigned() || value.get<uint64_t>() == 0) {
        return;
    }
    const std::optional<uint64_t> mapped = mapping(value.get<uint64_t>(), site);
    if (!mapped) {
        return;
    }
    value = *mapped == 0 && site.nullable ? Json(nullptr) : Json(*mapped);
}

void remapFields(Json& object, std::span<const FieldDescriptor> fields, std::string_view component,
                 const std::string& pointer, const EntityReferenceMapping& mapping) {
    if (!object.is_object()) {
        return;
    }
    for (const FieldDescriptor& field : fields) {
        const auto value = object.find(field.key);
        if (value == object.end()) {
            continue;
        }
        const std::string path = pointer + "/" + escapePointerToken(field.key);
        if (field.kind == FieldKind::EntityReference) {
            remapValue(*value, {.component = component, .field = path}, mapping);
        } else if (field.kind == FieldKind::Struct) {
            remapFields(*value, field.children, component, path, mapping);
        }
    }
}
} // namespace

void remapEntityReferences(Json& components, const ComponentTypeRegistry& types,
                           const EntityReferenceMapping& mapping) {
    if (!components.is_object()) {
        return;
    }
    for (auto item = components.begin(); item != components.end(); ++item) {
        if (const ComponentType* type = types.find(item.key())) {
            remapFields(item.value(), type->fields(), item.key(), {}, mapping);
        }
    }
    for (const CustomReferenceField& reference : customReferenceFields) {
        const auto component = components.find(reference.component);
        if (component == components.end() || !component->is_object()) {
            continue;
        }
        if (const auto value = component->find(reference.field); value != component->end()) {
            const std::string path = std::string("/") + reference.field;
            remapValue(*value, {.component = reference.component, .field = path, .nullable = false}, mapping);
        }
    }
}
