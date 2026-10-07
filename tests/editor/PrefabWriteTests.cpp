// Undoable prefab file writes: PrefabWriteCommand through the prefab
// operators, the PrefabWriteLedger the asset watcher consults, and refreshing
// instances after a file changed outside the editor.

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/application/PrefabInstanceRefresh.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/document/PrefabAuthoring.hpp"
#include "editor/document/PrefabEditDocument.hpp"
#include "editor/document/PrefabWriteCommand.hpp"
#include "editor/operators/OperatorRunner.hpp"
#include "editor/operators/PrefabOperators.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/Prefab.hpp"
#include "engine/scene/SceneDocument.hpp"

#include "support/Doctest.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

using testsupport::readText;
using testsupport::writeText;

TEST_SUITE_BEGIN("PrefabWrite");

namespace {
// The session's side of the write hooks: a ledger, the writes, and the scene
// each write came from.
struct WriteRecorder {
    PrefabWriteLedger ledger;
    std::vector<std::optional<std::string>> writes;
    std::vector<const Scene*> origins;

    PrefabWriteHooks hooks() {
        return {.fileWritten =
                    [this](const std::filesystem::path& file, const std::optional<std::string>& bytes,
                           const Scene* origin) {
                        ledger.record(file, bytes);
                        writes.push_back(bytes);
                        origins.push_back(origin);
                    },
                // As the session: the first content seen is what an outside
                // change is compared with.
                .fileRead =
                    [this](const std::filesystem::path& file, const std::optional<std::string>& bytes) {
                        if (ledger.recorded(file) == nullptr) {
                            ledger.record(file, bytes);
                        }
                    }};
    }
};

// A headless document, history, and selection, as the session owns them,
// with the prefab operators reporting writes to recorder.
struct Fixture {
    testsupport::TempDirectory directory;
    std::filesystem::path root;
    AssetManager assets;
    Scene scene;
    EditorDocument document;
    CommandHistory history;
    EditorDiagnostics diagnostics;
    Selection selection;
    WriteRecorder recorder;
    OperatorRegistry registry;
    OperatorRunner runner{registry, history};

    explicit Fixture(std::string_view name)
        : directory(name)
        , root(directory.path())
        , document(root) {
        CHECK_MESSAGE(registerPrefabOperators(registry, {.writeHooks = recorder.hooks()}).success,
                      "the prefab operators should register");
    }

    SceneEditing editing() {
        return SceneEditing(scene, assets, document, history, diagnostics);
    }

    OperatorResult invoke(std::string_view id, std::string_view properties = {}) {
        OperatorContext context{.editing = editing(), .selection = selection};
        return runner.invokeWithJson(id, context, properties);
    }

    void edit(const std::function<void()>& change) {
        editing().perform("Edit", {}, [&change] {
            change();
            return SceneResult{.success = true};
        });
    }

    Entity find(SceneEntityId id) const {
        return scene.findEntity(id);
    }

    float intensity(SceneEntityId id) const {
        return scene.registry().get<PointLightComponent>(find(id)).intensity;
    }
};

// A lamp prefab at lamp.prefab with two instances; returns their ids.
std::pair<SceneEntityId, SceneEntityId> twoLamps(Fixture& fixture) {
    Scene& scene = fixture.scene;
    const Entity lamp = scene.createEntity("Lamp");
    scene.registry().emplace<PointLightComponent>(lamp, glm::vec3(1.0f), 10.0f, 4.0f);
    fixture.selection.selectOnly(scene.id(lamp));
    CHECK_MESSAGE(fixture.invoke("prefab.create_from_selection", R"({"path":"lamp.prefab"})").status ==
                      OperatorStatus::Finished,
                  "the lamp prefab should be created");
    const SceneEntityId first = scene.id(lamp);
    CHECK_MESSAGE(fixture.invoke("prefab.instantiate", R"({"asset":"lamp.prefab"})").status == OperatorStatus::Finished,
                  "a second lamp should instantiate");
    return {first, *fixture.selection.active()};
}

} // namespace

