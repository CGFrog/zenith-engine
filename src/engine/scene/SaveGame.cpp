#include "SaveGame.hpp"

#include "Scene.hpp"
#include "engine/platform/PlatformServices.hpp"
#include "engine/platform/VersionedJson.hpp"
#include "engine/reflection/FieldJson.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>

namespace {
using Json = nlohmann::json;

constexpr std::string_view saveFormat = "zenith-save";
constexpr uint32_t saveVersion = 1;
constexpr std::string_view saveExtension = ".zsave";
constexpr std::string_view profileExtension = ".zprofile";
constexpr std::string_view sceneKind = "scene";

int64_t secondsSinceEpoch() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Empty when format can save game data.
std::string formatError(const SaveDataFormat& format) {
    if (format.kind.empty() || format.kind == sceneKind) {
        return "Save data needs a kind other than 'scene'";
    }
    if (format.version == 0) {
        return "Save data '" + format.kind + "' needs a version of 1 or more";
    }
    return {};
}

// Brings data saved at savedVersion up to format.version; empty on success.
// owner names the slot or profile in errors.
std::string migrateData(Json& data, uint32_t savedVersion, const SaveDataFormat& format, const std::string& owner) {
    if (savedVersion > format.version) {
        return owner + " was saved by a newer version of the game (" + format.kind + " version " +
               std::to_string(savedVersion) + ")";
    }
    if (savedVersion == format.version) {
        return {};
    }
    if (!format.migrate) {
        return owner + " holds " + format.kind + " version " + std::to_string(savedVersion) +
               ", which this game cannot upgrade";
    }
    if (std::string error = format.migrate(data, savedVersion); !error.empty()) {
        return owner + ": " + error;
    }
    return {};
}

VersionedJsonFormat profileFileFormat(const SaveDataFormat& format) {
    return {.description = "game profile",
            .currentVersion = static_cast<int>(format.version),
            .missing = MissingJsonFile::UseDefaults,
            .backup = UserDataBackup::Keep};
}

std::optional<std::string> readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

// The save's JSON, from its file or, when that is missing or unreadable, its
// backup from the previous save.
std::optional<Json> readSave(const std::filesystem::path& path) {
    for (const std::filesystem::path& candidate : {path, std::filesystem::path(path.string() + ".bak")}) {
        const std::optional<std::string> text = readText(candidate);
        if (!text) {
            continue;
        }
        Json parsed = Json::parse(*text, nullptr, false);
        if (parsed.is_object() && parsed.value("format", std::string{}) == saveFormat) {
            return parsed;
        }
    }
    return std::nullopt;
}

SaveGameInfo infoOf(const Json& save, std::string slot) {
    return {.slot = std::move(slot),
            .kind = save.value("kind", std::string(sceneKind)),
            .scene = std::filesystem::path(save.value("scene", std::string{})),
            .title = save.value("title", std::string{}),
            .savedAt = save.value("saved_at", int64_t{0})};
}
} // namespace

SaveGameStore::SaveGameStore(std::filesystem::path directory, std::filesystem::path contentRoot)
    : m_directory(std::move(directory))
    , m_contentRoot(std::move(contentRoot)) {}

bool SaveGameStore::validSlotName(std::string_view slot) {
    return !slot.empty() && slot.size() <= 64 && std::ranges::all_of(slot, [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '-' || character == '_';
    });
}

const std::filesystem::path& SaveGameStore::directory() const {
    return m_directory;
}

std::filesystem::path SaveGameStore::pathOf(std::string_view slot) const {
    return m_directory / (std::string(slot) + std::string(saveExtension));
}

