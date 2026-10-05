#include "AssetImport.hpp"

#include "editor/project/ProjectFileOperations.hpp"
#include "engine/assets/AssetMeta.hpp"

#include <system_error>

namespace {
// Files the asset database never tracks, so importing them would only add
// clutter (or, for sidecars, duplicate identities).
bool skippedName(const std::filesystem::path& name) {
    const std::string text = utf8PathText(name);
    return text.empty() || text.front() == '.' || name.extension() == ".tmp" ||
           name.extension() == std::filesystem::path(std::string(assetMetaExtension));
}

// True when path is base or lies inside it.
bool isWithin(const std::filesystem::path& path, const std::filesystem::path& base) {
    const std::filesystem::path relative = path.lexically_relative(base);
    return !relative.empty() && *relative.begin() != "..";
}

std::filesystem::path normalized(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(path, error);
    return (error ? path : absolute).lexically_normal();
}

// Copies a folder's contents, leaving skipped names behind.
void copyFolder(const std::filesystem::path& source, const std::filesystem::path& destination, std::error_code& error) {
    std::filesystem::create_directory(destination, error);
    if (error) {
        return;
    }
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(source, error)) {
        if (skippedName(entry.path().filename())) {
            continue;
        }
        const std::filesystem::path target = destination / entry.path().filename();
        if (entry.is_directory(error)) {
            copyFolder(entry.path(), target, error);
        } else if (entry.is_regular_file(error)) {
            std::filesystem::copy_file(entry.path(), target, std::filesystem::copy_options::none, error);
        }
        if (error) {
            return;
        }
    }
}
} // namespace

std::filesystem::path uniqueImportPath(const std::filesystem::path& folder, const std::filesystem::path& name) {
    std::filesystem::path candidate = folder / name;
    std::error_code error;
    if (!std::filesystem::exists(candidate, error)) {
        return candidate;
    }
    const bool isFolder = std::filesystem::is_directory(candidate, error);
    const std::filesystem::path stem = isFolder ? name : name.stem();
    const std::filesystem::path extension = isFolder ? std::filesystem::path{} : name.extension();
    for (int suffix = 1;; ++suffix) {
        std::filesystem::path numbered = stem;
        numbered += " (" + std::to_string(suffix) + ")";
        numbered += extension;
        candidate = folder / numbered;
        if (!std::filesystem::exists(candidate, error)) {
            return candidate;
        }
    }
}

AssetImportResult importIntoFolder(std::span<const std::filesystem::path> sources,
                                   const std::filesystem::path& folder) {
    AssetImportResult result;
    const std::filesystem::path destinationFolder = normalized(folder);
    for (const std::filesystem::path& rawSource : sources) {
        const std::filesystem::path source = normalized(rawSource);
        const std::string name = utf8PathText(source.filename());
        std::error_code error;
        if (skippedName(source.filename())) {
            continue;
        }
        const bool isFolder = std::filesystem::is_directory(source, error);
        if (!isFolder && !std::filesystem::is_regular_file(source, error)) {
            result.errors.push_back("Cannot import '" + utf8PathText(source) + "': it is not a file or folder.");
            continue;
        }
        if (isFolder && isWithin(destinationFolder, source)) {
            result.errors.push_back("Cannot import folder '" + name + "' into itself.");
            continue;
        }
        const std::filesystem::path destination = uniqueImportPath(destinationFolder, source.filename());
        if (isFolder) {
            copyFolder(source, destination, error);
        } else {
            std::filesystem::copy_file(source, destination, std::filesystem::copy_options::none, error);
        }
        if (error) {
            result.errors.push_back("Could not import '" + name + "': " + error.message());
            continue;
        }
        result.imported.push_back(destination);
    }
    return result;
}
