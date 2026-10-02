#include "engine/assets/AssetDatabase.hpp"
#include "engine/assets/AssetGuid.hpp"
#include "engine/assets/AssetMeta.hpp"
#include "engine/assets/ContentHash.hpp"
#include "engine/assets/ProductionAssetPipeline.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <unordered_set>
#include <vector>

namespace {
namespace fs = std::filesystem;

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

fs::path freshDirectory(const std::string& name) {
    const fs::path directory = fs::temp_directory_path() / "zenith_asset_database_tests" / name;
    fs::remove_all(directory);
    fs::create_directories(directory);
    return directory;
}

void writeText(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

std::string readText(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

AssetGuid metaGuid(const fs::path& assetPath) {
    AssetMeta meta;
    if (!readAssetMeta(assetMetaPath(assetPath), meta)) {
        return {};
    }
    return meta.guid;
}

bool hasChange(const AssetDatabaseResult& result, AssetChangeKind kind, const std::string& path) {
    return std::ranges::any_of(result.changes,
                               [&](const AssetChange& change) { return change.kind == kind && change.path == path; });
}

AssetDatabaseOptions seededOptions(uint64_t seed) {
    AssetDatabaseOptions options;
    options.guidSeed = seed;
    return options;
}

void testContentHash() {
    expect(contentHash(std::string_view{}) == ContentHasher::offsetBasis, "empty input hashes to the offset basis");
    expect(formatContentHash(contentHash("a")) == "af63dc4c8601ec8c", "FNV-1a 64 of 'a' matches the reference");
    expect(formatContentHash(contentHash("foobar")) == "85944171f73967e8",
           "FNV-1a 64 of 'foobar' matches the reference");
    expect(formatContentHash(1) == "0000000000000001", "hash text is zero padded");
    ContentHasher incremental;
    incremental.append("foo").append("bar");
    expect(incremental.value() == contentHash("foobar"), "incremental hashing matches one-shot hashing");
    expect(incremental.hex() == "85944171f73967e8", "hasher formats its own value");

    const fs::path directory = freshDirectory("content_hash");
    writeText(directory / "source.bin", "foobar");
    ContentHasher fileHasher;
    expect(fileHasher.appendFile(directory / "source.bin"), "existing file hashes");
    expect(fileHasher.value() == contentHash("foobar"), "file hashing matches its bytes");
    ContentHasher missing;
    expect(!missing.appendFile(directory / "missing.bin"), "missing file reports failure");

    // Derived cache names must stay byte-identical across the extraction:
    // source bytes then settings, tool version, and target, no separators.
    DerivedAssetCache cache(directory / "derived");
    std::string key;
    expect(
        static_cast<bool>(cache.key(
            {.source = directory / "source.bin", .importSettings = "srgb", .toolVersion = "v1", .targetPlatform = "pc"},
            key)),
        "derived cache key succeeds");
    expect(key == formatContentHash(contentHash("foobarsrgbv1pc")), "derived cache key is unchanged");
    fs::remove_all(directory);
}

void testAssetGuid() {
    const AssetGuid guid{0x0123456789abcdefull, 0xfedcba9876543210ull};
    expect(guid.toString() == "0123456789abcdeffedcba9876543210", "GUID text is lowercase high word first");
    expect(AssetGuid::parse(guid.toString()) == guid, "GUID text round-trips");
    expect(AssetGuid::parse("0123456789ABCDEFFEDCBA9876543210") == guid, "uppercase GUID text parses");
    expect(!AssetGuid::parse("0123"), "short GUID text is rejected");
    expect(!AssetGuid::parse("0123456789abcdeffedcba987654321g"), "non-hex GUID text is rejected");
    expect(AssetGuid{}.isNull() && !AssetGuid{}, "default GUID is null");

    AssetGuidGenerator first(42);
    AssetGuidGenerator second(42);
    expect(first.next() == second.next(), "seeded generators are deterministic");
    AssetGuidGenerator random;
    std::unordered_set<AssetGuid> seen;
    for (int index = 0; index < 1000; ++index) {
        const AssetGuid next = random.next();
        expect(!next.isNull(), "generated GUIDs are never null");
        seen.insert(next);
    }
    expect(seen.size() == 1000, "generated GUIDs are distinct and hashable");
}

void testAssetMeta() {
    AssetMeta meta{.guid = AssetGuid{1, 2}, .importSettings = {{"srgb", true}}};
    AssetMeta parsed;
    expect(static_cast<bool>(parseAssetMeta(serializeAssetMeta(meta), parsed)), "meta round-trips");
    expect(parsed.guid == meta.guid && parsed.importSettings == meta.importSettings, "meta fields round-trip");
    expect(static_cast<bool>(parseAssetMeta(R"({"version":1,"guid":"0000000000000000000000000000000a"})", parsed)) &&
               parsed.importSettings == nlohmann::json::object(),
           "missing import settings read as an empty object");
    expect(!parseAssetMeta(R"({"version":2,"guid":"0000000000000000000000000000000a"})", parsed),
           "future meta versions are rejected");
    expect(!parseAssetMeta(R"({"version":1,"guid":"00000000000000000000000000000000"})", parsed),
           "null GUIDs are rejected");
    expect(!parseAssetMeta(R"({"version":1,"guid":"0000000000000000000000000000000a","importSettings":3})", parsed),
           "non-object import settings are rejected");
    expect(!parseAssetMeta("{not json", parsed), "malformed JSON is rejected");

    const fs::path directory = freshDirectory("meta");
    const fs::path metaPath = directory / "stone.png.meta";
    expect(static_cast<bool>(writeAssetMeta(metaPath, meta)), "meta writes");
    expect(static_cast<bool>(writeAssetMeta(metaPath, meta)), "meta overwrites in place");
    expect(!fs::exists(directory / "stone.png.meta.tmp"), "atomic write leaves no temporary file");
    expect(static_cast<bool>(readAssetMeta(metaPath, parsed)) && parsed.guid == meta.guid, "meta reads back");
    expect(!writeAssetMeta(directory / "null.meta", AssetMeta{}), "null GUID meta is not written");
    fs::remove_all(directory);
}

void testInitialScanAndIgnoreRules() {
    const fs::path root = freshDirectory("initial_scan");
    writeText(root / "readme.txt", "r");
    writeText(root / "textures/stone.png", "png");
    writeText(root / ".git/config", "hidden");
    writeText(root / "build/output.bin", "ignored directory");
    writeText(root / "scratch.tmp", "temporary");
    writeText(root / "notes.LOG", "ignored extension");

    AssetDatabaseOptions options = seededOptions(1);
    options.ignoredNames = {"build"};
    options.ignoredExtensions = {".log"};
    AssetDatabase database(root, options);
    const AssetDatabaseResult scan = database.rescan();
    expect(static_cast<bool>(scan), "initial scan succeeds");
    expect(database.assetCount() == 2, "only trackable files are assets");
    expect(scan.changes.size() == 2 && scan.changes[0].path == "readme.txt" &&
               scan.changes[1].path == "textures/stone.png" && scan.changes[0].kind == AssetChangeKind::Added,
           "first scan reports every asset as added, sorted by path");
    expect(fs::exists(root / "readme.txt.meta") && fs::exists(root / "textures/stone.png.meta"),
           "missing sidecars are created");
    expect(!fs::exists(root / "scratch.tmp.meta") && !fs::exists(root / "build/output.bin.meta") &&
               !fs::exists(root / "notes.LOG.meta") && !fs::exists(root / ".git/config.meta"),
           "ignored files get no sidecar");
    expect(!fs::exists(root / "textures.meta"), "directories get no sidecar");

    const std::optional<AssetGuid> stone = database.guidForPath("textures/stone.png");
    expect(stone.has_value() && metaGuid(root / "textures/stone.png") == *stone, "sidecar holds the tracked GUID");
    expect(database.guidForPath("textures\\..\\textures/./stone.png") == std::nullopt,
           "parent-escaping lookups are rejected");
    expect(database.guidForPath("textures//stone.png") == stone, "lookups normalize separators");
    expect(stone && database.pathForGuid(*stone) == "textures/stone.png", "GUID resolves to its path");
    const std::vector<AssetRecord> records = database.records();
    expect(records.size() == 2 && records[0].path == "readme.txt" && records[1].path == "textures/stone.png",
           "records iterate by path");

    const AssetDatabaseResult unchanged = database.rescan();
    expect(unchanged && unchanged.changes.empty() && unchanged.warnings.empty(), "a quiet rescan reports nothing");

    AssetDatabase reopened(root, seededOptions(99));
    const AssetDatabaseResult reopenedScan = reopened.rescan();
    expect(reopenedScan && reopened.guidForPath("textures/stone.png") == stone,
           "GUIDs persist across database instances");

    AssetDatabase missingRoot(root / "does-not-exist");
    expect(!missingRoot.rescan(), "scanning a missing root fails");
    fs::remove_all(root);
}

void testApiMoveRenameRemove() {
    const fs::path root = freshDirectory("api_operations");
    writeText(root / "models/crate.gltf", "crate");
    writeText(root / "models/crate.bin", "crate buffers");
    writeText(root / "textures/wood.png", "wood");
    writeText(root / "blocker.txt", "blocker");
    AssetDatabase database(root, seededOptions(2));
    expect(static_cast<bool>(database.rescan()), "scan before operations succeeds");
    const AssetGuid wood = *database.guidForPath("textures/wood.png");
    const AssetGuid crate = *database.guidForPath("models/crate.gltf");

    const AssetDatabaseResult renamed = database.rename("textures/wood.png", "oak.png");
    expect(renamed && renamed.changes.size() == 1 && renamed.changes[0].kind == AssetChangeKind::Moved &&
               renamed.changes[0].previousPath == "textures/wood.png" && renamed.changes[0].path == "textures/oak.png",
           "rename reports a move");
    expect(fs::exists(root / "textures/oak.png") && fs::exists(root / "textures/oak.png.meta") &&
               !fs::exists(root / "textures/wood.png.meta"),
           "rename carries the sidecar");
    expect(database.guidForPath("textures/oak.png") == wood && !database.guidForPath("textures/wood.png"),
           "rename keeps the GUID");

    const AssetDatabaseResult moved = database.move("textures/oak.png", "materials/wood/oak.png");
    expect(moved && fs::exists(root / "materials/wood/oak.png.meta"), "move creates parents and carries the sidecar");
    expect(database.pathForGuid(wood) == "materials/wood/oak.png", "move updates the GUID's path");

    expect(!database.move("models/crate.gltf", "blocker.txt"), "move refuses to overwrite a file");
    expect(!database.move("models/crate.gltf", ".hidden/crate.gltf"), "move refuses ignored destinations");
    expect(!database.move("models/crate.gltf", "../outside.gltf"), "move refuses paths outside the root");
    expect(!database.move("models/crate.gltf", "models/crate.gltf.meta"), "move refuses sidecar names");
    expect(!database.rename("models/crate.gltf", "sub/name.gltf"), "rename refuses nested names");
    expect(!database.move("missing.txt", "other.txt"), "move refuses untracked sources");
    expect(fs::exists(root / "models/crate.gltf"), "refused moves leave the file in place");

    const AssetDatabaseResult directoryMove = database.move("models", "content/props");
    expect(directoryMove && directoryMove.changes.size() == 2, "directory move reports each contained asset");
    expect(database.pathForGuid(crate) == "content/props/crate.gltf" &&
               fs::exists(root / "content/props/crate.gltf.meta"),
           "directory move carries sidecars");
    expect(!database.move("content", "content/inner"), "a directory cannot move into itself");

    const AssetDatabaseResult afterOperations = database.rescan();
    expect(afterOperations && afterOperations.changes.empty(), "API operations leave nothing for rescan to report");

    const AssetDatabaseResult removed = database.remove("blocker.txt");
    expect(removed && hasChange(removed, AssetChangeKind::Removed, "blocker.txt"), "remove reports the asset");
    expect(!fs::exists(root / "blocker.txt") && !fs::exists(root / "blocker.txt.meta"),
           "remove deletes the file and sidecar");
    const AssetDatabaseResult removedDirectory = database.remove("content");
    expect(removedDirectory && removedDirectory.changes.size() == 2 && !fs::exists(root / "content"),
           "removing a directory removes its assets");
    expect(database.assetCount() == 1 && database.find(wood) != nullptr, "unrelated assets survive removal");
    expect(!database.remove("blocker.txt"), "removing an untracked path fails");

    // Moving onto a path whose asset was deleted outside the editor before a
    // rescan replaces the stale record.
    writeText(root / "first.txt", "first");
    writeText(root / "second.txt", "second");
    expect(static_cast<bool>(database.rescan()), "rescan before a stale move succeeds");
    const AssetGuid first = *database.guidForPath("first.txt");
    const AssetGuid second = *database.guidForPath("second.txt");
    fs::remove(root / "second.txt");
    fs::remove(root / "second.txt.meta");
    const AssetDatabaseResult staleMove = database.rename("first.txt", "second.txt");
    expect(staleMove && hasChange(staleMove, AssetChangeKind::Removed, "second.txt") &&
               hasChange(staleMove, AssetChangeKind::Moved, "second.txt"),
           "moving onto a stale path reports the stale asset as removed");
    expect(database.guidForPath("second.txt") == first && database.find(second) == nullptr,
           "the moved asset owns the destination path");
    expect(database.rescan().changes.empty(), "a stale move leaves nothing for rescan to report");
    fs::remove_all(root);
}

void testCaseOnlyRenames() {
#ifdef _WIN32
    const fs::path root = freshDirectory("case_only_renames");
    writeText(root / "Stone.png", "stone");
    AssetDatabaseOptions options = seededOptions(9);
    options.orphanMetaPolicy = OrphanMetaPolicy::Delete;
    AssetDatabase database(root, options);
    expect(static_cast<bool>(database.rescan()), "scan before case-only renames succeeds");
    const AssetGuid stone = *database.guidForPath("Stone.png");

    const AssetDatabaseResult renamed = database.rename("Stone.png", "stone.png");
    expect(renamed && database.guidForPath("stone.png") == stone, "an API case-only rename keeps the GUID");

    // Renamed outside the editor without renaming the sidecar.
    fs::rename(root / "stone.png", root / "STONE.png");
    const AssetDatabaseResult scan = database.rescan();
    expect(scan && database.guidForPath("STONE.png") == stone, "an external case-only rename keeps the GUID");
    expect(scan.orphanMetaFiles.empty() && metaGuid(root / "STONE.png") == stone,
           "a sidecar differing only in case is not an orphan");
    fs::remove_all(root);
#endif
}

void testExternalChanges() {
    const fs::path root = freshDirectory("external_changes");
    writeText(root / "audio/theme.ogg", "theme-audio");
    writeText(root / "textures/brick.png", "brick-texture-bytes");
    writeText(root / "textures/sky.hdr", "sky");
    writeText(root / "level.scene", "level-scene-document");
    AssetDatabase database(root, seededOptions(3));
    expect(static_cast<bool>(database.rescan()), "scan before external changes succeeds");
    const AssetGuid theme = *database.guidForPath("audio/theme.ogg");
    const AssetGuid brick = *database.guidForPath("textures/brick.png");
    const AssetGuid sky = *database.guidForPath("textures/sky.hdr");
    const AssetGuid level = *database.guidForPath("level.scene");

    // Moved with its sidecar.
    fs::create_directories(root / "music");
    fs::rename(root / "audio/theme.ogg", root / "music/theme.ogg");
    fs::rename(root / "audio/theme.ogg.meta", root / "music/theme.ogg.meta");
    // Moved without its sidecar; the old sidecar is left behind.
    fs::create_directories(root / "textures/walls");
    fs::rename(root / "textures/brick.png", root / "textures/walls/brick.png");
    // Sidecar deleted.
    fs::remove(root / "textures/sky.hdr.meta");
    // Content edited.
    writeText(root / "level.scene", "level-scene-document-with-more-entities");

    const AssetDatabaseResult scan = database.rescan();
    expect(static_cast<bool>(scan), "rescan after external changes succeeds");
    expect(hasChange(scan, AssetChangeKind::Moved, "music/theme.ogg") &&
               database.guidForPath("music/theme.ogg") == theme,
           "a file moved with its sidecar keeps its GUID");
    expect(hasChange(scan, AssetChangeKind::Moved, "textures/walls/brick.png") &&
               database.guidForPath("textures/walls/brick.png") == brick &&
               metaGuid(root / "textures/walls/brick.png") == brick,
           "a file moved without its sidecar is matched by size and timestamp");
    expect(scan.orphanMetaFiles == std::vector<std::string>{"textures/brick.png.meta"} &&
               fs::exists(root / "textures/brick.png.meta"),
           "the left-behind sidecar is reported and kept by default");
    expect(database.guidForPath("textures/sky.hdr") == sky && metaGuid(root / "textures/sky.hdr") == sky,
           "a deleted sidecar is restored with the previous GUID");
    expect(hasChange(scan, AssetChangeKind::Modified, "level.scene") && database.guidForPath("level.scene") == level,
           "edited content is reported as modified");
    expect(!hasChange(scan, AssetChangeKind::Added, "music/theme.ogg") &&
               !hasChange(scan, AssetChangeKind::Removed, "audio/theme.ogg"),
           "moves are not reported as add/remove pairs");

    // Copied with its sidecar: the original keeps the GUID.
    fs::copy_file(root / "music/theme.ogg", root / "music/theme_copy.ogg");
    fs::copy_file(root / "music/theme.ogg.meta", root / "music/theme_copy.ogg.meta");
    const AssetDatabaseResult copied = database.rescan();
    const std::optional<AssetGuid> copy = database.guidForPath("music/theme_copy.ogg");
    expect(database.guidForPath("music/theme.ogg") == theme, "the original keeps its GUID after a copy");
    expect(copy && *copy != theme && metaGuid(root / "music/theme_copy.ogg") == *copy,
           "the copy receives and persists a fresh GUID");
    expect(hasChange(copied, AssetChangeKind::Added, "music/theme_copy.ogg") && copied.warnings.size() == 1,
           "the copy is added with a duplicate-GUID warning");

    // Deleted asset with the delete policy.
    AssetDatabaseOptions deleting = seededOptions(4);
    deleting.orphanMetaPolicy = OrphanMetaPolicy::Delete;
    AssetDatabase cleaner(root, deleting);
    expect(static_cast<bool>(cleaner.rescan()), "second database scans the same root");
    fs::remove(root / "level.scene");
    const AssetDatabaseResult deleted = cleaner.rescan();
    expect(hasChange(deleted, AssetChangeKind::Removed, "level.scene") && !cleaner.guidForPath("level.scene"),
           "an externally deleted asset is removed");
    expect(!fs::exists(root / "level.scene.meta") && !fs::exists(root / "textures/brick.png.meta"),
           "the delete policy removes orphan sidecars");
    fs::remove_all(root);
}

void testDuplicateGuidsOnFirstScan() {
    const fs::path root = freshDirectory("duplicate_first_scan");
    const AssetMeta shared{.guid = AssetGuid{7, 7}, .importSettings = {{"quality", 3}}};
    writeText(root / "b.txt", "b");
    writeText(root / "a.txt", "a");
    expect(static_cast<bool>(writeAssetMeta(root / "a.txt.meta", shared)) &&
               static_cast<bool>(writeAssetMeta(root / "b.txt.meta", shared)),
           "duplicate sidecars are prepared");
    AssetDatabase database(root, seededOptions(5));
    const AssetDatabaseResult scan = database.rescan();
    expect(scan && database.guidForPath("a.txt") == shared.guid, "the first path keeps a duplicated GUID");
    const std::optional<AssetGuid> second = database.guidForPath("b.txt");
    expect(second && *second != shared.guid, "the later path is reassigned");
    AssetMeta rewritten;
    expect(readAssetMeta(root / "b.txt.meta", rewritten) && rewritten.guid == *second &&
               rewritten.importSettings == shared.importSettings,
           "the reassigned sidecar keeps its import settings");
    fs::remove_all(root);
}

void testMalformedSidecars() {
    const fs::path root = freshDirectory("malformed_sidecars");
    writeText(root / "tracked.txt", "tracked");
    AssetDatabase database(root, seededOptions(6));
    expect(static_cast<bool>(database.rescan()), "scan before corruption succeeds");
    const AssetGuid tracked = *database.guidForPath("tracked.txt");

    writeText(root / "tracked.txt.meta", "<<<<<<< merge conflict");
    writeText(root / "fresh.txt", "fresh");
    writeText(root / "fresh.txt.meta", "{broken");
    const AssetDatabaseResult scan = database.rescan();
    expect(scan && scan.warnings.size() == 2, "each unreadable sidecar produces a warning");
    expect(database.guidForPath("tracked.txt") == tracked && scan.changes.empty(),
           "a tracked asset keeps its identity while its sidecar is unreadable");
    expect(readText(root / "tracked.txt.meta") == "<<<<<<< merge conflict" &&
               readText(root / "fresh.txt.meta") == "{broken",
           "unreadable sidecars are never overwritten");
    expect(!database.guidForPath("fresh.txt"), "a new asset with an unreadable sidecar stays untracked");
    fs::remove_all(root);
}

void testImportSettings() {
    const fs::path root = freshDirectory("import_settings");
    writeText(root / "rock.png", "rock");
    AssetDatabase database(root, seededOptions(7));
    expect(static_cast<bool>(database.rescan()), "scan before import settings succeeds");
    const AssetGuid rock = *database.guidForPath("rock.png");
    expect(!database.setImportSettings(rock, nlohmann::json::array()), "non-object settings are rejected");
    const AssetDatabaseResult set = database.setImportSettings(rock, {{"compression", "bc7"}});
    expect(set && hasChange(set, AssetChangeKind::Modified, "rock.png"), "setting import settings reports a change");
    AssetMeta meta;
    expect(readAssetMeta(root / "rock.png.meta", meta) && meta.importSettings["compression"] == "bc7",
           "import settings persist in the sidecar");
    expect(database.rescan().changes.empty(), "persisted settings do not appear as a later change");

    meta.importSettings["compression"] = "bc1";
    expect(static_cast<bool>(writeAssetMeta(root / "rock.png.meta", meta)), "external sidecar edit is written");
    const AssetDatabaseResult edited = database.rescan();
    expect(hasChange(edited, AssetChangeKind::Modified, "rock.png") &&
               database.find(rock)->importSettings["compression"] == "bc1",
           "an external settings edit is reported as modified");
    fs::remove_all(root);
}

void testDependencies() {
    const fs::path root = freshDirectory("dependencies");
    writeText(root / "level.scene", "level");
    writeText(root / "crate.prefab", "crate prefab");
    writeText(root / "crate.gltf", "crate model");
    writeText(root / "wood.png", "wood texture");
    AssetDatabase database(root, seededOptions(8));
    expect(static_cast<bool>(database.rescan()), "scan before dependencies succeeds");
    const AssetGuid level = *database.guidForPath("level.scene");
    const AssetGuid prefab = *database.guidForPath("crate.prefab");
    const AssetGuid model = *database.guidForPath("crate.gltf");
    const AssetGuid texture = *database.guidForPath("wood.png");

    const std::vector<AssetGuid> levelDependencies{prefab, model};
    database.setDependencies(level, levelDependencies);
    const std::vector<AssetGuid> prefabDependencies{model, prefab, AssetGuid{}};
    database.setDependencies(prefab, prefabDependencies);
    const std::vector<AssetGuid> modelDependencies{texture, level};
    database.setDependencies(model, modelDependencies);

    expect(database.dependencies(prefab) == std::vector<AssetGuid>{model}, "self and null edges are ignored");
    std::vector<AssetGuid> expectedDependents{level, prefab};
    std::ranges::sort(expectedDependents);
    expect(database.dependents(model) == expectedDependents, "dependents are sorted direct dependents");
    std::vector<AssetGuid> expectedTransitive{level, prefab, model};
    std::ranges::sort(expectedTransitive);
    expect(database.transitiveDependents(texture) == expectedTransitive, "transitive dependents follow chains");
    std::vector<AssetGuid> cyclic{prefab, model};
    std::ranges::sort(cyclic);
    expect(database.transitiveDependents(level) == cyclic, "transitive dependents tolerate cycles");

    expect(static_cast<bool>(database.rename("crate.gltf", "crate_v2.gltf")), "rename with dependencies succeeds");
    expect(database.dependents(texture) == std::vector<AssetGuid>{model}, "edges survive renames");

    const std::vector<AssetGuid> replacement{texture};
    database.setDependencies(level, replacement);
    expect(database.dependents(model) == std::vector<AssetGuid>{prefab}, "replacing edges drops old reverse edges");

    expect(static_cast<bool>(database.remove("crate.prefab")), "removing a dependent succeeds");
    expect(database.dependents(model).empty(), "a removed asset's outgoing edges are dropped");
    fs::remove(root / "crate_v2.gltf");
    expect(static_cast<bool>(database.rescan()), "rescan after deleting a dependency succeeds");
    expect(database.dependents(texture) == std::vector<AssetGuid>{level},
           "an externally removed asset's outgoing edges are dropped");
    expect(database.dependencies(level) == std::vector<AssetGuid>{texture}, "edges into removed assets are kept");
    fs::remove_all(root);
}
} // namespace

int main() {
    testContentHash();
    testAssetGuid();
    testAssetMeta();
    testInitialScanAndIgnoreRules();
    testApiMoveRenameRemove();
    testCaseOnlyRenames();
    testExternalChanges();
    testDuplicateGuidsOnFirstScan();
    testMalformedSidecars();
    testImportSettings();
    testDependencies();
    fs::remove_all(fs::temp_directory_path() / "zenith_asset_database_tests");
    if (failures != 0) {
        std::cerr << failures << " asset database test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "Asset database tests passed\n";
    return EXIT_SUCCESS;
}
