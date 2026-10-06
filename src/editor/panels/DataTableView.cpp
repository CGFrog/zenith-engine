#include "DataTableView.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/project/ProjectFileOperations.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

namespace {
using Json = nlohmann::json;

constexpr const char* schemaSettingKey = "schema";
constexpr const char* idKey = "id";
constexpr const char* diagnosticSource = "Data";
// Rows past this are summarized rather than drawn.
constexpr size_t drawnRowLimit = 500;
constexpr size_t drawnColumnLimit = 64;

std::string cellText(const Json& value) {
    return value.is_string() ? value.get<std::string>() : value.dump();
}

bool readText(const std::filesystem::path& path, std::string& text) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    text = std::move(contents).str();
    return true;
}
} // namespace

std::string readDataTable(std::string_view text, std::span<const FieldDescriptor> schemaFields, DataTable& output) {
    output = {};
    Json document;
    try {
        document = Json::parse(text);
    } catch (const Json::parse_error& error) {
        return std::string("not valid JSON: ") + error.what();
    }
    if (!document.is_array()) {
        return "expected an array of entries";
    }
    for (const FieldDescriptor& field : schemaFields) {
        output.columns.push_back(field.key);
    }
    // JSON objects keep their keys sorted, so without a schema the id leads.
    const bool hasId =
        std::ranges::any_of(document, [](const Json& entry) { return entry.is_object() && entry.contains(idKey); });
    if (schemaFields.empty() && hasId) {
        output.columns.emplace_back(idKey);
    }
    for (const Json& entry : document) {
        if (!entry.is_object()) {
            return "entry " + std::to_string(output.rows.size()) + " is not an object";
        }
        for (auto item = entry.begin(); item != entry.end(); ++item) {
            if (std::ranges::find(output.columns, item.key()) == output.columns.end()) {
                output.columns.push_back(item.key());
            }
        }
        output.rows.emplace_back();
    }
    for (size_t row = 0; row < document.size(); ++row) {
        std::vector<std::string>& cells = output.rows[row];
        cells.reserve(output.columns.size());
        for (const std::string& column : output.columns) {
            const auto value = document[row].find(column);
            cells.push_back(value == document[row].end() ? std::string{} : cellText(*value));
        }
    }
    return {};
}

std::string validateDataTableStructure(std::string_view text, std::string_view source) {
    Json document;
    try {
        document = Json::parse(text);
    } catch (const Json::parse_error& error) {
        return std::string(source) + ": not valid JSON: " + error.what();
    }
    if (!document.is_array()) {
        return std::string(source) + ": expected an array of entries";
    }
    std::map<std::string, size_t, std::less<>> seen;
    for (size_t index = 0; index < document.size(); ++index) {
        const Json& entry = document[index];
        const std::string name = std::string(source) + "[" + std::to_string(index) + "]";
        if (!entry.is_object()) {
            return name + ": an entry must be an object";
        }
        const auto id = entry.find(idKey);
        if (id == entry.end()) {
            continue;
        }
        if (!id->is_string() || id->get<std::string>().empty()) {
            return name + ": the id must be a non-empty string";
        }
        if (const auto [previous, added] = seen.emplace(id->get<std::string>(), index); !added) {
            return name + " '" + previous->first + "': duplicate id, also used by entry " +
                   std::to_string(previous->second);
        }
    }
    return {};
}

std::string definitionSchemaSetting(const nlohmann::json& importSettings) {
    if (!importSettings.is_object()) {
        return {};
    }
    const auto schema = importSettings.find(schemaSettingKey);
    return schema != importSettings.end() && schema->is_string() ? schema->get<std::string>() : std::string{};
}

const DefinitionSchema* definitionSchemaFor(const DefinitionSchemaRegistry* schemas,
                                            const nlohmann::json& importSettings, const std::filesystem::path& file) {
    if (schemas == nullptr) {
        return nullptr;
    }
    const std::string named = definitionSchemaSetting(importSettings);
    return schemas->find(named.empty() ? utf8PathText(file.stem()) : named);
}

void DataTableView::update(const std::filesystem::path& file, const DefinitionSchemaRegistry* schemas,
                           const nlohmann::json& importSettings, uint64_t revision, EditorDiagnostics& diagnostics) {
    const DefinitionSchema* schema = definitionSchemaFor(schemas, importSettings, file);
    std::error_code error;
    Key key{.file = file,
            .writeTime = std::filesystem::last_write_time(file, error),
            .size = std::filesystem::file_size(file, error),
            .schema = schema != nullptr ? schema->name : std::string{},
            .registry = schemas,
            .schemaCount = schemas != nullptr ? schemas->schemas().size() : 0,
            .revision = revision};
    if (m_loaded && key == m_key) {
        return;
    }
    m_key = std::move(key);
    m_loaded = true;
    m_schemaName = m_key.schema;
    const std::string source = utf8PathText(file.filename());
    std::string text;
    if (!readText(file, text)) {
        m_table = {};
        m_error = source + ": cannot be read";
    } else {
        const std::string tableError = readDataTable(
            text,
            schema != nullptr ? std::span<const FieldDescriptor>(schema->fields) : std::span<const FieldDescriptor>{},
            m_table);
        if (schema != nullptr) {
            const DefinitionTableResult validated = schema->validate(text, source);
            m_error = validated ? std::string{} : validated.error;
        } else {
            m_error = validateDataTableStructure(text, source);
        }
        if (m_error.empty() && !tableError.empty()) {
            m_error = source + ": " + tableError;
        }
    }
    if (!m_error.empty()) {
        diagnostics.add(EditorDiagnosticSeverity::Warning, diagnosticSource, m_error);
    }
}

void DataTableView::draw(const std::filesystem::path& file, const DefinitionSchemaRegistry* schemas,
                         const nlohmann::json& importSettings, uint64_t revision, EditorDiagnostics& diagnostics) {
    update(file, schemas, importSettings, revision, diagnostics);
    if (m_schemaName.empty()) {
        ImGui::TextDisabled("No schema registered for this table; only its structure is checked.");
    } else {
        ImGui::TextDisabled("Schema: %s", m_schemaName.c_str());
    }
    if (!m_error.empty()) {
        widgets::CopyableText(m_error, palette::error);
    } else {
        ImGui::Text("%zu entries, valid.", m_table.rows.size());
    }
    if (m_table.columns.empty()) {
        return;
    }
    const int columnCount = static_cast<int>(std::min(m_table.columns.size(), drawnColumnLimit));
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                      ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##DataTable", columnCount, flags, {0.0f, 0.0f})) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    for (int column = 0; column < columnCount; ++column) {
        ImGui::TableSetupColumn(m_table.columns[static_cast<size_t>(column)].c_str());
    }
    ImGui::TableHeadersRow();
    const size_t rowCount = std::min(m_table.rows.size(), drawnRowLimit);
    for (size_t row = 0; row < rowCount; ++row) {
        ImGui::TableNextRow();
        for (int column = 0; column < columnCount; ++column) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(m_table.rows[row][static_cast<size_t>(column)].c_str());
        }
    }
    ImGui::EndTable();
    if (m_table.rows.size() > rowCount) {
        ImGui::TextDisabled("%zu more entries not shown.", m_table.rows.size() - rowCount);
    }
}

const DataTable& DataTableView::table() const {
    return m_table;
}

const std::string& DataTableView::error() const {
    return m_error;
}

const std::string& DataTableView::schemaName() const {
    return m_schemaName;
}
