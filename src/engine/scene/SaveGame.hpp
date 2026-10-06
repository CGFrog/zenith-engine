#pragma once

// SaveGameStore: save slots holding a scene's changes against its baseline document.

#include "SceneDocument.hpp"
#include "engine/reflection/Reflection.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class AssetManager;
class Scene;

// What a save slot holds besides the scene: the scene document it was saved
// from (its baseline), a title for menus, and when it was saved.
struct SaveGameInfo {
    std::string slot;
    // "scene" for a scene save (save), otherwise the SaveDataFormat::kind of
    // the game data it holds (saveData).
    std::string kind;
    // As save recorded it: relative to the store's content root when the
    // baseline was inside it, otherwise absolute.
    std::filesystem::path scene;
    std::string title;
    // Seconds since the Unix epoch.
    int64_t savedAt{0};
};

struct SaveGameResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// How a game's own data is saved (SaveGameStore::saveData, saveProfile).
struct SaveDataFormat {
    // What the data is, for example "run" or "profile"; data of another kind
    // does not load as it. "scene" is reserved for scene saves.
    std::string kind;
    // The data's own version, written with it.
    uint32_t version{1};
    // Upgrades data saved at an older version, in place, before it is read;
    // returns an error, or an empty string. Needed once version is above 1.
    std::function<std::string(nlohmann::json& data, uint32_t savedVersion)> migrate;
};

// Save slots in a directory (for a game, under userDataDirectory). A save
// records how a running scene differs from the scene document it was loaded
// from: SceneDocument::diff between the baseline and the scene serialized for
// a save (SceneDocumentPurpose::SaveGame, which keeps .runtimeState() fields),
// keeping only each changed entity's new state. Loading reads the baseline
// again and applies the changes, so entities the game never touched follow
// later edits of the level. Files are written atomically
// (atomicWriteUserData); a save interrupted mid-write is recovered from its
// backup on the next load.
class SaveGameStore {
  public:
    // Saves live in directory, one "<slot>.zsave" file each. Baselines inside
    // contentRoot (for example the project directory) are recorded relative
    // to it, so a save survives the project moving.
    explicit SaveGameStore(std::filesystem::path directory, std::filesystem::path contentRoot = {});

    // 1 to 64 letters, digits, '-' or '_'.
    static bool validSlotName(std::string_view slot);

    const std::filesystem::path& directory() const;
    std::filesystem::path pathOf(std::string_view slot) const;

    // Saves scene, which was loaded from the scene document at baseline, into
    // slot, replacing what the slot held. options are the scene's document
    // options; their purpose is ignored.
    SaveGameResult save(std::string_view slot, const Scene& scene, const AssetManager& assets,
                        const SceneDocumentOptions& options, const std::filesystem::path& baseline,
                        std::string title = {}) const;
    // Loads slot into destination, replacing its contents: the baseline scene
    // with the saved changes applied. info, when given, receives the slot's
    // metadata. destination is unchanged on failure.
    SaveGameResult load(std::string_view slot, AssetManager& assets, const SceneDocumentOptions& options,
                        Scene& destination, SaveGameInfo* info = nullptr) const;

    // Saves a game's own data into slot instead of a scene: a reflected
    // struct, for example a roguelike run (its seed and random streams, the
    // floor, the player's stats and inventory) that the game rebuilds its
    // scene from. Written atomically, like scene saves, and listed with them.
    template <typename Data>
    SaveGameResult saveData(std::string_view slot, const Data& data, Fields<Data> fields, const SaveDataFormat& format,
                            std::string title = {}) const {
        const std::vector<FieldDescriptor> descriptors = std::move(fields).build();
        return saveFields(slot, &data, descriptors, format, std::move(title));
    }

    // Loads slot's data of format.kind into data, starting from a
    // default-constructed Data and migrating older versions. data is
    // unchanged on failure; info, when given, receives the slot's metadata.
    template <typename Data>
    SaveGameResult loadData(std::string_view slot, Data& data, Fields<Data> fields, const SaveDataFormat& format,
                            SaveGameInfo* info = nullptr) const {
        const std::vector<FieldDescriptor> descriptors = std::move(fields).build();
        Data staged{};
        SaveGameResult result = loadFields(slot, &staged, descriptors, format, info);
        if (result) {
            data = std::move(staged);
        }
        return result;
    }

    // Game data kept across runs, such as unlocks and statistics:
    // "<name>.zprofile" beside the slots, a versioned JSON file written
    // atomically with a backup. list() does not show it and remove() never
    // touches it, so a game deleting a run slot on death keeps the profile.
    template <typename Data>
    SaveGameResult saveProfile(std::string_view name, const Data& data, Fields<Data> fields,
                               const SaveDataFormat& format) const {
        const std::vector<FieldDescriptor> descriptors = std::move(fields).build();
        return saveProfileFields(name, &data, descriptors, format);
    }

    // Loads profile name into data. A missing profile succeeds and leaves
    // data as it is (a new player's defaults); found, when given, says which.
    template <typename Data>
    SaveGameResult loadProfile(std::string_view name, Data& data, Fields<Data> fields, const SaveDataFormat& format,
                               bool* found = nullptr) const {
        const std::vector<FieldDescriptor> descriptors = std::move(fields).build();
        Data staged = data;
        SaveGameResult result = loadProfileFields(name, &staged, descriptors, format, found);
        if (result) {
            data = std::move(staged);
        }
        return result;
    }

    std::filesystem::path profilePathOf(std::string_view name) const;

    // The slots in the directory, newest first; unreadable files are skipped.
    std::vector<SaveGameInfo> list() const;
    SaveGameResult remove(std::string_view slot) const;

  private:
    SaveGameResult saveFields(std::string_view slot, const void* data, std::span<const FieldDescriptor> fields,
                              const SaveDataFormat& format, std::string title) const;
    SaveGameResult loadFields(std::string_view slot, void* data, std::span<const FieldDescriptor> fields,
                              const SaveDataFormat& format, SaveGameInfo* info) const;
    SaveGameResult saveProfileFields(std::string_view name, const void* data, std::span<const FieldDescriptor> fields,
                                     const SaveDataFormat& format) const;
    SaveGameResult loadProfileFields(std::string_view name, void* data, std::span<const FieldDescriptor> fields,
                                     const SaveDataFormat& format, bool* found) const;

    std::filesystem::path m_directory;
    std::filesystem::path m_contentRoot;
};
