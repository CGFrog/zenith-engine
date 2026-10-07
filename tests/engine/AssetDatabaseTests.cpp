// CPU-only coverage of the asset database: content hashes, GUIDs and .meta
// sidecars, scanning and ignore rules, moves and renames through the API and
// outside it, duplicate and malformed sidecars, import settings, and the
// dependency graph.

#include "engine/assets/AssetDatabase.hpp"
#include "engine/assets/AssetGuid.hpp"
#include "engine/assets/AssetMeta.hpp"
#include "engine/assets/ContentHash.hpp"
#include "engine/assets/ProductionAssetPipeline.hpp"

#include "support/Doctest.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

TEST_SUITE_BEGIN("AssetDatabase");

namespace {
namespace fs = std::filesystem;

using testsupport::readText;
using testsupport::writeText;

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

} // namespace

TEST_CASE("AssetDatabase: content hash") {
    CHECK_MESSAGE(contentHash(std::string_view{}) == ContentHasher::offsetBasis,
                  "empty input hashes to the offset basis");
    CHECK_MESSAGE(formatContentHash(contentHash("a")) == "af63dc4c8601ec8c", "FNV-1a 64 of 'a' matches the reference");
    CHECK_MESSAGE(formatContentHash(contentHash("foobar")) == "85944171f73967e8",
                  "FNV-1a 64 of 'foobar' matches the reference");
    CHECK_MESSAGE(formatContentHash(1) == "0000000000000001", "hash text is zero padded");
    ContentHasher incremental;
    incremental.append("foo").append("bar");
    CHECK_MESSAGE(incremental.value() == contentHash("foobar"), "incremental hashing matches one-shot hashing");
    CHECK_MESSAGE(incremental.hex() == "85944171f73967e8", "hasher formats its own value");

    const testsupport::TempDirectory directory("asset-database-content-hash");
    writeText(directory / "source.bin", "foobar");
    ContentHasher fileHasher;
    CHECK_MESSAGE(fileHasher.appendFile(directory / "source.bin"), "existing file hashes");
    CHECK_MESSAGE(fileHasher.value() == contentHash("foobar"), "file hashing matches its bytes");
    ContentHasher missing;
    CHECK_MESSAGE(!missing.appendFile(directory / "missing.bin"), "missing file reports failure");

    // Derived cache names must stay byte-identical across the extraction:
    // source bytes then settings, tool version, and target, no separators.
    DerivedAssetCache cache(directory / "derived");
    std::string key;
    CHECK_MESSAGE(
        static_cast<bool>(cache.key(
            {.source = directory / "source.bin", .importSettings = "srgb", .toolVersion = "v1", .targetPlatform = "pc"},
            key)),
        "derived cache key succeeds");
    CHECK_MESSAGE(key == formatContentHash(contentHash("foobarsrgbv1pc")), "derived cache key is unchanged");
}

TEST_CASE("AssetDatabase: asset guid") {
    const AssetGuid guid{0x0123456789abcdefull, 0xfedcba9876543210ull};
    CHECK_MESSAGE(guid.toString() == "0123456789abcdeffedcba9876543210", "GUID text is lowercase high word first");
    CHECK_MESSAGE(AssetGuid::parse(guid.toString()) == guid, "GUID text round-trips");
    CHECK_MESSAGE(AssetGuid::parse("0123456789ABCDEFFEDCBA9876543210") == guid, "uppercase GUID text parses");
    CHECK_MESSAGE(!AssetGuid::parse("0123"), "short GUID text is rejected");
    CHECK_MESSAGE(!AssetGuid::parse("0123456789abcdeffedcba987654321g"), "non-hex GUID text is rejected");
    CHECK_MESSAGE((AssetGuid{}.isNull() && !AssetGuid{}), "default GUID is null");

    AssetGuidGenerator first(42);
    AssetGuidGenerator second(42);
    CHECK_MESSAGE(first.next() == second.next(), "seeded generators are deterministic");
    AssetGuidGenerator random;
    std::unordered_set<AssetGuid> seen;
    for (int index = 0; index < 1000; ++index) {
        const AssetGuid next = random.next();
        CHECK_MESSAGE(!next.isNull(), "generated GUIDs are never null");
        seen.insert(next);
    }
    CHECK_MESSAGE(seen.size() == 1000, "generated GUIDs are distinct and hashable");
}

