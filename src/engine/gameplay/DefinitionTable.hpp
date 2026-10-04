#pragma once

#include "engine/reflection/Reflection.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Data-driven game content: a JSON file holding an array of objects, each one
// entry of a reflected struct (an item, an enemy, a loot table), looked up by
// a string id. The game defines the struct and its fields; the table loads,
// validates, and reloads it. Lists of nested structs (Fields::list) describe
// entries such as a loot table's drops.
//
// Loading is strict, because the file is written by hand: an unknown key (a
// typo), a wrong type, a missing required field, and a missing or duplicate
// id are errors naming the file, the entry, and the field. A failed load
// leaves the table as it was.

struct DefinitionTableResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// Where a definition file stood when it was last read: its write time and size.
struct DefinitionFileStamp {
    std::filesystem::file_time_type writeTime{};
    uintmax_t size{0};

    bool operator==(const DefinitionFileStamp&) const = default;
};

namespace definition_detail {
// Parses text, from source (named in errors), into entries through resize and
// element: one entry per array element, each read through fields. ids receives
// each entry's idKey value. Does not validate the descriptors.
DefinitionTableResult parseEntries(std::string_view text, std::string_view source,
                                   std::span<const FieldDescriptor> fields, std::string_view idKey,
                                   const std::function<void(size_t)>& resize,
                                   const std::function<void*(size_t)>& element, std::vector<std::string>& ids);
// Empty when fields are usable for a table keyed by idKey: valid descriptors
// with a required String field idKey.
std::string validateTableFields(std::span<const FieldDescriptor> fields, std::string_view idKey);
DefinitionTableResult readFile(const std::filesystem::path& path, std::string& text);
// Nullopt when the file cannot be inspected (missing, for example).
std::optional<DefinitionFileStamp> stampOf(const std::filesystem::path& path);
// path as UTF-8 with forward slashes, for errors.
std::string sourceName(const std::filesystem::path& path);
} // namespace definition_detail

template <typename Entry> class DefinitionTable {
  public:
    // fields describe Entry; idKey names its string id field.
    explicit DefinitionTable(Fields<Entry> fields, std::string idKey = "id")
        : m_fields(std::move(fields).build())
        , m_idKey(std::move(idKey)) {}

    // Reads the table from path, which reloadIfChanged then watches.
    DefinitionTableResult load(const std::filesystem::path& path) {
        m_path = path;
        m_stamp = definition_detail::stampOf(path);
        std::string text;
        if (DefinitionTableResult result = definition_detail::readFile(path, text); !result) {
            return result;
        }
        return parse(text, definition_detail::sourceName(path));
    }

    // Reads the table from text; source names it in errors.
    DefinitionTableResult parse(std::string_view text, std::string_view source) {
        if (std::string error = definition_detail::validateTableFields(m_fields, m_idKey); !error.empty()) {
            return {.error = std::string(source) + ": " + error};
        }
        std::vector<Entry> entries;
        std::vector<std::string> ids;
        DefinitionTableResult result = definition_detail::parseEntries(
            text, source, m_fields, m_idKey, [&entries](size_t size) { entries.resize(size); },
            [&entries](size_t index) -> void* { return &entries[index]; }, ids);
        if (!result) {
            return result;
        }
        m_entries = std::move(entries);
        m_index.clear();
        for (size_t index = 0; index < ids.size(); ++index) {
            m_index.emplace(std::move(ids[index]), index);
        }
        return result;
    }

    // Loads the file again when its write time or size changed since the last
    // attempt; nullopt when it did not (or nothing was loaded). A game calls
    // it from a system, for example once a second in the editor, to pick up
    // edits while playing; a failed reload keeps the previous entries.
    std::optional<DefinitionTableResult> reloadIfChanged() {
        if (m_path.empty()) {
            return std::nullopt;
        }
        const std::optional<DefinitionFileStamp> stamp = definition_detail::stampOf(m_path);
        if (stamp == m_stamp) {
            return std::nullopt;
        }
        return load(m_path);
    }

    // The entry with id, or null.
    const Entry* find(std::string_view id) const {
        const auto found = m_index.find(id);
        return found != m_index.end() ? &m_entries[found->second] : nullptr;
    }

    // Every entry, in file order. Valid until the next successful load.
    std::span<const Entry> entries() const {
        return m_entries;
    }

    size_t size() const {
        return m_entries.size();
    }

    const std::filesystem::path& path() const {
        return m_path;
    }

  private:
    std::vector<FieldDescriptor> m_fields;
    std::string m_idKey;
    std::vector<Entry> m_entries;
    std::map<std::string, size_t, std::less<>> m_index;
    std::filesystem::path m_path;
    std::optional<DefinitionFileStamp> m_stamp;
};
