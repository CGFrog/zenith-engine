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

AssetCatalogResult scanRoot(const std::filesystem::path& root, const std::filesystem::path& relativeBase,
                            std::map<std::string, EditorAssetRecord>& records) {
    std::error_code error;
    if (!std::filesystem::exists(root, error)) {
        return error ? AssetCatalogResult{.error = "Could not inspect asset root '" + root.string() +
                                                   "': " + error.message()}
                     : AssetCatalogResult{.success = true};
    }
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        const auto& entry = *iterator;
        if (entry.is_regular_file(error)) {
            EditorAssetKind kind;
            if (classify(entry.path(), kind)) {
                const auto relative = entry.path().lexically_relative(relativeBase);
                EditorAssetRecord record{.relativePath = relative,
                                         .absolutePath = entry.path().lexically_normal(),
                                         .kind = kind,
                                         .byteSize = entry.file_size(error),
                                         .lastWriteTime = entry.last_write_time(error)};
                if (!error) {
                    records[record.absolutePath.generic_string()] = std::move(record);
                }
            }
        }
        iterator.increment(error);
    }
    if (error) {
        return {.error = "Could not scan asset root '" + root.string() + "': " + error.message()};
    }
    return {.success = true};
}
} // namespace

AssetCatalog::AssetCatalog(std::filesystem::path assetRoot, std::filesystem::path sceneRoot)
    : m_assetRoot(std::move(assetRoot))
    , m_sceneRoot(std::move(sceneRoot)) {}

AssetCatalogResult AssetCatalog::refresh() {
    std::map<std::string, EditorAssetRecord> scanned;
    if (AssetCatalogResult result = scanRoot(m_assetRoot, m_assetRoot, scanned); !result) {
        return result;
    }
    if (m_sceneRoot != m_assetRoot) {
        if (AssetCatalogResult result = scanRoot(m_sceneRoot, m_sceneRoot, scanned); !result) {
            return result;
        }
    }

    std::map<std::string, EditorAssetRecord> previous;
    for (const EditorAssetRecord& asset : m_assets) {
        previous.emplace(asset.absolutePath.generic_string(), asset);
    }

    AssetCatalogResult result{.success = true};
    for (const auto& [path, asset] : scanned) {
        const auto old = previous.find(path);
        if (old == previous.end()) {
            result.changes.push_back({AssetChangeKind::Added, asset});
        } else if (old->second.byteSize != asset.byteSize || old->second.lastWriteTime != asset.lastWriteTime) {
            result.changes.push_back({AssetChangeKind::Modified, asset});
        }
    }
    for (const auto& [path, asset] : previous) {
        if (!scanned.contains(path)) {
            result.changes.push_back({AssetChangeKind::Removed, asset});
        }
    }

    m_assets.clear();
    m_assets.reserve(scanned.size());
    for (auto& [path, asset] : scanned) {
        m_assets.push_back(std::move(asset));
    }
    return result;
}

const std::vector<EditorAssetRecord>& AssetCatalog::assets() const {
    return m_assets;
}