TEST_CASE("PrefabWrite: undoable apply") {
    Fixture fixture("prefab-write-apply");
    const std::filesystem::path file = fixture.root / "lamp.prefab";
    const auto [first, second] = twoLamps(fixture);
    CHECK_MESSAGE((fixture.recorder.writes.size() == 1 && fixture.recorder.ledger.matches(file, readText(file))),
                  "creating a prefab should report its write to the hooks");
    const std::string before = readText(file);
    fixture.edit([&] { fixture.scene.registry().get<PointLightComponent>(fixture.find(first)).intensity = 30.0f; });
    fixture.selection.selectOnly(first);
    const size_t steps = fixture.history.size();
    CHECK_MESSAGE(
        (fixture.invoke("prefab.apply").status == OperatorStatus::Finished && fixture.intensity(second) == 30.0f),
        "apply should update the other instance");
    CHECK_MESSAGE(fixture.history.size() == steps + 1, "apply should record one step for the file and the scene");
    const std::string after = readText(file);
    CHECK_MESSAGE((after != before && fixture.recorder.ledger.matches(file, after)),
                  "the applied file should be in the ledger");
    CHECK_MESSAGE(fixture.recorder.origins.back() == &fixture.scene, "apply should name the scene it wrote from");

    fixture.editing().undo();
    CHECK_MESSAGE((readText(file) == before && fixture.intensity(second) == 10.0f && fixture.intensity(first) == 30.0f),
                  "undo should restore the file and the other instance");
    CHECK_MESSAGE(fixture.recorder.ledger.matches(file, before), "undo's write should be in the ledger");
    CHECK_MESSAGE(fixture.recorder.origins.back() == &fixture.scene, "undo should name the scene it wrote from");
    fixture.editing().redo();
    CHECK_MESSAGE((readText(file) == after && fixture.intensity(second) == 30.0f),
                  "redo should write the file and refresh the other instance again");
    CHECK_MESSAGE(fixture.recorder.ledger.matches(file, after), "redo's write should be in the ledger");

    // Applying the unchanged instance writes the same content: still one
    // step, no file write recorded.
    fixture.selection.selectOnly(first);
    const size_t writes = fixture.recorder.writes.size();
    CHECK_MESSAGE((fixture.invoke("prefab.apply").status == OperatorStatus::Finished && readText(file) == after &&
                   fixture.recorder.writes.size() == writes + 1),
                  "an apply rewriting the same content should still tell the hooks");
}

TEST_CASE("PrefabWrite: undo refuses outside change") {
    Fixture fixture("prefab-write-outside");
    const std::filesystem::path file = fixture.root / "lamp.prefab";
    const auto [first, second] = twoLamps(fixture);
    fixture.edit([&] { fixture.scene.registry().get<PointLightComponent>(fixture.find(first)).intensity = 30.0f; });
    fixture.selection.selectOnly(first);
    CHECK_MESSAGE(fixture.invoke("prefab.apply").status == OperatorStatus::Finished, "the lamp should apply");

    const std::string outside = readText(file) + "\n";
    writeText(file, outside);
    fixture.diagnostics.acknowledgeErrors();
    fixture.editing().undo();
    CHECK_MESSAGE((readText(file) == outside && fixture.intensity(second) == 30.0f),
                  "undo should change neither the file nor the scene when the file changed on disk");
    CHECK_MESSAGE(
        (fixture.diagnostics.hasUnacknowledgedError() && fixture.history.canUndo() && !fixture.history.canRedo()),
        "the refused undo should be reported and stay on the timeline");
    const auto& entries = fixture.diagnostics.entries();
    CHECK_MESSAGE(
        (!entries.empty() &&
         entries.back().message.find("lamp.prefab changed on disk since this step; undo skipped") != std::string::npos),
        "the error should name the file and say undo was skipped");
    fixture.diagnostics.acknowledgeErrors();

    // Once the file holds the step's content again, undo works; a change
    // before redo blocks redo the same way.
    PrefabWriteCommand probe("Probe", {.file = file, .before = std::string("a"), .after = std::string("b")}, {});
    EditorCommandContext context = fixture.editing().commandContext();
    writeText(file, "c");
    const EditorCommandResult redo = probe.execute(context);
    CHECK_MESSAGE((!redo && redo.error.find("redo skipped") != std::string::npos && readText(file) == "c"),
                  "redo should refuse a file that changed on disk");
    writeText(file, "a");
    CHECK_MESSAGE((probe.execute(context).success && readText(file) == "b"), "redo should write the step's content");
    CHECK_MESSAGE((probe.undo(context).success && readText(file) == "a"), "undo should write the previous content");
}

