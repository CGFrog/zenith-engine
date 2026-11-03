#include "Prefab.hpp"
#include "PrefabSystemInternal.hpp"

#include <glm/gtc/type_ptr.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace {
using Json = nlohmann::json;

bool readMatrix(const Json& value, glm::mat4& output) {
    if (!value.is_array() || value.size() != 16) {
        return false;
    }
    for (size_t index = 0; index < 16; ++index) {
        if (!value[index].is_number()) {
            return false;
        }
        glm::value_ptr(output)[index] = value[index].get<float>();
    }
    return true;
}

bool hasHierarchyCycle(const std::vector<PrefabEntity>& entities) {
    std::unordered_map<uint64_t, std::optional<uint64_t>> parents;
    for (const PrefabEntity& entity : entities) {
        parents.emplace(entity.id.value, entity.parent ? std::optional(entity.parent->value) : std::nullopt);
    }
    std::unordered_set<uint64_t> visiting;
    std::unordered_set<uint64_t> visited;
    std::function<bool(uint64_t)> visit = [&](uint64_t id) {
        if (visited.contains(id)) {
            return false;
        }
        if (!visiting.insert(id).second) {
            return true;
        }
        const auto parent = parents.at(id);
        if (parent && visit(*parent)) {
            return true;
        }
        visiting.erase(id);
        visited.insert(id);
        return false;
    };
    for (const PrefabEntity& entity : entities) {
        if (visit(entity.id.value)) {
            return true;
        }
    }
    return false;
}

// --- Version 1 to 2 ---------------------------------------------------------
// The version 1 component and property names are frozen with that format.

Json migrateVersion1PointLight(const Json& light) {
    return {{"color", light.at("color")},
            {"intensity", light.at("intensity")},
            {"range", light.at("range")},
            {"casts_shadows", light.value("castsShadows", false)},
            {"shadow_resolution", light.value("shadowResolution", 0u)},
            {"shadow_priority", light.value("shadowPriority", 0)}};
}

Json migrateVersion1Camera(const Json& camera) {
    return {{"vertical_fov", camera.at("verticalFov")},
            {"near_plane", camera.at("nearPlane")},
            {"far_plane", camera.at("farPlane")},
            {"primary", camera.value("primary", false)}};
}

// The version 2 key and payload of a version 1 component, or nullopt for a
// component version 1 never supported (kept unchanged).
std::optional<std::pair<std::string, Json>> migrateVersion1Component(const std::string& key, const Json& payload) {
    if (key == "pointLight") {
        return std::pair<std::string, Json>{"point_light",
                                            payload.is_null() ? payload : migrateVersion1PointLight(payload)};
    }
    if (key == "camera") {
        return std::pair<std::string, Json>{"camera", payload.is_null() ? payload : migrateVersion1Camera(payload)};
    }
    return std::nullopt;
}

void migrateVersion1Override(Json& overrideJson, std::vector<std::string>& diagnostics) {
    const std::string property = overrideJson.value("property", std::string{});
    Json value = overrideJson.contains("value") ? overrideJson["value"] : Json(nullptr);
    Json migrated = {{"entity", overrideJson.value("entity", uint64_t{0})}};
    constexpr std::string_view componentPrefix = "component.";
    if (property == "name") {
        migrated["component"] = "";
        migrated["field"] = "/name";
    } else if (property == "localTransform") {
        migrated["component"] = "";
        migrated["field"] = "/local_transform";
    } else if (property.starts_with(componentPrefix)) {
        const auto component = migrateVersion1Component(property.substr(componentPrefix.size()), value);
        if (!component) {
            diagnostics.push_back("Dropped unsupported version 1 prefab override: " + property);
            overrideJson = nullptr;
            return;
        }
        migrated["component"] = component->first;
        migrated["field"] = "";
        value = component->second;
    } else {
        diagnostics.push_back("Dropped unsupported version 1 prefab override: " + property);
        overrideJson = nullptr;
        return;
    }
    migrated["value"] = std::move(value);
    overrideJson = std::move(migrated);
}

