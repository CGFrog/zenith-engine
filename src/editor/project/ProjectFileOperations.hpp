#pragma once

#include <filesystem>
#include <string>
#include <string_view>

// Outcome of one project file operation. path names the file or folder the
// operation produced (the new folder, scene, renamed item, or copy).
struct ProjectFileResult {
    bool success{false};
    std::string error;
    std::filesystem::path path;

    explicit operator bool() const {
        return success;
    }
};

// File and folder operations behind the asset browser. None of them overwrite
// an existing item, and deletion only ever moves items to the OS trash. Names
// are UTF-8 file names without directory separators.

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

// Renames a file or folder in place. A file keeps its extension: when the new
// name does not already end with it (ignoring case), it is appended. Fails if
// another item already has the resulting name.
ProjectFileResult renameProjectItem(const std::filesystem::path& source, std::string_view utf8NewName);

// Copies a file or folder beside itself as "<name> copy", "<name> copy 2", ...
// keeping a file's extension.
ProjectFileResult duplicateProjectItem(const std::filesystem::path& source);

// Moves a file or folder to the OS trash: the Recycle Bin on Windows and the
// XDG home trash ($XDG_DATA_HOME/Trash) on Linux. It never deletes
// permanently on its own: on Linux an item the trash cannot take (another file
// system) fails; on Windows an item the Recycle Bin cannot take (a network
// share, a disabled bin) makes the shell ask the user before deleting it
// permanently, and declining fails.
ProjectFileResult moveProjectItemToTrash(const std::filesystem::path& target);

// Opens the OS file browser on the folder containing target, selecting target
// where the platform supports it (Explorer on Windows, xdg-open on Linux).
ProjectFileResult revealInFileBrowser(const std::filesystem::path& target);

// UTF-8 text for a path, for messages and settings files.
std::string utf8PathText(const std::filesystem::path& path);
std::filesystem::path pathFromUtf8Text(std::string_view text);