TEST_CASE("AssetDatabase: asset meta") {
    AssetMeta meta{.guid = AssetGuid{1, 2}, .importSettings = {{"srgb", true}}};
    AssetMeta parsed;
    CHECK_MESSAGE(static_cast<bool>(parseAssetMeta(serializeAssetMeta(meta), parsed)), "meta round-trips");
    CHECK_MESSAGE((parsed.guid == meta.guid && parsed.importSettings == meta.importSettings), "meta fields round-trip");
    CHECK_MESSAGE(
        (static_cast<bool>(parseAssetMeta(R"({"version":1,"guid":"0000000000000000000000000000000a"})", parsed)) &&
         parsed.importSettings == nlohmann::json::object()),
        "missing import settings read as an empty object");
    CHECK_MESSAGE(!parseAssetMeta(R"({"version":2,"guid":"0000000000000000000000000000000a"})", parsed),
                  "future meta versions are rejected");
    CHECK_MESSAGE(!parseAssetMeta(R"({"version":1,"guid":"00000000000000000000000000000000"})", parsed),
                  "null GUIDs are rejected");
    CHECK_MESSAGE(
        !parseAssetMeta(R"({"version":1,"guid":"0000000000000000000000000000000a","importSettings":3})", parsed),
        "non-object import settings are rejected");
    CHECK_MESSAGE(!parseAssetMeta("{not json", parsed), "malformed JSON is rejected");

    const testsupport::TempDirectory directory("asset-database-meta");
    const fs::path metaPath = directory / "stone.png.meta";
    CHECK_MESSAGE(static_cast<bool>(writeAssetMeta(metaPath, meta)), "meta writes");
    CHECK_MESSAGE(static_cast<bool>(writeAssetMeta(metaPath, meta)), "meta overwrites in place");
    CHECK_MESSAGE(!fs::exists(directory / "stone.png.meta.tmp"), "atomic write leaves no temporary file");
    CHECK_MESSAGE((static_cast<bool>(readAssetMeta(metaPath, parsed)) && parsed.guid == meta.guid), "meta reads back");
    CHECK_MESSAGE(!writeAssetMeta(directory / "null.meta", AssetMeta{}), "null GUID meta is not written");
}

