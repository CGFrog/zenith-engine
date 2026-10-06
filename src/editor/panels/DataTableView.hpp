#pragma once

// Shows and validates a definition-table JSON file in the Asset Browser.

#include "engine/gameplay/DefinitionSchemas.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class EditorDiagnostics;

// The Asset Browser's view of a definition table (EditorAssetKind::Data): the
// file's entries as a read-only table and whether they load. A table is
// validated against the DefinitionSchema the game module registered for it
// (ModuleRegistrar::addDefinitionTable): the one its import settings name
// (`{"schema": "<name>"}`), else one named like the file's stem
// ("enemies.json" uses "enemies"). Without a schema only the structure is
// checked: an array of objects whose "id" values, where present, are unique
// non-empty strings.

// Cell text: strings unquoted, other values as compact JSON.
struct DataTable {
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
};

// Reads text as a table. Columns are the schema's field keys first (in field
// order), or without a schema "id" when entries have one; then the other
// keys, entry by entry, each entry's keys by name (JSON objects keep no key
// order). Returns an empty string, or why text is not an array of objects.
std::string readDataTable(std::string_view text, std::span<const FieldDescriptor> schemaFields, DataTable& output);

// The structural check used without a schema (see above). Returns an empty
// string, or an error naming source and the entry.
std::string validateDataTableStructure(std::string_view text, std::string_view source);

// The schema a table file uses (see above), or null. importSettings is the
// file's import settings object.
const DefinitionSchema* definitionSchemaFor(const DefinitionSchemaRegistry* schemas,
                                            const nlohmann::json& importSettings, const std::filesystem::path& file);

// The name of the schema importSettings names, or empty.
std::string definitionSchemaSetting(const nlohmann::json& importSettings);

// Draws one table at a time and keeps what it read until the file, its
// schema, or the revision (ProjectAssets::dataRevision) changes. New
// validation errors are reported once as warnings. Holds no schema or
// validator between frames, so a game module may unload at any time.
class DataTableView {
  public:
    // Reads and validates file again when needed, then draws it.
    void draw(const std::filesystem::path& file, const DefinitionSchemaRegistry* schemas,
              const nlohmann::json& importSettings, uint64_t revision, EditorDiagnostics& diagnostics);
    // The reading and validation part of draw, for tests.
    void update(const std::filesystem::path& file, const DefinitionSchemaRegistry* schemas,
                const nlohmann::json& importSettings, uint64_t revision, EditorDiagnostics& diagnostics);

    const DataTable& table() const;
    // Why the file did not read or validate; empty when it is valid.
    const std::string& error() const;
    // The schema it was validated against; empty for the structural check.
    const std::string& schemaName() const;

  private:
    struct Key {
        std::filesystem::path file;
        std::filesystem::file_time_type writeTime{};
        uintmax_t size{0};
        std::string schema;
        // Schemas are identified by name plus the registry they came from, so
        // a reloaded module validates again.
        const DefinitionSchemaRegistry* registry{nullptr};
        size_t schemaCount{0};
        uint64_t revision{0};

        bool operator==(const Key&) const = default;
    };

    Key m_key;
    bool m_loaded{false};
    DataTable m_table;
    std::string m_error;
    std::string m_schemaName;
};
