#include "FieldJson.hpp"

#include <nlohmann/json.hpp>

#include <limits>

namespace {
using Json = nlohmann::json;

Json floatArray(const float* values, size_t count) {
    Json result = Json::array();
    for (size_t index = 0; index < count; ++index) {
        result.push_back(values[index]);
    }
    return result;
}

bool readFloats(const Json& input, float* values, size_t count) {
    if (!input.is_array() || input.size() != count) {
        return false;
    }
    for (size_t index = 0; index < count; ++index) {
        if (!input[index].is_number()) {
            return false;
        }
        // Out-of-range doubles become infinities, as the original float reads
        // did; owning types reject non-finite values where they matter.
        values[index] = static_cast<float>(input[index].get<double>());
    }
    return true;
}

Json writeField(const void* object, const FieldDescriptor& field) {
    switch (field.kind) {
    case FieldKind::Bool:
        return field.value<bool>(object);
    case FieldKind::Int32:
        return field.value<int32_t>(object);
    case FieldKind::UInt32:
        return field.value<uint32_t>(object);
    case FieldKind::Float:
        return field.value<float>(object);
    case FieldKind::Vec3:
        return floatArray(&field.value<glm::vec3>(object).x, 3);
    case FieldKind::Vec4:
        return floatArray(&field.value<glm::vec4>(object).x, 4);
    case FieldKind::UVec3: {
        const glm::uvec3& value = field.value<glm::uvec3>(object);
        return Json::array({value.x, value.y, value.z});
    }
    case FieldKind::String:
        return field.value<std::string>(object);
    case FieldKind::Enum: {
        const int64_t value = field.enumValue(object);
        if (field.encoding == EnumEncoding::Integer) {
            return value;
        }
        const EnumOption* option = field.findOption(value);
        return option != nullptr ? Json(option->key) : Json(value);
    }
    case FieldKind::FloatRange: {
        void* mutableObject = const_cast<void*>(object);
        return Json::array({*static_cast<float*>(field.address(mutableObject)),
                            *static_cast<float*>(field.secondAddress(mutableObject))});
    }
    case FieldKind::Struct: {
        Json nested;
        writeReflectedFields(&field.value<char>(object), field.children, nested);
        return nested;
    }
    }
    return nullptr;
}

std::string readField(const Json& input, void* object, const FieldDescriptor& field) {
    const auto mismatch = [&field](const char* expected) { return "Field '" + field.key + "' must be " + expected; };
    switch (field.kind) {
    case FieldKind::Bool:
        if (!input.is_boolean()) {
            return mismatch("true or false");
        }
        field.value<bool>(object) = input.get<bool>();
        return {};
    case FieldKind::Int32:
        if (!input.is_number_integer() || input.get<int64_t>() < std::numeric_limits<int32_t>::min() ||
            input.get<int64_t>() > std::numeric_limits<int32_t>::max()) {
            return mismatch("a 32-bit whole number");
        }
        field.value<int32_t>(object) = input.get<int32_t>();
        return {};
    case FieldKind::UInt32:
        if (!input.is_number_unsigned() || input.get<uint64_t>() > std::numeric_limits<uint32_t>::max()) {
            return mismatch("a non-negative 32-bit whole number");
        }
        field.value<uint32_t>(object) = input.get<uint32_t>();
        return {};
    case FieldKind::Float:
        if (!input.is_number()) {
            return mismatch("a number");
        }
        field.value<float>(object) = static_cast<float>(input.get<double>());
        return {};
    case FieldKind::Vec3:
        return readFloats(input, &field.value<glm::vec3>(object).x, 3) ? std::string{} : mismatch("three numbers");
    case FieldKind::Vec4:
        return readFloats(input, &field.value<glm::vec4>(object).x, 4) ? std::string{} : mismatch("four numbers");
    case FieldKind::UVec3: {
        if (!input.is_array() || input.size() != 3) {
            return mismatch("three non-negative whole numbers");
        }
        glm::uvec3 value;
        for (glm::length_t index = 0; index < 3; ++index) {
            if (!input[index].is_number_unsigned() ||
                input[index].get<uint64_t>() > std::numeric_limits<uint32_t>::max()) {
                return mismatch("three non-negative whole numbers");
            }
            value[index] = input[index].get<uint32_t>();
        }
        field.value<glm::uvec3>(object) = value;
        return {};
    }
    case FieldKind::String:
        if (!input.is_string()) {
            return mismatch("a string");
        }
        field.value<std::string>(object) = input.get<std::string>();
        return {};
    case FieldKind::Enum: {
        for (const EnumOption& option : field.options) {
            const bool matches = field.encoding == EnumEncoding::Integer
                                     ? input.is_number_integer() && input.get<int64_t>() == option.value
                                     : input.is_string() && input.get<std::string>() == option.key;
            if (matches) {
                field.setEnumValue(object, option.value);
                return {};
            }
        }
        std::string expected = "one of";
        for (const EnumOption& option : field.options) {
            expected += ' ';
            expected += field.encoding == EnumEncoding::Integer ? std::to_string(option.value) : option.key;
        }
        return mismatch(expected.c_str());
    }
    case FieldKind::FloatRange: {
        float values[2];
        if (!readFloats(input, values, 2)) {
            return mismatch("a [minimum, maximum] pair");
        }
        *static_cast<float*>(field.address(object)) = values[0];
        *static_cast<float*>(field.secondAddress(object)) = values[1];
        return {};
    }
    case FieldKind::Struct:
        if (std::string error = readReflectedFields(input, &field.value<char>(object), field.children);
            !error.empty()) {
            return field.key + ": " + error;
        }
        return {};
    }
    return mismatch("a supported value");
}
} // namespace

void writeReflectedFields(const void* object, std::span<const FieldDescriptor> fields, Json& output) {
    output = Json::object();
    for (const FieldDescriptor& field : fields) {
        output[field.key] = writeField(object, field);
    }
}

std::string readReflectedFields(const Json& input, void* object, std::span<const FieldDescriptor> fields) {
    if (!input.is_object()) {
        return "Expected an object of fields";
    }
    for (const FieldDescriptor& field : fields) {
        const auto found = input.find(field.key);
        if (found == input.end()) {
            if (field.optional) {
                continue;
            }
            return "Missing required field '" + field.key + "'";
        }
        if (std::string error = readField(*found, object, field); !error.empty()) {
            return error;
        }
    }
    return {};
}