// Upgrades a version 1 document in place. Extra parentless entities, which
// version 1 placed beside the root, become children of the first root.
void migrateVersion1To2(Json& document, std::vector<std::string>& diagnostics) {
    std::optional<uint64_t> firstRoot;
    for (Json& entity : document["entities"]) {
        if (!entity.is_object()) {
            continue;
        }
        if (const auto transform = entity.find("localTransform"); transform != entity.end()) {
            entity["local_transform"] = *transform;
            entity.erase("localTransform");
        }
        Json components = Json::object();
        if (const auto existing = entity.find("components"); existing != entity.end() && existing->is_object()) {
            for (auto component = existing->begin(); component != existing->end(); ++component) {
                if (auto migrated = migrateVersion1Component(component.key(), component.value())) {
                    components[migrated->first] = std::move(migrated->second);
                } else {
                    components[component.key()] = component.value();
                }
            }
        }
        if (const auto model = entity.find("modelAsset"); model != entity.end()) {
            components["model"] = Json{{"path", *model}};
            entity.erase("modelAsset");
        }
        entity["components"] = std::move(components);
        const auto parent = entity.find("parent");
        if (parent == entity.end() || parent->is_null()) {
            if (!firstRoot) {
                firstRoot = entity.value("id", uint64_t{0});
            } else {
                entity["parent"] = *firstRoot;
                diagnostics.push_back("Version 1 prefab entity " + std::to_string(entity.value("id", uint64_t{0})) +
                                      " was a second root; it is now a child of the first root");
            }
        }
    }
    if (const auto nested = document.find("nested"); nested != document.end() && nested->is_array()) {
        for (Json& record : *nested) {
            const auto overrides = record.find("overrides");
            if (overrides == record.end() || !overrides->is_array()) {
                continue;
            }
            Json migrated = Json::array();
            for (Json& overrideJson : *overrides) {
                migrateVersion1Override(overrideJson, diagnostics);
                if (!overrideJson.is_null()) {
                    migrated.push_back(std::move(overrideJson));
                }
            }
            *overrides = std::move(migrated);
        }
    }
    document["version"] = 2;
}

PrefabResult readEntity(const Json& item, PrefabEntity& entity) {
    entity = {};
    if (!item.is_object()) {
        return {.error = "Prefab entities must be objects"};
    }
    entity.id.value = item.value("id", uint64_t{0});
    entity.name = item.value("name", std::string{});
    if (const auto parent = item.find("parent"); parent != item.end() && !parent->is_null()) {
        entity.parent = PrefabEntityId{parent->get<uint64_t>()};
    }
    if (!readMatrix(item.at("local_transform"), entity.localTransform)) {
        return {.error = "Prefab local_transform must contain 16 numbers"};
    }
    if (const auto components = item.find("components"); components != item.end()) {
        if (!components->is_object()) {
            return {.error = "Prefab components must be an object"};
        }
        for (auto component = components->begin(); component != components->end(); ++component) {
            entity.components.emplace_back(component.key(), component.value().dump());
        }
        std::ranges::sort(entity.components, {}, &std::pair<std::string, std::string>::first);
    }
    return {.success = true};
}

PrefabResult validateEntities(const std::vector<PrefabEntity>& entities) {
    std::unordered_set<uint64_t> ids;
    for (const PrefabEntity& entity : entities) {
        if (!entity.id || !ids.insert(entity.id.value).second) {
            return {.error = "Prefab entity IDs must be non-zero and unique"};
        }
    }
    size_t roots = 0;
    for (const PrefabEntity& entity : entities) {
        if (!entity.parent) {
            ++roots;
        } else if (!ids.contains(entity.parent->value)) {
            return {.error = "Prefab entity refers to a missing parent"};
        }
    }
    if (hasHierarchyCycle(entities)) {
        return {.error = "Prefab entity hierarchy contains a cycle"};
    }
    if (roots != 1) {
        return {.error = "A prefab must have exactly one root entity"};
    }
    return {.success = true};
}

PrefabResult readNested(const Json& item, const std::unordered_set<uint64_t>& entityIds, NestedPrefab& nested) {
    nested.id = item.value("id", uint64_t{0});
    nested.source = item.value("source", std::string{});
    if (nested.id == 0 || nested.source.empty() || nested.source.is_absolute()) {
        return {.error = "Nested prefab requires a non-zero ID and a relative source"};
    }
    if (const auto parent = item.find("parent"); parent != item.end() && !parent->is_null()) {
        nested.parent = PrefabEntityId{parent->get<uint64_t>()};
        if (!entityIds.contains(nested.parent->value)) {
            return {.error = "Nested prefab refers to a missing parent"};
        }
    }
    for (const Json& overrideJson : item.value("overrides", Json::array())) {
        PrefabOverride overrideValue;
        if (PrefabResult result = prefabOverrideFromJson(overrideJson, overrideValue); !result) {
            return result;
        }
        nested.overrides.push_back(std::move(overrideValue));
    }
    return {.success = true};
}

