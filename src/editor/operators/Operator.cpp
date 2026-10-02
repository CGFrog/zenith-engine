#include "Operator.hpp"

#include "engine/reflection/FieldJson.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>

std::string OperatorProperties::applyJson(std::string_view json) {
    if (json.empty()) {
        return {};
    }
    const nlohmann::json parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return "Operator properties must be a JSON object";
    }
    if (parsed.empty()) {
        return {};
    }
    if (empty()) {
        return "The operator has no properties";
    }
    // Only the named fields change, so every field is read as optional.
    std::vector<FieldDescriptor> named;
    for (const FieldDescriptor& field : fields()) {
        if (parsed.contains(field.key)) {
            named.push_back(field);
        }
    }
    if (named.size() != parsed.size()) {
        for (const auto& [key, value] : parsed.items()) {
            if (std::ranges::none_of(named, [&key](const FieldDescriptor& field) { return field.key == key; })) {
                return "Unknown operator property '" + key + "'";
            }
        }
    }
    return readReflectedFields(parsed, object(), named);
}

std::string OperatorProperties::toJson() const {
    nlohmann::json output = nlohmann::json::object();
    if (!empty()) {
        writeReflectedFields(object(), fields(), output);
    }
    return output.dump();
}
