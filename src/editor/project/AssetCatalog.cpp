#include "AssetCatalog.hpp"

#include <algorithm>
#include <cctype>
#include <map>

namespace {
std::string lowercaseExtension(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return extension;
}

bool classify(const std::filesystem::path& path, EditorAssetKind& kind) {
    const std::string extension = lowercaseExtension(path);
    if (extension == ".gltf" || extension == ".glb") {
        kind = EditorAssetKind::Model;
    } else if (extension == ".png" || extension == ".jpg" || extension == ".jpeg") {
        kind = EditorAssetKind::Texture;
    } else if (extension == ".wav" || extension == ".ogg") {
        kind = EditorAssetKind::Audio;
    } else if (extension == ".zenith-scene") {
        kind = EditorAssetKind::Scene;
    } else {
        return false;
    }
    return true;
}

struct ScanOutput {
    std::map<std::string, EditorAssetRecord> records;
    std::map<std::string, EditorAssetFolder> folders;
};

void addFolder(const std::filesystem::path& path, const std::filesystem::path& root, ScanOutput& output) {
    const std::filesystem::path absolute = normalizedCatalogPath(path);
    std::filesystem::path relative = absolute == root ? std::filesystem::path{} : absolute.lexically_relative(root);
    output.folders[absolute.generic_string()] = {.relativePath = std::move(relative), .absolutePath = absolute};
}

AssetCatalogResult scanRoot(const std::filesystem::path& root, ScanOutput& output) {
    std::error_code error;
    if (!std::filesystem::exists(root, error) || !std::filesystem::is_directory(root, error)) {
        return error ? AssetCatalogResult{.error = "Could not inspect asset root '" + root.string() +
                                                   "': " + error.message()}
                     : AssetCatalogResult{.success = true};
    }
    addFolder(root, root, output);
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        const auto& entry = *iterator;
        if (entry.is_directory(error) && !entry.is_symlink(error)) {
            addFolder(entry.path(), root, output);
        } else if (!error && entry.is_regular_file(error)) {
            EditorAssetKind kind;
            if (classify(entry.path(), kind)) {
                const std::filesystem::path absolute = normalizedCatalogPath(entry.path());
                EditorAssetRecord record{.relativePath = absolute.lexically_relative(root),
                                         .absolutePath = absolute,
                                         .kind = kind,
                                         .byteSize = entry.file_size(error),
                                         .lastWriteTime = entry.last_write_time(error)};
                if (!error) {
                    output.records[record.absolutePath.generic_string()] = std::move(record);
                }
            }
        }
        if (!error) {
            iterator.increment(error);
        }
    }
    if (error) {
        return {.error = "Could not scan asset root '" + root.string() + "': " + error.message()};
    }
    return {.success = true};
}
} // namespace

std::filesystem::path normalizedCatalogPath(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error) {
        absolute = path;
    }
    absolute = absolute.lexically_normal();
    if (!absolute.has_filename() && absolute.has_relative_path()) {
        absolute = absolute.parent_path();
    }
    return absolute;
}

AssetCatalog::AssetCatalog(std::filesystem::path assetRoot, std::filesystem::path sceneRoot) {
    m_roots.push_back(normalizedCatalogPath(assetRoot));
    if (std::filesystem::path scenes = normalizedCatalogPath(sceneRoot); scenes != m_roots.front()) {
        m_roots.push_back(std::move(scenes));
    }
}

AssetCatalogResult AssetCatalog::refresh() {
    ScanOutput scanned;
    for (const std::filesystem::path& root : m_roots) {
        if (AssetCatalogResult result = scanRoot(root, scanned); !result) {
            return result;
        }
    }

    std::map<std::string, EditorAssetRecord> previous;
    for (const EditorAssetRecord& asset : m_assets) {
        previous.emplace(asset.absolutePath.generic_string(), asset);
    }

    AssetCatalogResult result{.success = true};
    for (const auto& [path, asset] : scanned.records) {
        const auto old = previous.find(path);
        if (old == previous.end()) {
            result.changes.push_back({AssetChangeKind::Added, asset});
        } else if (old->second.byteSize != asset.byteSize || old->second.lastWriteTime != asset.lastWriteTime) {
            result.changes.push_back({AssetChangeKind::Modified, asset});
        }
    }
    for (const auto& [path, asset] : previous) {
        if (!scanned.records.contains(path)) {
            result.changes.push_back({AssetChangeKind::Removed, asset});
        }
    }

    m_assets.clear();
    m_assets.reserve(scanned.records.size());
    for (auto& [path, asset] : scanned.records) {
        m_assets.push_back(std::move(asset));
    }
    m_folders.clear();
    m_folders.reserve(scanned.folders.size());
    for (auto& [path, folder] : scanned.folders) {
        m_folders.push_back(std::move(folder));
    }
    return result;
}

const std::vector<EditorAssetRecord>& AssetCatalog::assets() const {
    return m_assets;
}

const std::vector<EditorAssetFolder>& AssetCatalog::folders() const {
    return m_folders;
}

const std::vector<std::filesystem::path>& AssetCatalog::roots() const {
    return m_roots;
}

const EditorAssetFolder* AssetCatalog::findFolder(const std::filesystem::path& absolutePath) const {
    const auto found = std::ranges::find(m_folders, absolutePath, &EditorAssetFolder::absolutePath);
    return found == m_folders.end() ? nullptr : &*found;
}

const EditorAssetRecord* AssetCatalog::findAsset(const std::filesystem::path& absolutePath) const {
    const auto found = std::ranges::find(m_assets, absolutePath, &EditorAssetRecord::absolutePath);
    return found == m_assets.end() ? nullptr : &*found;
}

std::vector<const EditorAssetFolder*> AssetCatalog::subfolders(const std::filesystem::path& folder) const {
    std::vector<const EditorAssetFolder*> children;
    for (const EditorAssetFolder& candidate : m_folders) {
        if (candidate.absolutePath != folder && candidate.absolutePath.parent_path() == folder) {
            children.push_back(&candidate);
        }
    }
    return children;
}

std::vector<const EditorAssetRecord*> AssetCatalog::assetsInFolder(const std::filesystem::path& folder) const {
    std::vector<const EditorAssetRecord*> children;
    for (const EditorAssetRecord& asset : m_assets) {
        if (asset.absolutePath.parent_path() == folder) {
            children.push_back(&asset);
        }
    }
    return children;
}