PrefabResult replaceFile(const std::filesystem::path& temporary, const std::filesystem::path& destination) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return {.error = "Could not atomically replace prefab (Windows error " + std::to_string(GetLastError()) + ")"};
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        return {.error = "Could not atomically replace prefab: " + error.message()};
    }
#endif
    return {.success = true};
}
} // namespace

uint64_t prefabNestedReference(uint64_t record, PrefabEntityId local) {
    constexpr uint64_t nestedReferenceTag = uint64_t{1} << 62u;
    if (record == 0 || record >= (uint64_t{1} << 30u) || !local || local.value >= (uint64_t{1} << 32u)) {
        return 0;
    }
    return nestedReferenceTag | (record << 32u) | local.value;
}

Json prefabOverrideToJson(const PrefabOverride& value) {
    Json parsed = Json::parse(value.jsonValue, nullptr, false);
    Json result = {{"component", value.component},
                   {"field", value.field},
                   {"value", parsed.is_discarded() ? Json(nullptr) : std::move(parsed)}};
    if (value.nested != 0) {
        result["nested"] = value.nested;
    } else {
        result["entity"] = value.entity.value;
    }
    return result;
}

PrefabResult prefabOverrideFromJson(const Json& json, PrefabOverride& output) {
    if (!json.is_object() || !json.contains("value")) {
        return {.error = "Prefab overrides must be objects with a value"};
    }
    PrefabOverride parsed{.entity = PrefabEntityId{json.value("entity", uint64_t{0})},
                          .component = json.value("component", std::string{}),
                          .field = json.value("field", std::string{}),
                          .jsonValue = json.at("value").dump(),
                          .nested = json.value("nested", uint64_t{0})};
    if (parsed.nested != 0) {
        if (parsed.entity || !parsed.component.empty() || parsed.field != "/removed") {
            return {.error = "Nested record overrides must target /removed and no entity"};
        }
        output = std::move(parsed);
        return {.success = true};
    }
    if (!parsed.entity) {
        return {.error = "Prefab overrides require an entity ID"};
    }
    if (parsed.component.empty() && parsed.field != "/name" && parsed.field != "/local_transform" &&
        parsed.field != "/parent" && parsed.field != "/removed") {
        return {.error = "Entity overrides must target /name, /local_transform, /parent, or /removed, not '" +
                         parsed.field + "'"};
    }
    if (!parsed.field.empty()) {
        try {
            [[maybe_unused]] const Json::json_pointer pointer(parsed.field);
        } catch (const Json::exception&) {
            return {.error = "Prefab override field is not a JSON pointer: " + parsed.field};
        }
    }
    output = std::move(parsed);
    return {.success = true};
}

PrefabResult prefabEntityToJson(const PrefabEntity& entity, Json& output) {
    Json components = Json::object();
    for (const auto& [type, payload] : entity.components) {
        Json component = Json::parse(payload, nullptr, false);
        if (component.is_discarded()) {
            return {.error = "Prefab component contains invalid JSON: " + type};
        }
        components[type] = std::move(component);
    }
    output = {{"id", entity.id.value},
              {"name", entity.name},
              {"parent", entity.parent ? Json(entity.parent->value) : Json(nullptr)},
              {"local_transform", prefab_detail::matrixJson(entity.localTransform)},
              {"components", std::move(components)}};
    return {.success = true};
}

PrefabResult prefabEntityFromJson(const Json& json, PrefabEntity& output) {
    try {
        return readEntity(json, output);
    } catch (const Json::exception& error) {
        return {.error = std::string("Invalid prefab entity: ") + error.what()};
    }
}

