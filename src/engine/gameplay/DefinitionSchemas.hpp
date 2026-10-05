#pragma once

#include "engine/gameplay/DefinitionTable.hpp"

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// A named definition table layout a game registers so tools can check its
// data files without the game's types: the entry fields (for column names
// and order), the id key, and a validator that parses text exactly as
// DefinitionTable<Entry>::parse does.
//
// Lifetime: validate is code from whoever registered the schema (a game
// module), so a schema must not outlive that code. Copy names and fields out
// freely; never keep validate.
struct DefinitionSchema {
    std::string name;
    std::vector<FieldDescriptor> fields;
    std::string idKey{"id"};
    // Parses text (source names it in errors) as a table of these entries.
    std::function<DefinitionTableResult(std::string_view text, std::string_view source)> validate;
};

// A DefinitionSchema for Entry, validating with a DefinitionTable<Entry>.
template <typename Entry>
DefinitionSchema makeDefinitionSchema(std::string name, Fields<Entry> fields, std::string idKey = "id") {
    std::vector<FieldDescriptor> descriptors = Fields<Entry>(fields).build();
    return {.name = std::move(name),
            .fields = std::move(descriptors),
            .idKey = idKey,
            .validate = [fields = std::move(fields), idKey](std::string_view text,
                                                            std::string_view source) -> DefinitionTableResult {
                DefinitionTable<Entry> table(fields, idKey);
                return table.parse(text, source);
            }};
}

// The definition schemas a game registered, by unique name.
class DefinitionSchemaRegistry {
  public:
    // Rejects an empty name, a name already registered, a schema without a
    // validator, and fields unusable for a table keyed by idKey.
    DefinitionTableResult add(DefinitionSchema schema);
    // The schema called name, or null.
    const DefinitionSchema* find(std::string_view name) const;
    // In registration order.
    std::span<const DefinitionSchema> schemas() const;

  private:
    std::vector<DefinitionSchema> m_schemas;
};
