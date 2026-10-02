#include "engine/assets/AssetDatabase.hpp"

#include <algorithm>
#include <cctype>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace {
namespace fs = std::filesystem;

constexpr std::string_view temporaryExtension = ".tmp";

std::string toUtf8(const fs::path& path) {
    const std::u8string text = path.generic_u8string();
    return {text.begin(), text.end()};
}

fs::path fromUtf8(std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

bool endsWithIgnoringCase(std::string_view text, std::string_view suffix) {
    if (suffix.size() > text.size()) {
        return false;
    }
    return std::equal(suffix.begin(), suffix.end(), text.end() - static_cast<std::ptrdiff_t>(suffix.size()),
                      [](char left, char right) {
                          return std::tolower(static_cast<unsigned char>(left)) ==
                                 std::tolower(static_cast<unsigned char>(right));
                      });
}

bool isIgnoredName(std::string_view name, const AssetDatabaseOptions& options) {
    if (name.empty() || name.front() == '.' || endsWithIgnoringCase(name, temporaryExtension)) {
        return true;
    }
    if (std::ranges::find(options.ignoredNames, name) != options.ignoredNames.end()) {
        return true;
    }
    return std::ranges::any_of(options.ignoredExtensions,
                               [name](const std::string& extension) { return endsWithIgnoringCase(name, extension); });
}

bool isMetaName(std::string_view name) {
    return name.size() > assetMetaExtension.size() && name.ends_with(assetMetaExtension);
}

std::string stripMetaExtension(std::string_view path) {
    return std::string(path.substr(0, path.size() - assetMetaExtension.size()));
}

// Returns the canonical `a/b/c` form, or nothing for an empty, absolute, or
// parent-escaping path.
std::optional<std::string> normalizeRelativePath(std::string_view text) {
    const fs::path path = fromUtf8(text);
    if (path.has_root_name() || path.has_root_directory()) {
        return std::nullopt;
    }
    std::string normalized;
    for (const fs::path& component : path) {
        const std::string part = toUtf8(component);
        if (part.empty() || part == ".") {
            continue;
        }
        if (part == "..") {
            return std::nullopt;
        }
        if (!normalized.empty()) {
            normalized += '/';
        }
        normalized += part;
    }
    if (normalized.empty()) {
        return std::nullopt;
    }
    return normalized;
}

bool isUnderDirectory(std::string_view path, std::string_view directory) {
    return path.size() > directory.size() && path.starts_with(directory) && path[directory.size()] == '/';
}

std::string parentOf(std::string_view path) {
    const size_t separator = path.rfind('/');
    return separator == std::string_view::npos ? std::string() : std::string(path.substr(0, separator));
}

// True when nothing is at `destination`, or it is the same file system entry as
// `source` (a case-only rename on a case-insensitive file system).
bool destinationIsFree(const fs::path& source, const fs::path& destination) {
    std::error_code error;
    const bool exists = fs::exists(destination, error);
    if (error) {
        return false;
    }
    if (!exists) {
        return true;
    }
    const bool same = fs::equivalent(source, destination, error);
    return !error && same;
}

struct ScannedFile {
    uint64_t sizeBytes{0};
    fs::file_time_type lastWriteTime{};
};

struct DirectorySnapshot {
    std::map<std::string, ScannedFile> files;
    // Each `<path>.meta` found, recorded as `<path>`.
    std::set<std::string> metaSubjects;
    // Files whose status could not be read this scan. They keep their previous
    // identity and their sidecars are not orphans.
    std::set<std::string> unreadableFiles;
};

AssetDatabaseResult snapshotDirectory(const fs::path& root, const AssetDatabaseOptions& options,
                                      DirectorySnapshot& snapshot) {
    std::error_code error;
    if (!fs::is_directory(root, error)) {
        return {.error = "asset root is not a directory: " + toUtf8(root)};
    }
    AssetDatabaseResult result{.success = true};
    fs::recursive_directory_iterator iterator(root, fs::directory_options::skip_permission_denied, error);
    for (; !error && iterator != fs::recursive_directory_iterator(); iterator.increment(error)) {
        const fs::directory_entry& entry = *iterator;
        const std::string name = toUtf8(entry.path().filename());
        std::error_code entryError;
        if (isIgnoredName(name, options)) {
            if (entry.is_directory(entryError)) {
                iterator.disable_recursion_pending();
            }
            continue;
        }
        if (!entry.is_regular_file(entryError)) {
            continue;
        }
        const std::string relative = toUtf8(entry.path().lexically_relative(root));
        if (isMetaName(name)) {
            if (!isIgnoredName(stripMetaExtension(name), options)) {
                snapshot.metaSubjects.insert(stripMetaExtension(relative));
            }
            continue;
        }
        const uintmax_t size = entry.file_size(entryError);
        const fs::file_time_type lastWriteTime = entryError ? fs::file_time_type{} : entry.last_write_time(entryError);
        if (entryError) {
            result.warnings.push_back(relative + ": could not read file status: " + entryError.message());
            snapshot.unreadableFiles.insert(relative);
            continue;
        }
        snapshot.files.emplace(relative, ScannedFile{.sizeBytes = size, .lastWriteTime = lastWriteTime});
    }
    if (error) {
        return {.error = "could not scan asset root: " + error.message()};
    }
    return result;
}

enum class SidecarState {
    Present,
    Missing,
    Unreadable,
};

// One file's claim on an identity during a rescan. A null `meta.guid` means
// the identity is not yet decided.
struct IdentityClaim {
    std::string path;
    ScannedFile file;
    SidecarState sidecar{SidecarState::Missing};
    AssetMeta meta;
    bool needsSidecarWrite{false};
};

struct PreviousState {
    const std::unordered_map<AssetGuid, AssetRecord>& records;
    const std::map<std::string, AssetGuid>& guidByPath;

    const AssetRecord* recordAt(const std::string& path) const {
        const auto found = guidByPath.find(path);
        return found == guidByPath.end() ? nullptr : &records.at(found->second);
    }

    const AssetRecord* recordFor(AssetGuid guid) const {
        const auto found = records.find(guid);
        return found == records.end() ? nullptr : &found->second;
    }
};

std::vector<IdentityClaim> readClaims(const fs::path& root, const DirectorySnapshot& snapshot,
                                      const PreviousState& previous, std::vector<std::string>& warnings) {
    std::vector<IdentityClaim> claims;
    claims.reserve(snapshot.files.size());
    for (const auto& [path, file] : snapshot.files) {
        IdentityClaim claim{.path = path, .file = file};
        const fs::path metaPath = assetMetaPath(root / fromUtf8(path));
        std::error_code error;
        // The second check finds a sidecar whose name differs only in case on
        // a case-insensitive file system.
        if (!snapshot.metaSubjects.contains(path) && !fs::exists(metaPath, error)) {
            claim.needsSidecarWrite = true;
            claims.push_back(std::move(claim));
            continue;
        }
        const AssetMetaResult read = readAssetMeta(metaPath, claim.meta);
        if (read) {
            claim.sidecar = SidecarState::Present;
            claim.needsSidecarWrite = claim.meta.version < AssetMeta::currentVersion;
            claims.push_back(std::move(claim));
            continue;
        }
        const AssetRecord* prior = previous.recordAt(path);
        warnings.push_back(path + ".meta: " + read.error +
                           (prior ? "; keeping the previous identity" : "; the asset stays untracked"));
        if (prior) {
            claim.sidecar = SidecarState::Unreadable;
            claim.meta = {.guid = prior->guid, .importSettings = prior->importSettings};
            claims.push_back(std::move(claim));
        }
    }
    return claims;
}

// Leaves each GUID with one claimant: the path that held it before, otherwise
// the lexicographically first. Other claimants are cleared for a fresh GUID.
void resolveDuplicateGuids(std::vector<IdentityClaim>& claims, const PreviousState& previous,
                           std::vector<std::string>& warnings) {
    std::map<AssetGuid, std::vector<size_t>> claimants;
    for (size_t index = 0; index < claims.size(); ++index) {
        if (claims[index].meta.guid) {
            claimants[claims[index].meta.guid].push_back(index);
        }
    }
    for (const auto& [guid, indices] : claimants) {
        if (indices.size() < 2) {
            continue;
        }
        size_t keeper = indices.front();
        if (const AssetRecord* prior = previous.recordFor(guid)) {
            for (const size_t index : indices) {
                if (claims[index].path == prior->path) {
                    keeper = index;
                }
            }
        }
        for (const size_t index : indices) {
            if (index == keeper) {
                continue;
            }
            IdentityClaim& claim = claims[index];
            warnings.push_back(claim.path + ": duplicate GUID " + guid.toString() + " (kept by " + claims[keeper].path +
                               "); assigning a new GUID");
            claim.meta.guid = {};
            claim.needsSidecarWrite = claim.sidecar != SidecarState::Unreadable;
        }
    }
}

// Gives files without a sidecar back an identity they already had: the GUID
// previously at the same path, or the GUID of a vanished asset with the same
// size and modification time when that match is unambiguous both ways.
void recoverMissingIdentities(std::vector<IdentityClaim>& claims, const DirectorySnapshot& snapshot,
                              const PreviousState& previous) {
    std::unordered_set<AssetGuid> claimed;
    for (const IdentityClaim& claim : claims) {
        if (claim.meta.guid) {
            claimed.insert(claim.meta.guid);
        }
    }
    auto isUnresolved = [](const IdentityClaim& claim) {
        return claim.sidecar == SidecarState::Missing && !claim.meta.guid;
    };
    for (IdentityClaim& claim : claims) {
        const AssetRecord* prior = isUnresolved(claim) ? previous.recordAt(claim.path) : nullptr;
        if (prior && !claimed.contains(prior->guid)) {
            claim.meta = {.guid = prior->guid, .importSettings = prior->importSettings};
            claimed.insert(prior->guid);
        }
    }

    std::vector<const AssetRecord*> vanished;
    for (const auto& [guid, record] : previous.records) {
        if (!claimed.contains(guid) && !snapshot.files.contains(record.path) &&
            !snapshot.unreadableFiles.contains(record.path)) {
            vanished.push_back(&record);
        }
    }
    auto sameFile = [](const IdentityClaim& claim, const AssetRecord& record) {
        return claim.file.sizeBytes == record.sizeBytes && claim.file.lastWriteTime == record.lastWriteTime;
    };
    std::unordered_map<AssetGuid, size_t> matchesPerRecord;
    for (const IdentityClaim& claim : claims) {
        if (!isUnresolved(claim)) {
            continue;
        }
        for (const AssetRecord* record : vanished) {
            if (sameFile(claim, *record)) {
                ++matchesPerRecord[record->guid];
            }
        }
    }
    for (IdentityClaim& claim : claims) {
        if (!isUnresolved(claim)) {
            continue;
        }
        const AssetRecord* match = nullptr;
        size_t matchCount = 0;
        for (const AssetRecord* record : vanished) {
            if (sameFile(claim, *record)) {
                match = record;
                ++matchCount;
            }
        }
        if (matchCount == 1 && matchesPerRecord[match->guid] == 1) {
            claim.meta = {.guid = match->guid, .importSettings = match->importSettings};
        }
    }
}

bool contentChanged(const AssetRecord& before, const AssetRecord& after) {
    return before.sizeBytes != after.sizeBytes || before.lastWriteTime != after.lastWriteTime ||
           before.importSettings != after.importSettings;
}

std::vector<AssetChange> diffRecords(const std::unordered_map<AssetGuid, AssetRecord>& before,
                                     const std::unordered_map<AssetGuid, AssetRecord>& after) {
    std::vector<AssetChange> changes;
    for (const auto& [guid, current] : after) {
        const auto prior = before.find(guid);
        if (prior == before.end()) {
            changes.push_back({.kind = AssetChangeKind::Added, .guid = guid, .path = current.path});
            continue;
        }
        if (prior->second.path != current.path) {
            changes.push_back({.kind = AssetChangeKind::Moved,
                               .guid = guid,
                               .path = current.path,
                               .previousPath = prior->second.path});
        }
        if (contentChanged(prior->second, current)) {
            changes.push_back({.kind = AssetChangeKind::Modified, .guid = guid, .path = current.path});
        }
    }
    for (const auto& [guid, prior] : before) {
        if (!after.contains(guid)) {
            changes.push_back({.kind = AssetChangeKind::Removed, .guid = guid, .path = prior.path});
        }
    }
    std::ranges::sort(changes, [](const AssetChange& left, const AssetChange& right) {
        return std::tie(left.path, left.kind) < std::tie(right.path, right.kind);
    });
    return changes;
}

// Reports sidecars whose asset is gone and applies the orphan policy. A
// sidecar named after a directory (written by another tool), after a file whose
// status could not be read, or after an existing file whose name differs only
// in case is not an orphan.
void handleOrphanSidecars(const fs::path& root, OrphanMetaPolicy policy, const DirectorySnapshot& snapshot,
                          AssetDatabaseResult& result) {
    for (const std::string& subject : snapshot.metaSubjects) {
        std::error_code error;
        if (snapshot.files.contains(subject) || snapshot.unreadableFiles.contains(subject) ||
            fs::exists(root / fromUtf8(subject), error)) {
            continue;
        }
        const std::string metaPath = subject + std::string(assetMetaExtension);
        result.orphanMetaFiles.push_back(metaPath);
        if (policy == OrphanMetaPolicy::Delete) {
            fs::remove(root / fromUtf8(metaPath), error);
            if (error) {
                result.warnings.push_back(metaPath + ": could not delete orphan sidecar: " + error.message());
            }
        }
    }
}
} // namespace