SaveGameResult SaveGameStore::save(std::string_view slot, const Scene& scene, const AssetManager& assets,
                                   const SceneDocumentOptions& options, const std::filesystem::path& baseline,
                                   std::string title) const {
    if (!validSlotName(slot)) {
        return {.error = "Save slot '" + std::string(slot) + "' is not a valid name"};
    }
    const std::optional<std::string> baselineText = readText(baseline);
    if (!baselineText) {
        return {.error = "Cannot read the saved scene's baseline " + baseline.string()};
    }
    SceneDocumentOptions saveOptions = options;
    saveOptions.purpose = SceneDocumentPurpose::SaveGame;
    std::string current;
    if (SceneDocumentResult result = SceneDocument::serialize(scene, assets, saveOptions, current); !result) {
        return {.error = result.error};
    }
    SceneChanges changes;
    if (SceneDocumentResult result = SceneDocument::diff(*baselineText, current, changes); !result) {
        return {.error = result.error};
    }

    // Only each entity's new state: loading applies the changes forward.
    Json entities = Json::array();
    for (const SceneEntityChange& change : changes.entities) {
        entities.push_back({{"id", change.id.value}, {"after", change.after ? Json::parse(*change.after) : Json()}});
    }
    std::error_code error;
    const std::filesystem::path relative =
        m_contentRoot.empty() ? std::filesystem::path{} : std::filesystem::relative(baseline, m_contentRoot, error);
    const bool inside = !relative.empty() && !error && *relative.begin() != "..";
    Json save = {{"format", saveFormat},
                 {"version", saveVersion},
                 {"kind", sceneKind},
                 {"title", std::move(title)},
                 {"saved_at", secondsSinceEpoch()},
                 {"scene", (inside ? relative : std::filesystem::absolute(baseline)).generic_string()},
                 {"entities", std::move(entities)}};
    if (changes.collections) {
        save["collections"] = Json::parse(changes.collections->after);
    }
    const std::string text = save.dump(2);
    const PlatformResult written =
        atomicWriteUserData(pathOf(slot), std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
    return written ? SaveGameResult{.success = true} : SaveGameResult{.error = written.error};
}

SaveGameResult SaveGameStore::load(std::string_view slot, AssetManager& assets, const SceneDocumentOptions& options,
                                   Scene& destination, SaveGameInfo* info) const {
    if (!validSlotName(slot)) {
        return {.error = "Save slot '" + std::string(slot) + "' is not a valid name"};
    }
    const std::filesystem::path path = pathOf(slot);
    recoverAtomicUserData(path);
    const std::optional<Json> save = readSave(path);
    if (!save) {
        return {.error = "Save slot '" + std::string(slot) + "' is missing or unreadable"};
    }
    if (save->value("version", 0u) != saveVersion) {
        return {.error = "Save slot '" + std::string(slot) + "' has an unsupported version"};
    }
    const SaveGameInfo saved = infoOf(*save, std::string(slot));
    if (saved.kind != sceneKind) {
        return {.error = "Save slot '" + std::string(slot) + "' holds " + saved.kind + " data, not a scene"};
    }
    const std::filesystem::path baseline =
        saved.scene.is_absolute() || m_contentRoot.empty() ? saved.scene : m_contentRoot / saved.scene;

    try {
        SceneChanges changes;
        for (const Json& entity : save->at("entities")) {
            const Json& after = entity.at("after");
            changes.entities.push_back(
                {.id = SceneEntityId{entity.at("id").get<uint64_t>()},
                 .after = after.is_null() ? std::nullopt : std::optional<std::string>(after.dump())});
        }
        if (const auto collections = save->find("collections"); collections != save->end()) {
            changes.collections = SceneCollectionsChange{.after = collections->dump()};
        }
        Scene staged;
        if (SceneDocumentResult result = SceneDocument::load(baseline, assets, options, staged); !result) {
            return {.error = "Cannot load the save's scene " + baseline.string() + ": " + result.error};
        }
        if (SceneDocumentResult result =
                SceneDocument::applyChanges(changes, SceneChangeDirection::Forward, assets, options, staged);
            !result) {
            return {.error = "Cannot apply save slot '" + std::string(slot) + "': " + result.error};
        }
        destination.replaceWith(std::move(staged));
    } catch (const Json::exception& exception) {
        return {.error = "Save slot '" + std::string(slot) + "' is malformed: " + exception.what()};
    }
    if (info != nullptr) {
        *info = saved;
    }
    return {.success = true};
}

SaveGameResult SaveGameStore::saveFields(std::string_view slot, const void* data,
                                         std::span<const FieldDescriptor> fields, const SaveDataFormat& format,
                                         std::string title) const {
    if (!validSlotName(slot)) {
        return {.error = "Save slot '" + std::string(slot) + "' is not a valid name"};
    }
    if (std::string error = formatError(format); !error.empty()) {
        return {.error = std::move(error)};
    }
    Json written;
    writeReflectedFields(data, fields, written);
    const Json save = {{"format", saveFormat},           {"version", saveVersion},    {"kind", format.kind},
                       {"data_version", format.version}, {"title", std::move(title)}, {"saved_at", secondsSinceEpoch()},
                       {"data", std::move(written)}};
    const std::string text = save.dump(2);
    const PlatformResult result =
        atomicWriteUserData(pathOf(slot), std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
    return result ? SaveGameResult{.success = true} : SaveGameResult{.error = result.error};
}

SaveGameResult SaveGameStore::loadFields(std::string_view slot, void* data, std::span<const FieldDescriptor> fields,
                                         const SaveDataFormat& format, SaveGameInfo* info) const {
    if (!validSlotName(slot)) {
        return {.error = "Save slot '" + std::string(slot) + "' is not a valid name"};
    }
    if (std::string error = formatError(format); !error.empty()) {
        return {.error = std::move(error)};
    }
    const std::string owner = "Save slot '" + std::string(slot) + "'";
    const std::filesystem::path path = pathOf(slot);
    recoverAtomicUserData(path);
    const std::optional<Json> save = readSave(path);
    if (!save) {
        return {.error = owner + " is missing or unreadable"};
    }
    if (save->value("version", 0u) != saveVersion) {
        return {.error = owner + " has an unsupported version"};
    }
    const SaveGameInfo saved = infoOf(*save, std::string(slot));
    if (saved.kind != format.kind) {
        return {.error = owner + " holds " + saved.kind + " data, not " + format.kind};
    }
    const auto stored = save->find("data");
    if (stored == save->end() || !stored->is_object()) {
        return {.error = owner + " has no " + format.kind + " data"};
    }
    Json contents = *stored;
    if (std::string error = migrateData(contents, save->value("data_version", 0u), format, owner); !error.empty()) {
        return {.error = std::move(error)};
    }
    if (std::string error = readReflectedFields(contents, data, fields); !error.empty()) {
        return {.error = owner + ": " + error};
    }
    if (info != nullptr) {
        *info = saved;
    }
    return {.success = true};
}

std::filesystem::path SaveGameStore::profilePathOf(std::string_view name) const {
    return m_directory / (std::string(name) + std::string(profileExtension));
}

SaveGameResult SaveGameStore::saveProfileFields(std::string_view name, const void* data,
                                                std::span<const FieldDescriptor> fields,
                                                const SaveDataFormat& format) const {
    if (!validSlotName(name)) {
        return {.error = "Profile '" + std::string(name) + "' is not a valid name"};
    }
    if (std::string error = formatError(format); !error.empty()) {
        return {.error = std::move(error)};
    }
    Json written;
    writeReflectedFields(data, fields, written);
    const PlatformResult result = writeVersionedJson(profilePathOf(name), profileFileFormat(format),
                                                     Json{{"kind", format.kind}, {"data", std::move(written)}});
    return result ? SaveGameResult{.success = true} : SaveGameResult{.error = result.error};
}

SaveGameResult SaveGameStore::loadProfileFields(std::string_view name, void* data,
                                                std::span<const FieldDescriptor> fields, const SaveDataFormat& format,
                                                bool* found) const {
    if (found != nullptr) {
        *found = false;
    }
    if (!validSlotName(name)) {
        return {.error = "Profile '" + std::string(name) + "' is not a valid name"};
    }
    if (std::string error = formatError(format); !error.empty()) {
        return {.error = std::move(error)};
    }
    const std::filesystem::path path = profilePathOf(name);
    recoverAtomicUserData(path);
    VersionedJsonResult read = readVersionedJson(path, profileFileFormat(format));
    if (!read) {
        return {.error = read.error};
    }
    if (!read.found) {
        return {.success = true};
    }
    const std::string owner = "Profile '" + std::string(name) + "'";
    if (read.document.value("kind", std::string{}) != format.kind) {
        return {.error = owner + " does not hold " + format.kind + " data"};
    }
    const auto stored = read.document.find("data");
    if (stored == read.document.end() || !stored->is_object()) {
        return {.error = owner + " has no data"};
    }
    Json contents = *stored;
    if (std::string error = migrateData(contents, static_cast<uint32_t>(read.version), format, owner); !error.empty()) {
        return {.error = std::move(error)};
    }
    if (std::string error = readReflectedFields(contents, data, fields); !error.empty()) {
        return {.error = owner + ": " + error};
    }
    if (found != nullptr) {
        *found = true;
    }
    return {.success = true};
}

std::vector<SaveGameInfo> SaveGameStore::list() const {
    std::vector<SaveGameInfo> slots;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(m_directory, error)) {
        const std::filesystem::path& path = entry.path();
        const std::string slot = path.stem().string();
        if (path.extension() != saveExtension || !validSlotName(slot)) {
            continue;
        }
        if (const std::optional<Json> save = readSave(path)) {
            slots.push_back(infoOf(*save, slot));
        }
    }
    std::ranges::sort(slots, [](const SaveGameInfo& left, const SaveGameInfo& right) {
        return left.savedAt != right.savedAt ? left.savedAt > right.savedAt : left.slot < right.slot;
    });
    return slots;
}

SaveGameResult SaveGameStore::remove(std::string_view slot) const {
    if (!validSlotName(slot)) {
        return {.error = "Save slot '" + std::string(slot) + "' is not a valid name"};
    }
    const std::filesystem::path path = pathOf(slot);
    std::error_code error;
    bool removed = false;
    for (const std::filesystem::path& file :
         {path, std::filesystem::path(path.string() + ".bak"), std::filesystem::path(path.string() + ".tmp")}) {
        removed = std::filesystem::remove(file, error) || removed;
        if (error) {
            return {.error = "Cannot remove save slot '" + std::string(slot) + "': " + error.message()};
        }
    }
    return removed ? SaveGameResult{.success = true}
                   : SaveGameResult{.error = "Save slot '" + std::string(slot) + "' does not exist"};
}