TEST_CASE("PrefabWrite: undo create removes file") {
    Fixture fixture("prefab-write-create");
    const Entity lamp = fixture.scene.createEntity("Lamp");
    fixture.selection.selectOnly(fixture.scene.id(lamp));
    CHECK_MESSAGE(fixture.invoke("prefab.create_from_selection", R"({"path":"props/lamp.prefab"})").status ==
                      OperatorStatus::Finished,
                  "the prefab should be created");
    const std::filesystem::path file = fixture.root / "props/lamp.prefab";
    const std::string created = readText(file);
    fixture.editing().undo();
    CHECK_MESSAGE((!std::filesystem::exists(file) && fixture.recorder.ledger.matches(file, std::nullopt)),
                  "undoing create should remove the file and note the removal");
    fixture.editing().redo();
    CHECK_MESSAGE(readText(file) == created, "redoing create should write the file again");

    // A file edited outside the editor is never removed by undo.
    writeText(file, created + " ");
    fixture.editing().undo();
    CHECK_MESSAGE(std::filesystem::exists(file), "undoing create should keep a file changed on disk");
    fixture.diagnostics.acknowledgeErrors();
}

TEST_CASE("PrefabWrite: ledger") {
    const testsupport::TempDirectory directory("prefab-write-ledger");
    const std::filesystem::path& root = directory.path();
    PrefabWriteLedger ledger;
    const std::filesystem::path file = root / "props" / ".." / "lamp.prefab";
    CHECK_MESSAGE((!ledger.matches(file, std::string("x")) && ledger.recorded(file) == nullptr),
                  "an empty ledger should match nothing");
    ledger.record(file, std::string("x"));
    CHECK_MESSAGE((ledger.matches(root / "lamp.prefab", std::string("x")) && !ledger.matches(file, std::string("y")) &&
                   !ledger.matches(file, std::nullopt)),
                  "the ledger should compare normalized paths and whole contents");
    ledger.record(file, std::nullopt);
    CHECK_MESSAGE((ledger.matches(file, std::nullopt) && ledger.recorded(file) != nullptr && !*ledger.recorded(file)),
                  "the ledger should note a removed file");
    ledger.forget(file);
    CHECK_MESSAGE(ledger.recorded(file) == nullptr, "forget should drop the entry");
}