AssetDatabase::AssetDatabase(std::filesystem::path root, AssetDatabaseOptions options)
    : m_options(std::move(options))
    , m_generator(m_options.guidSeed ? AssetGuidGenerator(*m_options.guidSeed) : AssetGuidGenerator()) {
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(root, error);
    m_root = (error ? root : absolute).lexically_normal();
    if (!m_root.has_filename() && m_root.has_relative_path()) {
        m_root = m_root.parent_path();
    }
}

AssetDatabaseResult AssetDatabase::rescan() {
    DirectorySnapshot snapshot;
    AssetDatabaseResult result = snapshotDirectory(m_root, m_options, snapshot);
    if (!result) {
        return result;
    }
    const PreviousState previous{.records = m_records, .guidByPath = m_guidByPath};
    std::vector<IdentityClaim> claims = readClaims(m_root, snapshot, previous, result.warnings);
    resolveDuplicateGuids(claims, previous, result.warnings);
    recoverMissingIdentities(claims, snapshot, previous);

    std::unordered_map<AssetGuid, AssetRecord> records;
    std::map<std::string, AssetGuid> guidByPath;
    for (IdentityClaim& claim : claims) {
        if (!claim.meta.guid) {
            claim.meta.guid = m_generator.next();
            claim.needsSidecarWrite = claim.sidecar != SidecarState::Unreadable;
        }
        if (claim.needsSidecarWrite) {
            if (AssetMetaResult written = writeAssetMeta(assetMetaPath(absolutePath(claim.path)), claim.meta);
                !written) {
                result.warnings.push_back(claim.path + ".meta: " + written.error);
            }
        }
        guidByPath.emplace(claim.path, claim.meta.guid);
        records.emplace(claim.meta.guid, AssetRecord{.guid = claim.meta.guid,
                                                     .path = claim.path,
                                                     .importSettings = std::move(claim.meta.importSettings),
                                                     .sizeBytes = claim.file.sizeBytes,
                                                     .lastWriteTime = claim.file.lastWriteTime});
    }
    for (const std::string& path : snapshot.unreadableFiles) {
        const AssetRecord* prior = previous.recordAt(path);
        if (prior && !records.contains(prior->guid)) {
            guidByPath.emplace(path, prior->guid);
            records.emplace(prior->guid, *prior);
        }
    }
    handleOrphanSidecars(m_root, m_options.orphanMetaPolicy, snapshot, result);

    result.changes = diffRecords(m_records, records);
    for (const AssetChange& change : result.changes) {
        if (change.kind == AssetChangeKind::Removed) {
            clearDependencies(change.guid);
        }
    }
    m_records = std::move(records);
    m_guidByPath = std::move(guidByPath);
    return result;
}