TEST_CASE("AssetDatabase: initial scan and ignore rules") {
    const testsupport::TempDirectory root("asset-database-initial-scan");
    writeText(root / "readme.txt", "r");
    writeText(root / "textures/stone.png", "png");
    writeText(root / ".git/config", "hidden");
    writeText(root / "build/output.bin", "ignored directory");
    writeText(root / "scratch.tmp", "temporary");
    writeText(root / "notes.LOG", "ignored extension");

    AssetDatabaseOptions options = seededOptions(1);
    options.ignoredNames = {"build"};
    options.ignoredExtensions = {".log"};
    AssetDatabase database(root.path(), options);
    const AssetDatabaseResult scan = database.rescan();
    CHECK_MESSAGE(static_cast<bool>(scan), "initial scan succeeds");
    CHECK_MESSAGE(database.assetCount() == 2, "only trackable files are assets");
    CHECK_MESSAGE((scan.changes.size() == 2 && scan.changes[0].path == "readme.txt" &&
                   scan.changes[1].path == "textures/stone.png" && scan.changes[0].kind == AssetChangeKind::Added),
                  "first scan reports every asset as added, sorted by path");
    CHECK_MESSAGE((fs::exists(root / "readme.txt.meta") && fs::exists(root / "textures/stone.png.meta")),
                  "missing sidecars are created");
    CHECK_MESSAGE((!fs::exists(root / "scratch.tmp.meta") && !fs::exists(root / "build/output.bin.meta") &&
                   !fs::exists(root / "notes.LOG.meta") && !fs::exists(root / ".git/config.meta")),
                  "ignored files get no sidecar");
    CHECK_MESSAGE(!fs::exists(root / "textures.meta"), "directories get no sidecar");

    const std::optional<AssetGuid> stone = database.guidForPath("textures/stone.png");
    CHECK_MESSAGE((stone.has_value() && metaGuid(root / "textures/stone.png") == *stone),
                  "sidecar holds the tracked GUID");
    CHECK_MESSAGE(database.guidForPath("textures\\..\\textures/./stone.png") == std::nullopt,
                  "parent-escaping lookups are rejected");
    CHECK_MESSAGE(database.guidForPath("textures//stone.png") == stone, "lookups normalize separators");
    CHECK_MESSAGE((stone && database.pathForGuid(*stone) == "textures/stone.png"), "GUID resolves to its path");
    const std::vector<AssetRecord> records = database.records();
    CHECK_MESSAGE((records.size() == 2 && records[0].path == "readme.txt" && records[1].path == "textures/stone.png"),
                  "records iterate by path");

    const AssetDatabaseResult unchanged = database.rescan();
    CHECK_MESSAGE((unchanged && unchanged.changes.empty() && unchanged.warnings.empty()),
                  "a quiet rescan reports nothing");

    AssetDatabase reopened(root.path(), seededOptions(99));
    const AssetDatabaseResult reopenedScan = reopened.rescan();
    CHECK_MESSAGE((reopenedScan && reopened.guidForPath("textures/stone.png") == stone),
                  "GUIDs persist across database instances");

    AssetDatabase missingRoot(root / "does-not-exist");
    CHECK_MESSAGE(!missingRoot.rescan(), "scanning a missing root fails");
}

