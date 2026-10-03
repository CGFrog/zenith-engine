#include "Reflection.hpp"

#include <cctype>
#include <unordered_set>

int64_t FieldDescriptor::enumValue(const void* object) const {
    return readEnum(address(const_cast<void*>(object)));
}

void FieldDescriptor::setEnumValue(void* object, int64_t value) const {
    writeEnum(address(object), value);
}

const EnumOption* FieldDescriptor::findOption(int64_t value) const {
    for (const EnumOption& option : options) {
        if (option.value == value) {
            return &option;
        }
    }
    return nullptr;
}

std::string labelFromKey(std::string_view key) {
    std::string label;
    label.reserve(key.size());
    for (const char character : key) {
        label.push_back(character == '_' ? ' ' : character);
    }
    if (!label.empty()) {
        label.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(label.front())));
    }
    return label;
}

std::string validateFieldDescriptors(const std::vector<FieldDescriptor>& fields) {
    std::unordered_set<std::string_view> keys;
    for (const FieldDescriptor& field : fields) {
        if (field.key.empty()) {
            return "A reflected field has an empty key";
        }
        if (!keys.insert(field.key).second) {
            return "Duplicate reflected field '" + field.key + "'";
        }
        if (field.kind == FieldKind::Enum && field.options.empty()) {
            return "Enum field '" + field.key + "' lists no options";
        }
        if (field.kind == FieldKind::EntityReference && (field.presentation.angle || field.presentation.color)) {
            return "Entity reference field '" + field.key + "' cannot be shown as an angle or a color";
        }
        if (field.kind == FieldKind::Struct) {
            if (std::string error = validateFieldDescriptors(field.children); !error.empty()) {
                return field.key + "." + error;
            }
        }
    }
    return {};
}