std::optional<AssetGuid> AssetDatabase::guidForPath(std::string_view path) const {
    const std::optional<std::string> normalized = normalizeRelativePath(path);
    if (!normalized) {
        return std::nullopt;
    }
    const auto found = m_guidByPath.find(*normalized);
    if (found == m_guidByPath.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<std::string> AssetDatabase::pathForGuid(AssetGuid guid) const {
    const AssetRecord* record = find(guid);
    if (!record) {
        return std::nullopt;
    }
    return record->path;
}

const AssetRecord* AssetDatabase::find(AssetGuid guid) const {
    const auto found = m_records.find(guid);
    return found == m_records.end() ? nullptr : &found->second;
}

std::vector<AssetRecord> AssetDatabase::records() const {
    std::vector<AssetRecord> ordered;
    ordered.reserve(m_guidByPath.size());
    for (const auto& [path, guid] : m_guidByPath) {
        ordered.push_back(m_records.at(guid));
    }
    return ordered;
}

AssetDatabaseResult AssetDatabase::setImportSettings(AssetGuid guid, nlohmann::json settings) {
    const auto found = m_records.find(guid);
    if (found == m_records.end()) {
        return {.error = "unknown asset GUID " + guid.toString()};
    }
    if (!settings.is_object()) {
        return {.error = "import settings must be a JSON object"};
    }
    AssetRecord& record = found->second;
    const AssetMeta meta{.guid = guid, .importSettings = settings};
    if (AssetMetaResult written = writeAssetMeta(assetMetaPath(absolutePath(record.path)), meta); !written) {
        return {.error = record.path + ".meta: " + written.error};
    }
    record.importSettings = std::move(settings);
    return {.success = true, .changes = {{.kind = AssetChangeKind::Modified, .guid = guid, .path = record.path}}};
}

AssetDatabaseResult AssetDatabase::move(std::string_view fromPath, std::string_view toPath) {
    const std::optional<std::string> source = normalizeRelativePath(fromPath);
    const std::optional<std::string> destination = normalizeRelativePath(toPath);
    if (!source || !destination) {
        return {.error = "asset paths must be non-empty and project-relative"};
    }
    if (*source == *destination) {
        return {.success = true};
    }
    if (!isTrackablePath(*source) || !isTrackablePath(*destination)) {
        return {.error =
                    "path is ignored by the asset database: " + (isTrackablePath(*source) ? *destination : *source)};
    }
    std::error_code error;
    if (std::filesystem::is_directory(absolutePath(*source), error)) {
        return moveDirectory(*source, *destination);
    }
    return moveFile(*source, *destination);
}

AssetDatabaseResult AssetDatabase::rename(std::string_view path, std::string_view newName) {
    if (newName.empty() || newName == "." || newName == ".." || newName.find_first_of("/\\") != std::string::npos) {
        return {.error = "a new name must be a single path component"};
    }
    const std::optional<std::string> source = normalizeRelativePath(path);
    if (!source) {
        return {.error = "asset paths must be non-empty and project-relative"};
    }
    const std::string parent = parentOf(*source);
    return move(*source, parent.empty() ? std::string(newName) : parent + "/" + std::string(newName));
}

AssetDatabaseResult AssetDatabase::moveFile(const std::string& fromPath, const std::string& toPath) {
    const auto tracked = m_guidByPath.find(fromPath);
    if (tracked == m_guidByPath.end()) {
        return {.error = "not a tracked asset: " + fromPath};
    }
    const AssetGuid guid = tracked->second;
    const std::filesystem::path source = absolutePath(fromPath);
    const std::filesystem::path destination = absolutePath(toPath);
    const std::filesystem::path sourceMeta = assetMetaPath(source);
    const std::filesystem::path destinationMeta = assetMetaPath(destination);
    if (!destinationIsFree(source, destination) || !destinationIsFree(sourceMeta, destinationMeta)) {
        return {.error = "destination already exists: " + toPath};
    }
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) {
        return {.error = "could not create " + parentOf(toPath) + ": " + error.message()};
    }
    std::filesystem::rename(source, destination, error);
    if (error) {
        return {.error = "could not move " + fromPath + ": " + error.message()};
    }
    AssetDatabaseResult result{.success = true};
    AssetRecord& record = m_records.at(guid);
    if (std::filesystem::exists(sourceMeta, error)) {
        std::filesystem::rename(sourceMeta, destinationMeta, error);
        if (error) {
            std::error_code rollbackError;
            std::filesystem::rename(destination, source, rollbackError);
            return {.error = "could not move " + fromPath + ".meta: " + error.message()};
        }
    } else if (AssetMetaResult written =
                   writeAssetMeta(destinationMeta, {.guid = guid, .importSettings = record.importSettings});
               !written) {
        result.warnings.push_back(toPath + ".meta: " + written.error);
    }
    m_guidByPath.erase(tracked);
    forgetStalePath(toPath, result);
    m_guidByPath.emplace(toPath, guid);
    record.path = toPath;
    result.changes.push_back({.kind = AssetChangeKind::Moved, .guid = guid, .path = toPath, .previousPath = fromPath});
    return result;
}

AssetDatabaseResult AssetDatabase::moveDirectory(const std::string& fromPath, const std::string& toPath) {
    if (isUnderDirectory(toPath, fromPath)) {
        return {.error = "cannot move a directory into itself: " + fromPath};
    }
    const std::filesystem::path source = absolutePath(fromPath);
    const std::filesystem::path destination = absolutePath(toPath);
    if (!destinationIsFree(source, destination)) {
        return {.error = "destination already exists: " + toPath};
    }
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) {
        return {.error = "could not create " + parentOf(toPath) + ": " + error.message()};
    }
    std::filesystem::rename(source, destination, error);
    if (error) {
        return {.error = "could not move " + fromPath + ": " + error.message()};
    }
    std::vector<std::pair<std::string, AssetGuid>> moved;
    for (auto entry = m_guidByPath.lower_bound(fromPath + "/");
         entry != m_guidByPath.end() && isUnderDirectory(entry->first, fromPath); ++entry) {
        moved.emplace_back(entry->first, entry->second);
    }
    AssetDatabaseResult result{.success = true};
    for (const auto& [oldPath, guid] : moved) {
        std::string newPath = toPath + oldPath.substr(fromPath.size());
        m_guidByPath.erase(oldPath);
        forgetStalePath(newPath, result);
        m_guidByPath.emplace(newPath, guid);
        m_records.at(guid).path = newPath;
        result.changes.push_back(
            {.kind = AssetChangeKind::Moved, .guid = guid, .path = std::move(newPath), .previousPath = oldPath});
    }
    return result;
}