TEST_CASE("AssetDatabase: api move rename remove") {
    const testsupport::TempDirectory root("asset-database-api-operations");
    writeText(root / "models/crate.gltf", "crate");
    writeText(root / "models/crate.bin", "crate buffers");
    writeText(root / "textures/wood.png", "wood");
    writeText(root / "blocker.txt", "blocker");
    AssetDatabase database(root.path(), seededOptions(2));
    CHECK_MESSAGE(static_cast<bool>(database.rescan()), "scan before operations succeeds");
    const AssetGuid wood = *database.guidForPath("textures/wood.png");
    const AssetGuid crate = *database.guidForPath("models/crate.gltf");

    const AssetDatabaseResult renamed = database.rename("textures/wood.png", "oak.png");
    CHECK_MESSAGE((renamed && renamed.changes.size() == 1 && renamed.changes[0].kind == AssetChangeKind::Moved &&
                   renamed.changes[0].previousPath == "textures/wood.png" &&
                   renamed.changes[0].path == "textures/oak.png"),
                  "rename reports a move");
    CHECK_MESSAGE((fs::exists(root / "textures/oak.png") && fs::exists(root / "textures/oak.png.meta") &&
                   !fs::exists(root / "textures/wood.png.meta")),
                  "rename carries the sidecar");
    CHECK_MESSAGE((database.guidForPath("textures/oak.png") == wood && !database.guidForPath("textures/wood.png")),
                  "rename keeps the GUID");

    const AssetDatabaseResult moved = database.move("textures/oak.png", "materials/wood/oak.png");
    CHECK_MESSAGE((moved && fs::exists(root / "materials/wood/oak.png.meta")),
                  "move creates parents and carries the sidecar");
    CHECK_MESSAGE(database.pathForGuid(wood) == "materials/wood/oak.png", "move updates the GUID's path");

    CHECK_MESSAGE(!database.move("models/crate.gltf", "blocker.txt"), "move refuses to overwrite a file");
    CHECK_MESSAGE(!database.move("models/crate.gltf", ".hidden/crate.gltf"), "move refuses ignored destinations");
    CHECK_MESSAGE(!database.move("models/crate.gltf", "../outside.gltf"), "move refuses paths outside the root");
    CHECK_MESSAGE(!database.move("models/crate.gltf", "models/crate.gltf.meta"), "move refuses sidecar names");
    CHECK_MESSAGE(!database.rename("models/crate.gltf", "sub/name.gltf"), "rename refuses nested names");
    CHECK_MESSAGE(!database.move("missing.txt", "other.txt"), "move refuses untracked sources");
    CHECK_MESSAGE(fs::exists(root / "models/crate.gltf"), "refused moves leave the file in place");

    const AssetDatabaseResult directoryMove = database.move("models", "content/props");
    CHECK_MESSAGE((directoryMove && directoryMove.changes.size() == 2), "directory move reports each contained asset");
    CHECK_MESSAGE((database.pathForGuid(crate) == "content/props/crate.gltf" &&
                   fs::exists(root / "content/props/crate.gltf.meta")),
                  "directory move carries sidecars");
    CHECK_MESSAGE(!database.move("content", "content/inner"), "a directory cannot move into itself");

    const AssetDatabaseResult afterOperations = database.rescan();
    CHECK_MESSAGE((afterOperations && afterOperations.changes.empty()),
                  "API operations leave nothing for rescan to report");

    const AssetDatabaseResult removed = database.remove("blocker.txt");
    CHECK_MESSAGE((removed && hasChange(removed, AssetChangeKind::Removed, "blocker.txt")), "remove reports the asset");
    CHECK_MESSAGE((!fs::exists(root / "blocker.txt") && !fs::exists(root / "blocker.txt.meta")),
                  "remove deletes the file and sidecar");
    const AssetDatabaseResult removedDirectory = database.remove("content");
    CHECK_MESSAGE((removedDirectory && removedDirectory.changes.size() == 2 && !fs::exists(root / "content")),
                  "removing a directory removes its assets");
    CHECK_MESSAGE((database.assetCount() == 1 && database.find(wood) != nullptr), "unrelated assets survive removal");
    CHECK_MESSAGE(!database.remove("blocker.txt"), "removing an untracked path fails");

    // Moving onto a path whose asset was deleted outside the editor before a
    // rescan replaces the stale record.
    writeText(root / "first.txt", "first");
    writeText(root / "second.txt", "second");
    CHECK_MESSAGE(static_cast<bool>(database.rescan()), "rescan before a stale move succeeds");
    const AssetGuid first = *database.guidForPath("first.txt");
    const AssetGuid second = *database.guidForPath("second.txt");
    fs::remove(root / "second.txt");
    fs::remove(root / "second.txt.meta");
    const AssetDatabaseResult staleMove = database.rename("first.txt", "second.txt");
    CHECK_MESSAGE((staleMove && hasChange(staleMove, AssetChangeKind::Removed, "second.txt") &&
                   hasChange(staleMove, AssetChangeKind::Moved, "second.txt")),
                  "moving onto a stale path reports the stale asset as removed");
    CHECK_MESSAGE((database.guidForPath("second.txt") == first && database.find(second) == nullptr),
                  "the moved asset owns the destination path");
    CHECK_MESSAGE(database.rescan().changes.empty(), "a stale move leaves nothing for rescan to report");
}

