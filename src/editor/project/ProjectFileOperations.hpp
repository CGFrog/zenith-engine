#pragma once

#include "engine/assets/AssetDatabase.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Outcome of one project file operation. path names the file or folder the
// operation produced (the new folder, scene, renamed item, moved item, or
// copy).
struct ProjectFileResult {
    bool success{false};
    std::string error;
    std::filesystem::path path;
    // When the operation went through an asset database: the changes it made
    // there, with database-relative paths, and its recoverable problems.
    std::vector<AssetChange> assetChanges;
    std::vector<std::string> warnings;

    explicit operator bool() const {
        return success;
    }
};

// File and folder operations behind the asset browser. None of them overwrite
// an existing item, and deletion only ever moves items to the OS trash. Names
// are UTF-8 file names without directory separators.
//
// Asset files carry a `<file>.meta` sidecar holding their GUID (see
// AssetDatabase). Renames, moves, and trashing take the sidecar along, and a
// copy gets a sidecar with a fresh GUID and the same import settings. When a
// database is passed it must be the one tracking the item; the operation goes
// through it so its identities stay current, and the result lists its changes.

// Rejects names that cannot be a single portable file name. Returns an empty
// string for a valid name, otherwise the reason it is invalid.
std::string invalidFileNameReason(std::string_view utf8Name);

// The first "<base><extension>", "<base> 2<extension>", ... that does not exist
// in folder. Fails if the folder cannot be inspected.
ProjectFileResult uniqueChildPath(const std::filesystem::path& folder, std::string_view utf8Base,
                                  const std::filesystem::path& extension);

// Creates "New Folder" (or "New Folder 2", ...) in parent.
ProjectFileResult createProjectFolder(const std::filesystem::path& parent);

// Writes an empty scene document named "New Scene.zenith-scene" (or a unique
// variant) into folder. assetRoot is the scene document's asset root.
ProjectFileResult createProjectScene(const std::filesystem::path& folder, const std::filesystem::path& assetRoot);

// Writes a default material file named "New Material.zmat" (or a unique
// variant) into folder.
ProjectFileResult createProjectMaterial(const std::filesystem::path& folder);

// Writes a prefab document with one empty root entity named "New Prefab.prefab"
// (or a unique variant) into folder.
ProjectFileResult createProjectPrefab(const std::filesystem::path& folder);

// Renames a file or folder in place. A file keeps its extension: when the new
// name does not already end with it (ignoring case), it is appended. Fails if
// another item already has the resulting name.
ProjectFileResult renameProjectItem(const std::filesystem::path& source, std::string_view utf8NewName,
                                    AssetDatabase* database = nullptr);

// Moves a file or folder into destinationFolder, keeping its name. Fails if the
// folder already holds an item with that name or lies inside source. Moving an
// item to the folder it is already in succeeds without changes.
ProjectFileResult moveProjectItem(const std::filesystem::path& source, const std::filesystem::path& destinationFolder,
                                  AssetDatabase* database = nullptr);

// Copies a file or folder beside itself as "<name> copy", "<name> copy 2", ...
// keeping a file's extension. Copied sidecars get fresh GUIDs.
ProjectFileResult duplicateProjectItem(const std::filesystem::path& source);

// Moves a file or folder to the OS trash: the Recycle Bin on Windows and the
// XDG home trash ($XDG_DATA_HOME/Trash) on Linux. It never deletes
// permanently on its own: on Linux an item the trash cannot take (another file
// system) fails; on Windows an item the Recycle Bin cannot take (a network
// share, a disabled bin) makes the shell ask the user before deleting it
// permanently, and declining fails. A file's sidecar follows it to the trash,
// so restoring both restores the asset's identity.
ProjectFileResult moveProjectItemToTrash(const std::filesystem::path& target, AssetDatabase* database = nullptr);

// Opens the OS file browser on the folder containing target, selecting target
// where the platform supports it (Explorer on Windows, xdg-open on Linux).
ProjectFileResult revealInFileBrowser(const std::filesystem::path& target);

// UTF-8 text for a path, for messages and settings files.
std::string utf8PathText(const std::filesystem::path& path);
std::filesystem::path pathFromUtf8Text(std::string_view text);