AssetDatabaseResult AssetDatabase::remove(std::string_view path) {
    const std::optional<std::string> normalized = normalizeRelativePath(path);
    if (!normalized) {
        return {.error = "asset paths must be non-empty and project-relative"};
    }
    if (!isTrackablePath(*normalized)) {
        return {.error = "path is ignored by the asset database: " + *normalized};
    }
    const std::filesystem::path target = absolutePath(*normalized);
    std::error_code error;
    std::vector<AssetGuid> removed;
    AssetDatabaseResult result{.success = true};
    if (std::filesystem::is_directory(target, error)) {
        std::filesystem::remove_all(target, error);
        if (error) {
            return {.error = "could not delete " + *normalized + ": " + error.message()};
        }
        for (auto entry = m_guidByPath.lower_bound(*normalized + "/");
             entry != m_guidByPath.end() && isUnderDirectory(entry->first, *normalized); ++entry) {
            removed.push_back(entry->second);
        }
    } else {
        const auto tracked = m_guidByPath.find(*normalized);
        if (tracked == m_guidByPath.end()) {
            return {.error = "not a tracked asset: " + *normalized};
        }
        std::filesystem::remove(target, error);
        if (error) {
            return {.error = "could not delete " + *normalized + ": " + error.message()};
        }
        std::filesystem::remove(assetMetaPath(target), error);
        if (error) {
            result.warnings.push_back(*normalized + ".meta: could not delete sidecar: " + error.message());
        }
        removed.push_back(tracked->second);
    }
    for (const AssetGuid guid : removed) {
        result.changes.push_back({.kind = AssetChangeKind::Removed, .guid = guid, .path = m_records.at(guid).path});
        forgetAsset(guid);
    }
    return result;
}

