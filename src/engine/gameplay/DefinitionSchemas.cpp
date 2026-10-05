#include "DefinitionSchemas.hpp"

#include <algorithm>
#include <utility>

DefinitionTableResult DefinitionSchemaRegistry::add(DefinitionSchema schema) {
    if (schema.name.empty()) {
        return {.error = "A definition schema needs a name"};
    }
    if (find(schema.name) != nullptr) {
        return {.error = "Definition schema '" + schema.name + "' is registered twice"};
    }
    if (!schema.validate) {
        return {.error = "Definition schema '" + schema.name + "' has no validator"};
    }
    if (std::string error = definition_detail::validateTableFields(schema.fields, schema.idKey); !error.empty()) {
        return {.error = "Definition schema '" + schema.name + "': " + error};
    }
    m_schemas.push_back(std::move(schema));
    return {.success = true};
}

const DefinitionSchema* DefinitionSchemaRegistry::find(std::string_view name) const {
    const auto found = std::ranges::find(m_schemas, name, &DefinitionSchema::name);
    return found != m_schemas.end() ? &*found : nullptr;
}

std::span<const DefinitionSchema> DefinitionSchemaRegistry::schemas() const {
    return m_schemas;
}
