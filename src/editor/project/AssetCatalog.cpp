#include "AssetCatalog.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <map>

struct AssetCatalog::Root {
    // Absolute and normalized.
    std::filesystem::path path;
    // Null for the source root, which is listed but not identified.
    std::shared_ptr<AssetDatabase> database;
    std::unique_ptr<FileWatcher> watcher;
    // The last watcher start failure, reported once until it changes.
    std::string watcherError;
};

namespace {
std::string lowercaseExtension(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return extension;
}

std::filesystem::path fromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

// True when path is base or lies inside it.
bool isWithin(const std::filesystem::path& path, const std::filesystem::path& base) {
    const std::filesystem::path relative = path.lexically_relative(base);
    return !relative.empty() && *relative.begin() != "..";
}

void addFolder(const std::filesystem::path& path, const std::filesystem::path& root,
               std::map<std::string, EditorAssetFolder>& folders) {
    const std::filesystem::path absolute = normalizedCatalogPath(path);
    std::filesystem::path relative = absolute == root ? std::filesystem::path{} : absolute.lexically_relative(root);
    folders[absolute.generic_string()] = {.relativePath = std::move(relative), .absolutePath = absolute};
}

AssetCatalogResult listFolders(const std::filesystem::path& root, std::map<std::string, EditorAssetFolder>& folders) {
    std::error_code error;
    if (!std::filesystem::exists(root, error) || !std::filesystem::is_directory(root, error)) {
        return error ? AssetCatalogResult{.error = "Could not inspect asset root '" + root.string() +
                                                   "': " + error.message()}
                     : AssetCatalogResult{.success = true};
    }
    addFolder(root, root, folders);
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        const auto& entry = *iterator;
        if (entry.is_directory(error) && !entry.is_symlink(error)) {
            addFolder(entry.path(), root, folders);
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

void appendResult(AssetCatalogResult&& source, AssetCatalogResult& destination) {
    std::ranges::move(source.changes, std::back_inserter(destination.changes));
    std::ranges::move(source.movedFiles, std::back_inserter(destination.movedFiles));
    std::ranges::move(source.removedFiles, std::back_inserter(destination.removedFiles));
    std::ranges::move(source.warnings, std::back_inserter(destination.warnings));
}

std::string rootDescription(const std::filesystem::path& root) {
    const std::u8string text = root.u8string();
    return "'" + std::string(text.begin(), text.end()) + "'";
}

// Names beginning with '.' (.git, .vs, .vscode) are skipped, as the asset
// databases skip them.
bool hiddenName(const std::filesystem::path& path) {
    const std::filesystem::path::string_type name = path.filename().native();
    return !name.empty() && name.front() == '.';
}

// Every script under a root without a database, keyed by absolute path.
AssetCatalogResult listScripts(const std::filesystem::path& root, std::map<std::string, EditorAssetRecord>& records) {
    std::error_code error;
    if (!std::filesystem::is_directory(root, error)) {
        return {.success = true};
    }
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        const std::filesystem::directory_entry& entry = *iterator;
        if (hiddenName(entry.path())) {
            if (entry.is_directory(error)) {
                iterator.disable_recursion_pending();
            }
        } else if (entry.is_regular_file(error) && editorAssetKind(entry.path()) == EditorAssetKind::Script) {
            const std::filesystem::path absolute = normalizedCatalogPath(entry.path());
            records[absolute.generic_string()] = {.relativePath = absolute.lexically_relative(root),
                                                  .absolutePath = absolute,
                                                  .kind = EditorAssetKind::Script,
                                                  .byteSize = entry.file_size(error),
                                                  .lastWriteTime = entry.last_write_time(error)};
        }
        // A file that vanished mid-scan is simply left out.
        error.clear();
        iterator.increment(error);
    }
    if (error) {
        return {.error = "Could not scan source root " + rootDescription(root) + ": " + error.message()};
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

std::optional<EditorAssetKind> editorAssetKind(const std::filesystem::path& path) {
    const std::string extension = lowercaseExtension(path);
    if (extension == ".gltf" || extension == ".glb") {
        return EditorAssetKind::Model;
    }
    if (extension == ".png" || extension == ".jpg" || extension == ".jpeg") {
        return EditorAssetKind::Texture;
    }
    if (extension == ".wav" || extension == ".ogg") {
        return EditorAssetKind::Audio;
    }
    if (extension == ".zenith-scene") {
        return EditorAssetKind::Scene;
    }
    if (extension == ".prefab") {
        return EditorAssetKind::Prefab;
    }
    if (extension == ".cpp" || extension == ".cc" || extension == ".cxx" || extension == ".c" || extension == ".hpp" ||
        extension == ".hh" || extension == ".hxx" || extension == ".h" || extension == ".inl") {
        return EditorAssetKind::Script;
    }
    return std::nullopt;
}

AssetCatalog::AssetCatalog(std::filesystem::path assetRoot, std::filesystem::path sceneRoot,
                           AssetCatalogOptions options)
    : m_options(std::move(options)) {
    m_roots.push_back(normalizedCatalogPath(assetRoot));
    if (std::filesystem::path scenes = normalizedCatalogPath(sceneRoot); scenes != m_roots.front()) {
        m_roots.push_back(std::move(scenes));
    }
    const AssetDatabaseOptions databaseOptions{.guidSeed = m_options.guidSeed};
    for (const std::filesystem::path& path : m_roots) {
        // A root inside another is tracked by the outer root's database, so a
        // file never has two identities.
        const bool nested = std::ranges::any_of(
            m_roots, [&path](const std::filesystem::path& other) { return other != path && isWithin(path, other); });
        if (nested) {
            continue;
        }
        auto root = std::make_unique<Root>();
        root->path = path;
        root->database = std::make_shared<AssetDatabase>(path, databaseOptions);
        m_trackedRoots.push_back(std::move(root));
    }
    if (m_options.sourceRoot.empty()) {
        return;
    }
    std::filesystem::path source = normalizedCatalogPath(m_options.sourceRoot);
    const bool overlaps = std::ranges::any_of(m_roots, [&source](const std::filesystem::path& other) {
        return isWithin(source, other) || isWithin(other, source);
    });
    if (!overlaps) {
        m_sourceRoot = source;
        m_roots.push_back(source);
        auto root = std::make_unique<Root>();
        root->path = std::move(source);
        m_trackedRoots.push_back(std::move(root));
    }
}

AssetCatalog::~AssetCatalog() = default;

AssetCatalogResult AssetCatalog::refresh() {
    AssetCatalogResult result{.success = true};
    for (const std::unique_ptr<Root>& root : m_trackedRoots) {
        AssetCatalogResult scanned = rescanRoot(*root);
        if (!scanned) {
            return scanned;
        }
        appendResult(std::move(scanned), result);
    }
    AssetCatalogResult listed = relist();
    if (!listed) {
        return listed;
    }
    return result;
}

AssetCatalogResult AssetCatalog::poll() {
    AssetCatalogResult result{.success = true};
    std::vector<Root*> changedRoots;
    for (const std::unique_ptr<Root>& root : m_trackedRoots) {
        if (!root->watcher) {
            startWatcher(*root, result.warnings);
            // Changes made before the watcher started would otherwise be
            // missed, so a newly watched root is reconciled once.
            if (root->watcher) {
                changedRoots.push_back(root.get());
            }
            continue;
        }
        const std::vector<FileWatchEvent> events = root->watcher->poll();
        if (events.empty()) {
            continue;
        }
        changedRoots.push_back(root.get());
        std::error_code error;
        if (!std::filesystem::is_directory(root->path, error)) {
            // The root itself went away; watch it again once it returns.
            root->watcher.reset();
        }
    }
    if (changedRoots.empty()) {
        return result;
    }
    for (Root* root : changedRoots) {
        AssetCatalogResult scanned = rescanRoot(*root);
        if (!scanned) {
            return scanned;
        }
        appendResult(std::move(scanned), result);
    }
    AssetCatalogResult listed = relist();
    if (!listed) {
        return listed;
    }
    return result;
}

bool AssetCatalog::watching() const {
    if (!m_options.watchFiles) {
        return false;
    }
    return std::ranges::all_of(m_trackedRoots, [](const std::unique_ptr<Root>& root) {
        std::error_code error;
        return root->watcher != nullptr || !std::filesystem::is_directory(root->path, error);
    });
}

void AssetCatalog::startWatcher(Root& root, std::vector<std::string>& warnings) {
    std::error_code error;
    if (!m_options.watchFiles || !std::filesystem::is_directory(root.path, error)) {
        return;
    }
    if (const FileWatcherResult created = FileWatcher::create(root.path, m_options.watcher, root.watcher); !created) {
        root.watcher.reset();
        if (created.error != root.watcherError) {
            root.watcherError = created.error;
            warnings.push_back("Could not watch " + rootDescription(root.path) +
                               " for changes; use Refresh to rescan. " + created.error);
        }
        return;
    }
    root.watcherError.clear();
}

AssetCatalogResult AssetCatalog::rescanRoot(Root& root) {
    std::error_code error;
    // A root without a database has no identities to reconcile; relist()
    // lists its scripts.
    if (!root.database || !std::filesystem::is_directory(root.path, error)) {
        // A missing root lists as empty; its database keeps the last known
        // identities in case the folder comes back.
        return {.success = true};
    }
    const std::vector<AssetRecord> previous = root.database->records();
    AssetDatabaseResult scanned = root.database->rescan();
    if (!scanned) {
        return {.error = "Could not scan " + rootDescription(root.path) + ": " + scanned.error};
    }
    AssetCatalogResult result{.success = true};
    appendChanges(root, scanned.changes, previous, result);
    for (std::string& warning : scanned.warnings) {
        result.warnings.push_back(rootDescription(root.path) + ": " + std::move(warning));
    }
    return result;
}

void AssetCatalog::appendChanges(const Root& root, const std::vector<AssetChange>& changes,
                                 const std::vector<AssetRecord>& previousRecords, AssetCatalogResult& result) const {
    const auto makeRecord = [this, &root, &previousRecords](const std::string& path,
                                                            AssetGuid guid) -> std::optional<EditorAssetRecord> {
        const std::filesystem::path absolute = normalizedCatalogPath(root.path / fromUtf8(path));
        const std::optional<EditorAssetKind> kind = editorAssetKind(absolute);
        if (!kind) {
            return std::nullopt;
        }
        EditorAssetRecord record{.absolutePath = absolute, .kind = *kind, .guid = guid};
        std::filesystem::path listingRoot = root.path;
        for (const std::filesystem::path& candidate : m_roots) {
            if (isWithin(absolute, candidate) && candidate.native().size() > listingRoot.native().size()) {
                listingRoot = candidate;
            }
        }
        record.relativePath = absolute.lexically_relative(listingRoot);
        if (const AssetRecord* tracked = root.database->find(guid)) {
            record.byteSize = tracked->sizeBytes;
            record.lastWriteTime = tracked->lastWriteTime;
        } else if (const auto old = std::ranges::find(previousRecords, guid, &AssetRecord::guid);
                   old != previousRecords.end()) {
            record.byteSize = old->sizeBytes;
            record.lastWriteTime = old->lastWriteTime;
        }
        return record;
    };

    for (const AssetChange& change : changes) {
        std::optional<EditorAssetRecord> record = makeRecord(change.path, change.guid);
        if (change.kind == AssetChangeKind::Removed) {
            result.removedFiles.push_back(normalizedCatalogPath(root.path / fromUtf8(change.path)));
        }
        if (change.kind == AssetChangeKind::Moved) {
            const std::filesystem::path previousPath = normalizedCatalogPath(root.path / fromUtf8(change.previousPath));
            result.movedFiles.push_back({previousPath, normalizedCatalogPath(root.path / fromUtf8(change.path))});
            const bool wasListed = editorAssetKind(previousPath).has_value();
            if (record && wasListed) {
                result.changes.push_back(
                    {.kind = AssetCatalogChangeKind::Moved, .asset = std::move(*record), .previousPath = previousPath});
            } else if (record) {
                result.changes.push_back({.kind = AssetCatalogChangeKind::Added, .asset = std::move(*record)});
            } else if (wasListed) {
                std::optional<EditorAssetRecord> removed = makeRecord(change.previousPath, change.guid);
                result.changes.push_back({.kind = AssetCatalogChangeKind::Removed, .asset = std::move(*removed)});
            }
            continue;
        }
        if (!record) {
            continue;
        }
        AssetCatalogChangeKind kind = AssetCatalogChangeKind::Added;
        if (change.kind == AssetChangeKind::Modified) {
            kind = AssetCatalogChangeKind::Modified;
        } else if (change.kind == AssetChangeKind::Removed) {
            kind = AssetCatalogChangeKind::Removed;
        }
        result.changes.push_back({.kind = kind, .asset = std::move(*record)});
    }
}

AssetCatalogResult AssetCatalog::relist() {
    std::map<std::string, EditorAssetFolder> folders;
    for (const std::filesystem::path& root : m_roots) {
        if (AssetCatalogResult result = listFolders(root, folders); !result) {
            return result;
        }
    }

    std::map<std::string, EditorAssetRecord> records;
    for (const std::unique_ptr<Root>& root : m_trackedRoots) {
        if (!root->database) {
            if (AssetCatalogResult listed = listScripts(root->path, records); !listed) {
                return listed;
            }
            continue;
        }
        std::error_code error;
        if (!std::filesystem::is_directory(root->path, error)) {
            continue;
        }
        AssetCatalogResult converted{.success = true};
        std::vector<AssetChange> current;
        for (const AssetRecord& record : root->database->records()) {
            current.push_back({.kind = AssetChangeKind::Added, .guid = record.guid, .path = record.path});
        }
        appendChanges(*root, current, {}, converted);
        for (AssetCatalogChange& change : converted.changes) {
            records[change.asset.absolutePath.generic_string()] = std::move(change.asset);
        }
    }

    m_assets.clear();
    m_assets.reserve(records.size());
    for (auto& [path, asset] : records) {
        m_assets.push_back(std::move(asset));
    }
    m_folders.clear();
    m_folders.reserve(folders.size());
    for (auto& [path, folder] : folders) {
        m_folders.push_back(std::move(folder));
    }
    return {.success = true};
}

AssetCatalog::Root* AssetCatalog::rootOwning(const AssetDatabase& database) {
    const auto found = std::ranges::find_if(
        m_trackedRoots, [&database](const std::unique_ptr<Root>& root) { return root->database.get() == &database; });
    return found == m_trackedRoots.end() ? nullptr : found->get();
}

AssetCatalogResult AssetCatalog::applyDatabaseChanges(const AssetDatabase& database,
                                                      const std::vector<AssetChange>& changes,
                                                      std::vector<std::string> warnings) {
    Root* root = rootOwning(database);
    if (root == nullptr) {
        return {.error = "The asset database does not belong to this project"};
    }
    AssetCatalogResult result{.success = true, .warnings = std::move(warnings)};
    // Removed assets are already gone from the database, so their records
    // come from the listing as it was before the operation.
    std::vector<AssetRecord> previous;
    for (const EditorAssetRecord& asset : m_assets) {
        previous.push_back({.guid = asset.guid, .sizeBytes = asset.byteSize, .lastWriteTime = asset.lastWriteTime});
    }
    appendChanges(*root, changes, previous, result);
    if (AssetCatalogResult listed = relist(); !listed) {
        return listed;
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

bool AssetCatalog::inSourceRoot(const std::filesystem::path& absolutePath) const {
    return !m_sourceRoot.empty() && isWithin(absolutePath, m_sourceRoot);
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

std::shared_ptr<const AssetDatabase> AssetCatalog::assetDatabase() const {
    // The asset root is first; the database whose root contains it tracks it.
    for (const std::unique_ptr<Root>& root : m_trackedRoots) {
        if (root->database && isWithin(m_roots.front(), root->path)) {
            return root->database;
        }
    }
    return m_trackedRoots.front()->database;
}

AssetDatabase* AssetCatalog::databaseFor(const std::filesystem::path& absolutePath) {
    const std::filesystem::path normalized = normalizedCatalogPath(absolutePath);
    Root* owner = nullptr;
    for (const std::unique_ptr<Root>& root : m_trackedRoots) {
        if (root->database && isWithin(normalized, root->path) &&
            (owner == nullptr || root->path.native().size() > owner->path.native().size())) {
            owner = root.get();
        }
    }
    return owner == nullptr ? nullptr : owner->database.get();
}
