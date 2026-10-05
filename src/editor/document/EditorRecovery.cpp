#include "EditorRecovery.hpp"

#include "engine/assets/Assets.hpp"
#include "engine/platform/FileContents.hpp"
#include "engine/platform/PlatformServices.hpp"

#include <nlohmann/json.hpp>

#include <span>
#include <utility>

EditorRecovery::EditorRecovery(std::filesystem::path path)
    : m_path(std::move(path)) {}

SceneDocumentResult EditorRecovery::autosave(const Scene& scene, const AssetManager& assets,
                                             const SceneDocumentOptions& options) const {
    return SceneDocument::save(m_path, scene, assets, options);
}

SceneDocumentResult EditorRecovery::recover(Scene& scene, AssetManager& assets,
                                            const SceneDocumentOptions& options) const {
    return SceneDocument::load(m_path, assets, options, scene);
}

bool EditorRecovery::available() const {
    std::error_code error;
    return std::filesystem::is_regular_file(m_path, error);
}

bool EditorRecovery::discard(std::string& errorMessage) const {
    std::error_code error;
    std::filesystem::remove(m_path, error);
    if (error) {
        errorMessage = "Could not remove recovery file '" + m_path.string() + "': " + error.message();
        return false;
    }
    return true;
}

const std::filesystem::path& EditorRecovery::path() const {
    return m_path;
}

PrefabEditRecovery::PrefabEditRecovery(std::filesystem::path path)
    : m_scene(std::move(path)) {}

std::filesystem::path PrefabEditRecovery::notePath() const {
    std::filesystem::path note = m_scene.path();
    note += ".json";
    return note;
}

SceneDocumentResult PrefabEditRecovery::autosave(const Scene& scene, const AssetManager& assets,
                                                 const SceneDocumentOptions& options,
                                                 const PrefabEditRecoveryNote& note) const {
    if (SceneDocumentResult saved = m_scene.autosave(scene, assets, options); !saved) {
        return saved;
    }
    const std::string text = nlohmann::json{
        {"version", 1},
        {"prefab", note.reference.generic_string()},
        {"root", note.root.value},
        {"display_name",
         note.displayName}}.dump(2);
    const auto* bytes = reinterpret_cast<const uint8_t*>(text.data());
    if (const PlatformResult written =
            atomicWriteUserData(notePath(), std::span<const uint8_t>(bytes, text.size()), UserDataBackup::None);
        !written) {
        return {.error = "Could not write the prefab recovery note: " + written.error};
    }
    return {.success = true};
}

SceneDocumentResult PrefabEditRecovery::recover(Scene& scene, AssetManager& assets, const SceneDocumentOptions& options,
                                                PrefabEditRecoveryNote& note) const {
    std::string text;
    if (readFileText(notePath(), text) != FileReadStatus::Success) {
        return {.error = "Could not read the prefab recovery note '" + notePath().string() + "'"};
    }
    const nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
    if (!json.is_object() || !json.contains("prefab") || !json["prefab"].is_string() || !json.contains("root") ||
        !json["root"].is_number_unsigned()) {
        return {.error = "The prefab recovery note is not valid"};
    }
    PrefabEditRecoveryNote read{.reference = json["prefab"].get<std::string>(),
                                .root = SceneEntityId{json["root"].get<uint64_t>()},
                                .displayName = json.value("display_name", std::string{})};
    if (SceneDocumentResult loaded = m_scene.recover(scene, assets, options); !loaded) {
        return loaded;
    }
    if (read.displayName.empty()) {
        read.displayName = read.reference.stem().generic_string();
    }
    note = std::move(read);
    return {.success = true};
}

bool PrefabEditRecovery::available() const {
    std::error_code error;
    return m_scene.available() && std::filesystem::is_regular_file(notePath(), error);
}

bool PrefabEditRecovery::discard(std::string& errorMessage) const {
    std::error_code error;
    std::filesystem::remove(notePath(), error);
    if (error) {
        errorMessage = "Could not remove recovery file '" + notePath().string() + "': " + error.message();
        return false;
    }
    return m_scene.discard(errorMessage);
}