TEST_CASE("PrefabWrite: external change refreshes instances") {
    Fixture fixture("prefab-write-external");
    const std::filesystem::path file = fixture.root / "lamp.prefab";
    const auto [first, second] = twoLamps(fixture);
    // The second instance overrides the range; that must survive.
    fixture.edit([&] { fixture.scene.registry().get<PointLightComponent>(fixture.find(second)).range = 9.0f; });

    // The editor's own write is not an outside change.
    PrefabFileChange change;
    CHECK_MESSAGE((classifyPrefabFileChange(fixture.recorder.ledger, file, fixture.root, change).success &&
                   !change.external && change.reference == "lamp.prefab" && change.displayName == "Lamp"),
                  "a file holding the editor's last write should not count as changed outside");

    // Another tool raises the intensity in the file.
    PrefabDocument document;
    CHECK_MESSAGE(loadPrefab(file, document).success, "the prefab should load");
    const auto light = std::ranges::find(document.entities.front().components, std::string("point_light"),
                                         &std::pair<std::string, std::string>::first);
    if (light == document.entities.front().components.end()) {
        CHECK_MESSAGE(false, "the prefab should hold its light");
        return;
    }
    nlohmann::json value = nlohmann::json::parse(light->second);
    value["intensity"] = 55.0f;
    light->second = value.dump();
    CHECK_MESSAGE(savePrefab(file, document).success, "the outside edit should save");

    CHECK_MESSAGE((classifyPrefabFileChange(fixture.recorder.ledger, file, fixture.root, change).success &&
                   change.external && change.previous.has_value()),
                  "an outside change should be reported with the content before it");
    const size_t steps = fixture.history.size();
    size_t refreshed = 0;
    const PrefabResult result =
        refreshPrefabInstances(fixture.editing(), change.reference, change.previous ? &*change.previous : nullptr,
                               externalPrefabRefreshStepName(change.displayName), &refreshed);
    CHECK_MESSAGE(
        (result.success && refreshed == 2 && fixture.intensity(first) == 55.0f && fixture.intensity(second) == 55.0f),
        "every instance should take the outside change");
    CHECK_MESSAGE(fixture.scene.registry().get<PointLightComponent>(fixture.find(second)).range == 9.0f,
                  "an instance should keep its own overrides through the refresh");
    CHECK_MESSAGE((fixture.history.size() == steps + 1 && fixture.history.canUndo()),
                  "the refresh should be one undo step");
    CHECK_MESSAGE(externalPrefabRefreshStepName("Lamp") == "Refresh Lamp instances (changed on disk)",
                  "the step should be named after the prefab");

    // The watcher reports the same change again: nothing more happens.
    CHECK_MESSAGE(
        (classifyPrefabFileChange(fixture.recorder.ledger, file, fixture.root, change).success && !change.external),
        "a repeated report of a handled change should be ignored");

    fixture.editing().undo();
    CHECK_MESSAGE((fixture.intensity(second) == 10.0f && fixture.intensity(first) == 10.0f),
                  "undoing the refresh should restore the instances");
    fixture.editing().redo();
    CHECK_MESSAGE(fixture.intensity(second) == 55.0f, "redoing the refresh should apply it again");
}

TEST_CASE("PrefabWrite: refresh without previous content") {
    Fixture fixture("prefab-write-unknown");
    const std::filesystem::path file = fixture.root / "lamp.prefab";
    const auto [first, second] = twoLamps(fixture);
    PrefabWriteLedger fresh;
    PrefabDocument document;
    CHECK_MESSAGE(loadPrefab(file, document).success, "the prefab should load");
    document.entities.front().name = "Renamed lamp";
    CHECK_MESSAGE(savePrefab(file, document).success, "the outside edit should save");
    PrefabFileChange change;
    CHECK_MESSAGE((classifyPrefabFileChange(fresh, file, fixture.root, change).success && change.external &&
                   !change.previous.has_value()),
                  "a change to a file the ledger never saw should have no previous content");
    CHECK_MESSAGE((refreshPrefabInstances(fixture.editing(), change.reference, nullptr, "Refresh", nullptr).success &&
                   fixture.scene.registry().get<NameComponent>(fixture.find(second)).name == "Renamed lamp"),
                  "instances should refresh from their persisted overrides when the previous content is unknown");
    CHECK_MESSAGE(!classifyPrefabFileChange(fresh, fixture.root / ".." / "elsewhere.prefab", fixture.root, change),
                  "a file outside the asset root should be rejected");
    static_cast<void>(first);
}

