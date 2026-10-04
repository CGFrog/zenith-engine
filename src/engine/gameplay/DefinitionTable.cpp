#include "DefinitionTable.hpp"

#include "engine/reflection/FieldJson.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <system_error>

namespace definition_detail {
namespace {
using Json = nlohmann::json;

const FieldDescriptor* findField(std::span<const FieldDescriptor> fields, std::string_view key) {
    for (const FieldDescriptor& field : fields) {
        if (field.key == key) {
            return &field;
        }
    }
    return nullptr;
}

// The first key of object, or of a struct or list element inside it, that
// fields do not name, as a path ("drops[1].wieght"). Values of the wrong type
// are left to readReflectedFields, which reports them.
std::optional<std::string> unknownKey(const Json& object, std::span<const FieldDescriptor> fields) {
    if (!object.is_object()) {
        return std::nullopt;
    }
    for (auto item = object.begin(); item != object.end(); ++item) {
        const FieldDescriptor* field = findField(fields, item.key());
        if (field == nullptr) {
            return item.key();
        }
        if (field->kind == FieldKind::Struct) {
            if (std::optional<std::string> nested = unknownKey(item.value(), field->children)) {
                return field->key + "." + *nested;
            }
        } else if (field->kind == FieldKind::List && item.value().is_array()) {
            for (size_t index = 0; index < item.value().size(); ++index) {
                if (std::optional<std::string> nested = unknownKey(item.value()[index], field->children)) {
                    return field->key + "[" + std::to_string(index) + "]." + *nested;
                }
            }
        }
    }
    return std::nullopt;
}

// "items.json[3]", with the entry's id when it has one: "items.json[3] 'sword'".
std::string entryName(std::string_view source, size_t index, const Json& entry, std::string_view idKey) {
    std::string name = std::string(source) + "[" + std::to_string(index) + "]";
    if (entry.is_object()) {
        if (const auto id = entry.find(idKey);
            id != entry.end() && id->is_string() && !id->get<std::string>().empty()) {
            name += " '" + id->get<std::string>() + "'";
        }
    }
    return name;
}
} // namespace

DefinitionTableResult parseEntries(std::string_view text, std::string_view source,
                                   std::span<const FieldDescriptor> fields, std::string_view idKey,
                                   const std::function<void(size_t)>& resize,
                                   const std::function<void*(size_t)>& element, std::vector<std::string>& ids) {
    Json document;
    try {
        document = Json::parse(text);
    } catch (const Json::parse_error& error) {
        return {.error = std::string(source) + ": not valid JSON: " + error.what()};
    }
    if (!document.is_array()) {
        return {.error = std::string(source) + ": expected an array of entries"};
    }
    const FieldDescriptor* idField = findField(fields, idKey);
    resize(document.size());
    ids.clear();
    ids.reserve(document.size());
    std::map<std::string, size_t, std::less<>> seen;
    for (size_t index = 0; index < document.size(); ++index) {
        const Json& entry = document[index];
        const std::string name = entryName(source, index, entry, idKey);
        if (!entry.is_object()) {
            return {.error = name + ": an entry must be an object"};
        }
        if (std::optional<std::string> unknown = unknownKey(entry, fields)) {
            return {.error = name + ": unknown field '" + *unknown + "'"};
        }
        void* object = element(index);
        if (std::string error = readReflectedFields(entry, object, fields); !error.empty()) {
            return {.error = name + ": " + error};
        }
        std::string id = idField->value<std::string>(object);
        if (id.empty()) {
            return {.error = name + ": the id '" + std::string(idKey) + "' is empty"};
        }
        if (const auto previous = seen.find(id); previous != seen.end()) {
            return {.error = name + ": duplicate id, also used by entry " + std::to_string(previous->second)};
        }
        seen.emplace(id, index);
        ids.push_back(std::move(id));
    }
    return {.success = true};
}

std::string validateTableFields(std::span<const FieldDescriptor> fields, std::string_view idKey) {
    if (std::string error = validateFieldDescriptors(std::vector<FieldDescriptor>(fields.begin(), fields.end()));
        !error.empty()) {
        return error;
    }
    const FieldDescriptor* idField = findField(fields, idKey);
    if (idField == nullptr || idField->kind != FieldKind::String || idField->optional || idField->runtimeState) {
        return "the entries need a required string field '" + std::string(idKey) + "' as their id";
    }
    return {};
}

DefinitionTableResult readFile(const std::filesystem::path& path, std::string& text) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {.error = sourceName(path) + ": cannot be read"};
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    text = std::move(contents).str();
    return {.success = true};
}

std::optional<DefinitionFileStamp> stampOf(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(path, error);
    if (error) {
        return std::nullopt;
    }
    const uintmax_t size = std::filesystem::file_size(path, error);
    if (error) {
        return std::nullopt;
    }
    return DefinitionFileStamp{.writeTime = writeTime, .size = size};
}

std::string sourceName(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return {text.begin(), text.end()};
}
} // namespace definition_detail