void AssetDatabase::setDependencies(AssetGuid asset, std::span<const AssetGuid> dependencies) {
    clearDependencies(asset);
    for (const AssetGuid dependency : dependencies) {
        if (!dependency || dependency == asset) {
            continue;
        }
        m_dependencies[asset].insert(dependency);
        m_dependents[dependency].insert(asset);
    }
}

std::vector<AssetGuid> AssetDatabase::dependencies(AssetGuid asset) const {
    const auto found = m_dependencies.find(asset);
    return found == m_dependencies.end() ? std::vector<AssetGuid>{}
                                         : std::vector<AssetGuid>(found->second.begin(), found->second.end());
}

std::vector<AssetGuid> AssetDatabase::dependents(AssetGuid asset) const {
    const auto found = m_dependents.find(asset);
    return found == m_dependents.end() ? std::vector<AssetGuid>{}
                                       : std::vector<AssetGuid>(found->second.begin(), found->second.end());
}

std::vector<AssetGuid> AssetDatabase::transitiveDependents(AssetGuid asset) const {
    std::set<AssetGuid> visited;
    std::vector<AssetGuid> pending{asset};
    while (!pending.empty()) {
        const AssetGuid current = pending.back();
        pending.pop_back();
        const auto found = m_dependents.find(current);
        if (found == m_dependents.end()) {
            continue;
        }
        for (const AssetGuid dependent : found->second) {
            if (dependent != asset && visited.insert(dependent).second) {
                pending.push_back(dependent);
            }
        }
    }
    return {visited.begin(), visited.end()};
}