TEST_CASE("PrefabWrite: refresh parity with apply") {
    // refreshInstances does what apply did to the other instances: the same
    // file change through apply and through the shared refresh gives the
    // same scenes.
    const auto build = [](const std::filesystem::path& root, Scene& scene, SceneEntityId& firstId,
                          SceneEntityId& secondId) {
        AssetManager assets;
        const SceneDocumentOptions options{.assetRoot = root};
        PrefabAuthoring authoring(assets, options);
        Scene authored;
        const Entity wheel = authored.createEntity("Wheel");
        authored.registry().emplace<PointLightComponent>(wheel);
        CHECK_MESSAGE(authoring.create(authored, wheel, "wheel.prefab").success, "the parity wheel should be created");
        const Entity cart = authored.createEntity("Cart");
        authored.setParent(authored.createEntity("Axle"), cart);
        authored.setParent(wheel, cart);
        CHECK_MESSAGE(authoring.create(authored, cart, "cart.prefab").success, "the parity cart should be created");
        Entity first = entt::null;
        Entity second = entt::null;
        CHECK_MESSAGE((authoring.instantiate(scene, "cart.prefab", entt::null, first).success &&
                       authoring.instantiate(scene, "cart.prefab", entt::null, second).success),
                      "two parity carts should instantiate");
        firstId = scene.id(first);
        secondId = scene.id(second);
    };
    const auto childNamed = [](const Scene& scene, Entity parent, const std::string& name) {
        for (const Entity child : scene.registry().get<RelationshipComponent>(parent).children) {
            if (scene.registry().get<NameComponent>(child).name == name) {
                return child;
            }
        }
        return Entity{entt::null};
    };
    const auto editFirst = [&](Scene& scene, SceneEntityId firstId) {
        const Entity first = scene.findEntity(firstId);
        scene.registry().get<PointLightComponent>(childNamed(scene, first, "Wheel")).intensity = 8.0f;
        scene.destroyEntity(childNamed(scene, first, "Axle"));
        scene.setParent(scene.createEntity("Seat"), first);
    };

    AssetManager assets;
    const testsupport::TempDirectory applyRootDirectory("prefab-write-parity-apply");
    const std::filesystem::path& applyRoot = applyRootDirectory.path();
    Scene applied;
    SceneEntityId appliedFirst;
    SceneEntityId appliedSecond;
    build(applyRoot, applied, appliedFirst, appliedSecond);
    editFirst(applied, appliedFirst);
    PrefabAuthoring applyAuthoring(assets, SceneDocumentOptions{.assetRoot = applyRoot});
    CHECK_MESSAGE(applyAuthoring.apply(applied, applied.findEntity(appliedFirst)).success,
                  "the parity apply should succeed");

    // The same content arrives from outside: write it, then refresh both
    // instances against the content before.
    const testsupport::TempDirectory refreshRootDirectory("prefab-write-parity-refresh");
    const std::filesystem::path& refreshRoot = refreshRootDirectory.path();
    Scene refreshed;
    SceneEntityId refreshedFirst;
    SceneEntityId refreshedSecond;
    build(refreshRoot, refreshed, refreshedFirst, refreshedSecond);
    PrefabDocument previous;
    CHECK_MESSAGE(loadPrefab(refreshRoot / "cart.prefab", previous).success, "the parity cart should load");
    std::filesystem::copy_file(applyRoot / "cart.prefab", refreshRoot / "cart.prefab",
                               std::filesystem::copy_options::overwrite_existing);
    PrefabAuthoring refreshAuthoring(assets, SceneDocumentOptions{.assetRoot = refreshRoot});
    CHECK_MESSAGE(refreshAuthoring.refreshInstances(refreshed, "cart.prefab", &previous).success,
                  "the parity refresh should succeed");

    const SceneDocumentOptions options{.assetRoot = applyRoot};
    const Entity appliedOther = applied.findEntity(appliedSecond);
    const Entity refreshedOther = refreshed.findEntity(refreshedSecond);
    CHECK_MESSAGE(
        (appliedOther != entt::null && refreshedOther != entt::null &&
         childNamed(applied, appliedOther, "Seat") != entt::null &&
         childNamed(refreshed, refreshedOther, "Seat") != entt::null &&
         childNamed(refreshed, refreshedOther, "Axle") == entt::null &&
         refreshed.registry().get<PointLightComponent>(childNamed(refreshed, refreshedOther, "Wheel")).intensity ==
             8.0f),
        "refreshInstances should update the other instance as apply does");
    std::vector<PrefabOverride> appliedOverrides;
    std::vector<PrefabOverride> refreshedOverrides;
    CHECK_MESSAGE((PrefabSystem(assets, options).overrides(applied, appliedOther, appliedOverrides).success &&
                   PrefabSystem(assets, SceneDocumentOptions{.assetRoot = refreshRoot})
                       .overrides(refreshed, refreshedOther, refreshedOverrides)
                       .success &&
                   appliedOverrides == refreshedOverrides && refreshedOverrides.empty()),
                  "both paths should leave the other instance matching the prefab");
}