PrefabResult serializePrefab(const PrefabDocument& prefab, std::string& output) {
    Json document = {
        {"version", PrefabDocument::currentVersion}, {"entities", Json::array()}, {"nested", Json::array()}};
    for (const PrefabEntity& entity : prefab.entities) {
        Json encoded;
        if (PrefabResult result = prefabEntityToJson(entity, encoded); !result) {
            return result;
        }
        document["entities"].push_back(std::move(encoded));
    }
    for (const NestedPrefab& nested : prefab.nested) {
        Json overrides = Json::array();
        for (const PrefabOverride& overrideValue : nested.overrides) {
            if (Json::parse(overrideValue.jsonValue, nullptr, false).is_discarded()) {
                return {.error = "Prefab override contains invalid JSON"};
            }
            overrides.push_back(prefabOverrideToJson(overrideValue));
        }
        document["nested"].push_back({{"id", nested.id},
                                      {"source", nested.source.generic_string()},
                                      {"parent", nested.parent ? Json(nested.parent->value) : Json(nullptr)},
                                      {"overrides", std::move(overrides)}});
    }
    if (!prefab.displayName.empty()) {
        document["metadata"] = {{"display_name", prefab.displayName}};
    }
    output = document.dump(2);
    return {.success = true};
}

PrefabResult deserializePrefab(std::string_view input, PrefabDocument& prefab) {
    try {
        Json document = Json::parse(input);
        if (!document.is_object() || !document.contains("entities") || !document["entities"].is_array() ||
            !document.contains("version") || !document["version"].is_number_unsigned()) {
            return {.error = "Malformed prefab document: it needs a version and an entities array"};
        }
        const uint64_t version = document["version"].get<uint64_t>();
        if (version == 0 || version > PrefabDocument::currentVersion) {
            return {.error = "Unsupported prefab document version " + std::to_string(version) +
                             ": this build reads versions 1 to " + std::to_string(PrefabDocument::currentVersion)};
        }
        PrefabResult result{.success = true};
        if (version == 1) {
            migrateVersion1To2(document, result.diagnostics);
        }

        PrefabDocument loaded;
        for (const Json& item : document["entities"]) {
            PrefabEntity entity;
            if (PrefabResult read = readEntity(item, entity); !read) {
                return read;
            }
            loaded.entities.push_back(std::move(entity));
        }
        if (PrefabResult valid = validateEntities(loaded.entities); !valid) {
            return valid;
        }
        std::unordered_set<uint64_t> entityIds;
        for (const PrefabEntity& entity : loaded.entities) {
            entityIds.insert(entity.id.value);
        }
        std::unordered_set<uint64_t> nestedIds;
        for (const Json& item : document.value("nested", Json::array())) {
            NestedPrefab nested;
            if (PrefabResult read = readNested(item, entityIds, nested); !read) {
                return read;
            }
            if (!nestedIds.insert(nested.id).second) {
                return {.error = "Nested prefab IDs must be unique"};
            }
            loaded.nested.push_back(std::move(nested));
        }
        if (const auto metadata = document.find("metadata"); metadata != document.end() && metadata->is_object()) {
            loaded.displayName = metadata->value("display_name", std::string{});
        }
        prefab = std::move(loaded);
        return result;
    } catch (const Json::exception& error) {
        return {.error = std::string("Could not parse prefab: ") + error.what()};
    }
}

PrefabResult savePrefab(const std::filesystem::path& path, const PrefabDocument& prefab) {
    std::string serialized;
    if (PrefabResult result = serializePrefab(prefab, serialized); !result) {
        return result;
    }
    std::error_code error;
    if (const std::filesystem::path parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            return {.error = "Could not create prefab directory: " + error.message()};
        }
    }
    // The asset database skips .tmp files, so the partial file is never
    // tracked as an asset.
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << serialized << '\n';
        output.flush();
        if (!output) {
            std::filesystem::remove(temporary, error);
            return {.error = "Could not write prefab: " + path.string()};
        }
    }
    if (PrefabResult result = replaceFile(temporary, path); !result) {
        std::filesystem::remove(temporary, error);
        return result;
    }
    return {.success = true};
}

PrefabResult loadPrefab(const std::filesystem::path& path, PrefabDocument& prefab) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {.error = "Could not open prefab: " + path.string()};
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    PrefabResult result = deserializePrefab(contents.str(), prefab);
    if (!result) {
        result.error = path.filename().string() + ": " + result.error;
    }
    return result;
}