TEST_CASE("AssetDatabase: case only renames") {
#ifdef _WIN32
    const testsupport::TempDirectory root("asset-database-case-only-renames");
    writeText(root / "Stone.png", "stone");
    AssetDatabaseOptions options = seededOptions(9);
    options.orphanMetaPolicy = OrphanMetaPolicy::Delete;
    AssetDatabase database(root.path(), options);
    CHECK_MESSAGE(static_cast<bool>(database.rescan()), "scan before case-only renames succeeds");
    const AssetGuid stone = *database.guidForPath("Stone.png");

    const AssetDatabaseResult renamed = database.rename("Stone.png", "stone.png");
    CHECK_MESSAGE((renamed && database.guidForPath("stone.png") == stone), "an API case-only rename keeps the GUID");

    // Renamed outside the editor without renaming the sidecar.
    fs::rename(root / "stone.png", root / "STONE.png");
    const AssetDatabaseResult scan = database.rescan();
    CHECK_MESSAGE((scan && database.guidForPath("STONE.png") == stone), "an external case-only rename keeps the GUID");
    CHECK_MESSAGE((scan.orphanMetaFiles.empty() && metaGuid(root / "STONE.png") == stone),
                  "a sidecar differing only in case is not an orphan");
#endif
}

TEST_CASE("AssetDatabase: external changes") {
    const testsupport::TempDirectory root("asset-database-external-changes");
    writeText(root / "audio/theme.ogg", "theme-audio");
    writeText(root / "textures/brick.png", "brick-texture-bytes");
    writeText(root / "textures/sky.hdr", "sky");
    writeText(root / "level.scene", "level-scene-document");
    AssetDatabase database(root.path(), seededOptions(3));
    CHECK_MESSAGE(static_cast<bool>(database.rescan()), "scan before external changes succeeds");
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
    CHECK_MESSAGE(static_cast<bool>(scan), "rescan after external changes succeeds");
    CHECK_MESSAGE((hasChange(scan, AssetChangeKind::Moved, "music/theme.ogg") &&
                   database.guidForPath("music/theme.ogg") == theme),
                  "a file moved with its sidecar keeps its GUID");
    CHECK_MESSAGE((hasChange(scan, AssetChangeKind::Moved, "textures/walls/brick.png") &&
                   database.guidForPath("textures/walls/brick.png") == brick &&
                   metaGuid(root / "textures/walls/brick.png") == brick),
                  "a file moved without its sidecar is matched by size and timestamp");
    CHECK_MESSAGE((scan.orphanMetaFiles == std::vector<std::string>{"textures/brick.png.meta"} &&
                   fs::exists(root / "textures/brick.png.meta")),
                  "the left-behind sidecar is reported and kept by default");
    CHECK_MESSAGE((database.guidForPath("textures/sky.hdr") == sky && metaGuid(root / "textures/sky.hdr") == sky),
                  "a deleted sidecar is restored with the previous GUID");
    CHECK_MESSAGE(
        (hasChange(scan, AssetChangeKind::Modified, "level.scene") && database.guidForPath("level.scene") == level),
        "edited content is reported as modified");
    CHECK_MESSAGE((!hasChange(scan, AssetChangeKind::Added, "music/theme.ogg") &&
                   !hasChange(scan, AssetChangeKind::Removed, "audio/theme.ogg")),
                  "moves are not reported as add/remove pairs");

    // Copied with its sidecar: the original keeps the GUID.
    fs::copy_file(root / "music/theme.ogg", root / "music/theme_copy.ogg");
    fs::copy_file(root / "music/theme.ogg.meta", root / "music/theme_copy.ogg.meta");
    const AssetDatabaseResult copied = database.rescan();
    const std::optional<AssetGuid> copy = database.guidForPath("music/theme_copy.ogg");
    CHECK_MESSAGE(database.guidForPath("music/theme.ogg") == theme, "the original keeps its GUID after a copy");
    CHECK_MESSAGE((copy && *copy != theme && metaGuid(root / "music/theme_copy.ogg") == *copy),
                  "the copy receives and persists a fresh GUID");
    CHECK_MESSAGE((hasChange(copied, AssetChangeKind::Added, "music/theme_copy.ogg") && copied.warnings.size() == 1),
                  "the copy is added with a duplicate-GUID warning");

    // Deleted asset with the delete policy.
    AssetDatabaseOptions deleting = seededOptions(4);
    deleting.orphanMetaPolicy = OrphanMetaPolicy::Delete;
    AssetDatabase cleaner(root.path(), deleting);
    CHECK_MESSAGE(static_cast<bool>(cleaner.rescan()), "second database scans the same root");
    fs::remove(root / "level.scene");
    const AssetDatabaseResult deleted = cleaner.rescan();
    CHECK_MESSAGE((hasChange(deleted, AssetChangeKind::Removed, "level.scene") && !cleaner.guidForPath("level.scene")),
                  "an externally deleted asset is removed");
    CHECK_MESSAGE((!fs::exists(root / "level.scene.meta") && !fs::exists(root / "textures/brick.png.meta")),
                  "the delete policy removes orphan sidecars");
}

