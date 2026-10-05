#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

// Importing files from outside the project (an Explorer drop, the Import
// dialog) into a project folder. Pure file operations: the caller rescans the
// asset database afterwards, which gives the copies their `.meta` sidecars
// and GUIDs.
//
// A file is copied; a folder is copied with everything in it. An imported
// name that is already taken in the destination gets " (1)", " (2)", ...
// before its extension ("stone (1).png"); nothing existing is ever replaced.
// `.meta` sidecars among the sources (or inside a source folder) are left
// behind, so an asset copied from another project gets an identity of its
// own instead of a duplicate GUID; hidden names and `*.tmp` files are skipped
// as the asset database skips them.
struct AssetImportResult {
    // The copies made, one per imported source, in source order.
    std::vector<std::filesystem::path> imported;
    // One message per source that could not be imported (missing, the
    // destination itself, a folder inside its own destination, a copy error).
    std::vector<std::string> errors;

    bool success() const {
        return errors.empty();
    }
};

// The first "<stem><extension>", "<stem> (1)<extension>", ... that does not
// exist in folder. A folder name has no extension.
std::filesystem::path uniqueImportPath(const std::filesystem::path& folder, const std::filesystem::path& name);

// Copies every source into folder (see above). folder must exist.
AssetImportResult importIntoFolder(std::span<const std::filesystem::path> sources, const std::filesystem::path& folder);