std::filesystem::path AssetDatabase::absolutePath(std::string_view relativePath) const {
    return m_root / fromUtf8(relativePath);
}

bool AssetDatabase::isTrackablePath(std::string_view relativePath) const {
    size_t start = 0;
    while (start <= relativePath.size()) {
        const size_t end = std::min(relativePath.find('/', start), relativePath.size());
        if (isIgnoredName(relativePath.substr(start, end - start), m_options)) {
            return false;
        }
        start = end + 1;
    }
    return !isMetaName(relativePath.substr(relativePath.rfind('/') + 1));
}

void AssetDatabase::forgetStalePath(const std::string& path, AssetDatabaseResult& result) {
    // The destination was free on disk, so an asset still recorded there was
    // deleted externally and not yet rescanned.
    const auto stale = m_guidByPath.find(path);
    if (stale == m_guidByPath.end()) {
        return;
    }
    const AssetGuid guid = stale->second;
    result.changes.push_back({.kind = AssetChangeKind::Removed, .guid = guid, .path = path});
    forgetAsset(guid);
}

void AssetDatabase::forgetAsset(AssetGuid guid) {
    const auto found = m_records.find(guid);
    if (found == m_records.end()) {
        return;
    }
    m_guidByPath.erase(found->second.path);
    m_records.erase(found);
    clearDependencies(guid);
}

void AssetDatabase::clearDependencies(AssetGuid asset) {
    const auto found = m_dependencies.find(asset);
    if (found == m_dependencies.end()) {
        return;
    }
    for (const AssetGuid dependency : found->second) {
        const auto reverse = m_dependents.find(dependency);
        if (reverse == m_dependents.end()) {
            continue;
        }
        reverse->second.erase(asset);
        if (reverse->second.empty()) {
            m_dependents.erase(reverse);
        }
    }
    m_dependencies.erase(found);
}