TEST_CASE("AssetDatabase: duplicate guids on first scan") {
    const testsupport::TempDirectory root("asset-database-duplicate-first-scan");
    const AssetMeta shared{.guid = AssetGuid{7, 7}, .importSettings = {{"quality", 3}}};
    writeText(root / "b.txt", "b");
    writeText(root / "a.txt", "a");
    CHECK_MESSAGE((static_cast<bool>(writeAssetMeta(root / "a.txt.meta", shared)) &&
                   static_cast<bool>(writeAssetMeta(root / "b.txt.meta", shared))),
                  "duplicate sidecars are prepared");
    AssetDatabase database(root.path(), seededOptions(5));
    const AssetDatabaseResult scan = database.rescan();
    CHECK_MESSAGE((scan && database.guidForPath("a.txt") == shared.guid), "the first path keeps a duplicated GUID");
    const std::optional<AssetGuid> second = database.guidForPath("b.txt");
    CHECK_MESSAGE((second && *second != shared.guid), "the later path is reassigned");
    AssetMeta rewritten;
    CHECK_MESSAGE((readAssetMeta(root / "b.txt.meta", rewritten) && rewritten.guid == *second &&
                   rewritten.importSettings == shared.importSettings),
                  "the reassigned sidecar keeps its import settings");
}

TEST_CASE("AssetDatabase: malformed sidecars") {
    const testsupport::TempDirectory root("asset-database-malformed-sidecars");
    writeText(root / "tracked.txt", "tracked");
    AssetDatabase database(root.path(), seededOptions(6));
    CHECK_MESSAGE(static_cast<bool>(database.rescan()), "scan before corruption succeeds");
    const AssetGuid tracked = *database.guidForPath("tracked.txt");

    writeText(root / "tracked.txt.meta", "<<<<<<< merge conflict");
    writeText(root / "fresh.txt", "fresh");
    writeText(root / "fresh.txt.meta", "{broken");
    const AssetDatabaseResult scan = database.rescan();
    CHECK_MESSAGE((scan && scan.warnings.size() == 2), "each unreadable sidecar produces a warning");
    CHECK_MESSAGE((database.guidForPath("tracked.txt") == tracked && scan.changes.empty()),
                  "a tracked asset keeps its identity while its sidecar is unreadable");
    CHECK_MESSAGE((readText(root / "tracked.txt.meta") == "<<<<<<< merge conflict" &&
                   readText(root / "fresh.txt.meta") == "{broken"),
                  "unreadable sidecars are never overwritten");
    CHECK_MESSAGE(!database.guidForPath("fresh.txt"), "a new asset with an unreadable sidecar stays untracked");
}