TEST_CASE("PrefabWrite: save reports through hooks") {
    const testsupport::TempDirectory directory("prefab-write-save");
    const std::filesystem::path& root = directory.path();
    AssetManager assets;
    const SceneDocumentOptions options{.assetRoot = root};
    PrefabAuthoring authoring(assets, options);
    Scene authored;
    const Entity lamp = authored.createEntity("Lamp");
    authored.registry().emplace<PointLightComponent>(lamp);
    CHECK_MESSAGE(authoring.create(authored, lamp, "lamp.prefab").success, "the lamp should be created");

    PrefabSystem prefabs(assets, options);
    Scene document;
    OpenedPrefabDocument opened;
    CHECK_MESSAGE(openPrefabDocument("lamp.prefab", document, prefabs, opened).success, "the lamp should open");
    document.registry().get<PointLightComponent>(document.findEntity(opened.root)).intensity = 2.0f;
    WriteRecorder recorder;
    SavedPrefabDocument saved;
    CHECK_MESSAGE((savePrefabDocument(document, opened.root, "lamp.prefab", prefabs, saved, recorder.hooks()).success &&
                   recorder.writes.size() == 1 && recorder.ledger.matches(root / "lamp.prefab", saved.after)),
                  "saving a prefab document should report the write");

    // A step built from the save restores the file on undo.
    PrefabWriteCommand step("Edit Lamp", {.file = root / "lamp.prefab", .before = saved.before, .after = saved.after},
                            {}, recorder.hooks());
    Scene level;
    EditorCommandContext context{level, assets, options};
    CHECK_MESSAGE((step.execute(context).success && recorder.writes.size() == 2),
                  "a step's first execute should write nothing new but still report the content");
    CHECK_MESSAGE((step.undo(context).success && readText(root / "lamp.prefab") == opened.fileBytes &&
                   recorder.ledger.matches(root / "lamp.prefab", opened.fileBytes)),
                  "undoing the save step should restore the file");
}

namespace {

// lamp.prefab (a lit Lamp with a Shade) and shelf.prefab (a Shelf holding one
// lamp) under root.
void authorLampAndShelf(const std::filesystem::path& root) {
    AssetManager assets;
    PrefabAuthoring authoring(assets, SceneDocumentOptions{.assetRoot = root});
    Scene staging;
    const Entity lamp = staging.createEntity("Lamp");
    staging.registry().emplace<PointLightComponent>(lamp, glm::vec3(1.0f), 10.0f, 4.0f);
    staging.setParent(staging.createEntity("Shade"), lamp);
    CHECK_MESSAGE(authoring.create(staging, lamp, "lamp.prefab").success, "the lamp prefab should be created");
    Scene shelfStaging;
    const Entity shelf = shelfStaging.createEntity("Shelf");
    Entity nested = entt::null;
    CHECK_MESSAGE((authoring.instantiate(shelfStaging, "lamp.prefab", shelf, nested).success &&
                   authoring.create(shelfStaging, shelf, "shelf.prefab").success),
                  "the shelf prefab should be created");
}

} // namespace

