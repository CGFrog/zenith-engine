#pragma once

#include "SceneDocument.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

class AssetManager;
class Scene;

// What a save slot holds besides the scene: the scene document it was saved
// from (its baseline), a title for menus, and when it was saved.
struct SaveGameInfo {
    std::string slot;
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
    // The slots in the directory, newest first; unreadable files are skipped.
    std::vector<SaveGameInfo> list() const;
    SaveGameResult remove(std::string_view slot) const;

  private:
    std::filesystem::path m_directory;
    std::filesystem::path m_contentRoot;
};