TEST_CASE("AssetDatabase: import settings") {
    const testsupport::TempDirectory root("asset-database-import-settings");
    writeText(root / "rock.png", "rock");
    AssetDatabase database(root.path(), seededOptions(7));
    CHECK_MESSAGE(static_cast<bool>(database.rescan()), "scan before import settings succeeds");
    const AssetGuid rock = *database.guidForPath("rock.png");
    CHECK_MESSAGE(!database.setImportSettings(rock, nlohmann::json::array()), "non-object settings are rejected");
    const AssetDatabaseResult set = database.setImportSettings(rock, {{"compression", "bc7"}});
    CHECK_MESSAGE((set && hasChange(set, AssetChangeKind::Modified, "rock.png")),
                  "setting import settings reports a change");
    AssetMeta meta;
    CHECK_MESSAGE((readAssetMeta(root / "rock.png.meta", meta) && meta.importSettings["compression"] == "bc7"),
                  "import settings persist in the sidecar");
    CHECK_MESSAGE(database.rescan().changes.empty(), "persisted settings do not appear as a later change");

    meta.importSettings["compression"] = "bc1";
    CHECK_MESSAGE(static_cast<bool>(writeAssetMeta(root / "rock.png.meta", meta)), "external sidecar edit is written");
    const AssetDatabaseResult edited = database.rescan();
    CHECK_MESSAGE((hasChange(edited, AssetChangeKind::Modified, "rock.png") &&
                   database.find(rock)->importSettings["compression"] == "bc1"),
                  "an external settings edit is reported as modified");
}

TEST_CASE("AssetDatabase: dependencies") {
    const testsupport::TempDirectory root("asset-database-dependencies");
    writeText(root / "level.scene", "level");
    writeText(root / "crate.prefab", "crate prefab");
    writeText(root / "crate.gltf", "crate model");
    writeText(root / "wood.png", "wood texture");
    AssetDatabase database(root.path(), seededOptions(8));
    CHECK_MESSAGE(static_cast<bool>(database.rescan()), "scan before dependencies succeeds");
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

    CHECK_MESSAGE(database.dependencies(prefab) == std::vector<AssetGuid>{model}, "self and null edges are ignored");
    std::vector<AssetGuid> expectedDependents{level, prefab};
    std::ranges::sort(expectedDependents);
    CHECK_MESSAGE(database.dependents(model) == expectedDependents, "dependents are sorted direct dependents");
    std::vector<AssetGuid> expectedTransitive{level, prefab, model};
    std::ranges::sort(expectedTransitive);
    CHECK_MESSAGE(database.transitiveDependents(texture) == expectedTransitive, "transitive dependents follow chains");
    std::vector<AssetGuid> cyclic{prefab, model};
    std::ranges::sort(cyclic);
    CHECK_MESSAGE(database.transitiveDependents(level) == cyclic, "transitive dependents tolerate cycles");

    CHECK_MESSAGE(static_cast<bool>(database.rename("crate.gltf", "crate_v2.gltf")),
                  "rename with dependencies succeeds");
    CHECK_MESSAGE(database.dependents(texture) == std::vector<AssetGuid>{model}, "edges survive renames");

    const std::vector<AssetGuid> replacement{texture};
    database.setDependencies(level, replacement);
    CHECK_MESSAGE(database.dependents(model) == std::vector<AssetGuid>{prefab},
                  "replacing edges drops old reverse edges");

    CHECK_MESSAGE(static_cast<bool>(database.remove("crate.prefab")), "removing a dependent succeeds");
    CHECK_MESSAGE(database.dependents(model).empty(), "a removed asset's outgoing edges are dropped");
    fs::remove(root / "crate_v2.gltf");
    CHECK_MESSAGE(static_cast<bool>(database.rescan()), "rescan after deleting a dependency succeeds");
    CHECK_MESSAGE(database.dependents(texture) == std::vector<AssetGuid>{level},
                  "an externally removed asset's outgoing edges are dropped");
    CHECK_MESSAGE(database.dependencies(level) == std::vector<AssetGuid>{texture},
                  "edges into removed assets are kept");
}

TEST_SUITE_END();