// A prefab file the editor never wrote: the ledger learns its content when a
// document with its instances opens or an instance is placed, so an outside
// change keeps the instances' live overrides.
TEST_CASE("PrefabWrite: ledger seeding") {
    Fixture fixture("prefab-write-seeding");
    authorLampAndShelf(fixture.root);
    PrefabWriteLedger& ledger = fixture.recorder.ledger;
    const std::filesystem::path file = (fixture.root / "lamp.prefab").lexically_normal();
    CHECK_MESSAGE(fixture.invoke("prefab.instantiate", R"({"asset":"shelf.prefab"})").status ==
                      OperatorStatus::Finished,
                  "seeding: the shelf should instantiate");
    CHECK_MESSAGE((ledger.recorded(file) != nullptr && ledger.recorded(fixture.root / "shelf.prefab") != nullptr),
                  "seeding: placing an instance notes its file and its nested prefabs' files");

    // A level saved earlier and opened now: nothing has been noted yet.
    PrefabAuthoring authoring(fixture.assets, fixture.document.options());
    Scene level;
    Entity first = entt::null;
    Entity second = entt::null;
    CHECK_MESSAGE((authoring.instantiate(level, "lamp.prefab", entt::null, first).success &&
                   authoring.instantiate(level, "lamp.prefab", entt::null, second).success),
                  "seeding: the level's lamps should instantiate");
    // An override edited since the last prefab operation, so not persisted.
    level.registry().get<PointLightComponent>(second).range = 9.0f;
    Scene unseededLevel;
    std::string text;
    CHECK_MESSAGE((SceneDocument::serialize(level, fixture.assets, fixture.document.options(), text).success &&
                   SceneDocument::deserialize(text, fixture.assets, fixture.document.options(), unseededLevel).success),
                  "seeding: the level should copy");

    PrefabWriteLedger seeded;
    notePrefabFilesRead(
        level, fixture.root,
        {.fileRead = [&seeded](const std::filesystem::path& path, const std::optional<std::string>& bytes) {
            seeded.record(path, bytes);
        }});
    CHECK_MESSAGE(seeded.matches(file, readText(file)), "seeding: opening a document notes its prefabs' content");

    PrefabDocument document;
    CHECK_MESSAGE(loadPrefab(file, document).success, "seeding: the lamp should load");
    document.entities.front().name = "Renamed lamp";
    CHECK_MESSAGE(savePrefab(file, document).success, "seeding: the outside edit should save");

    PrefabFileChange change;
    CHECK_MESSAGE((classifyPrefabFileChange(seeded, file, fixture.root, change).success && change.previous.has_value()),
                  "seeding: the outside change knows the content before it");
    CHECK_MESSAGE((authoring.refreshInstances(level, change.reference, &*change.previous).success &&
                   level.registry().get<NameComponent>(second).name == "Renamed lamp" &&
                   level.registry().get<PointLightComponent>(second).range == 9.0f),
                  "seeding: the refresh keeps the override edited since the last prefab operation");

    // Without the seed the same refresh falls back to persisted overrides.
    CHECK_MESSAGE(authoring.refreshInstances(unseededLevel, "lamp.prefab", nullptr).success,
                  "seeding: refresh unseeded");
    float lastRange = 0.0f;
    for (const auto [entity, instance] : unseededLevel.registry().view<const PrefabInstanceComponent>().each()) {
        if (instance.prefab == "lamp.prefab") {
            lastRange = unseededLevel.registry().get<PointLightComponent>(entity).range;
        }
    }
    CHECK_MESSAGE(lastRange == 4.0f,
                  "seeding: without the previous content the live override is lost (what seeding prevents)");
}

TEST_SUITE_END();
