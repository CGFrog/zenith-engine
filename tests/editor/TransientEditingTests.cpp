// Transient (generated) entities in the editor: left out of undo capture,
// copies, saves, and Play's snapshot, survive undo and redo around them, and
// are marked in the outliner.

#include "EditorTestFixture.hpp"

#include "editor/document/SceneFragment.hpp"
#include "editor/panels/outliner/OutlinerTree.hpp"
#include "engine/scene/SceneDocument.hpp"

#include "support/Doctest.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

// A transient preview root with one child under parent (the scene root when
// null), as a generator would build it.
Entity buildPreview(Scene& scene, Entity parent = entt::null) {
    const Entity root = scene.createEntity("Preview");
    scene.markTransient(root);
    if (parent != entt::null) {
        scene.setParent(root, parent);
    }
    scene.setParent(scene.createEntity("Preview Room"), root);
    return root;
}

const OutlinerNode* findRow(std::span<const OutlinerNode> rows, SceneEntityId id) {
    for (const OutlinerNode& row : rows) {
        if (row.kind == OutlinerNodeKind::Entity && row.entity == id) {
            return &row;
        }
        if (const OutlinerNode* found = findRow(row.children, id)) {
            return found;
        }
    }
    return nullptr;
}

} // namespace

TEST_SUITE_BEGIN("TransientEditing");

// Edits that create or capture transient entities record only saved ones,
// and undo and redo leave the preview alone, even under a removed parent.
TEST_CASE("TransientEditing: undo") {
    EditorTestFixture fixture;
    Scene& scene = fixture.scene;
    const Entity level = scene.createEntity("Level");
    const SceneEntityId levelId = scene.id(level);
    const size_t historyBefore = fixture.history.size();

    fixture.editing().perform("Generate", {}, [&scene] {
        buildPreview(scene);
        return SceneResult{.success = true};
    });
    CHECK_MESSAGE(fixture.history.size() == historyBefore, "an edit that only builds a preview records no undo step");

    SceneEditing::EntitySnapshot snapshot;
    const Entity preview = buildPreview(scene);
    CHECK_MESSAGE(fixture.editing().captureEntities(std::span(&preview, 1), snapshot), "a transient entity captures");
    scene.registry().get<NameComponent>(preview).name = "Renamed Preview";
    fixture.editing().commit("Rename Preview", {}, std::move(snapshot));
    CHECK_MESSAGE((fixture.history.size() == historyBefore && fixture.diagnostics.entries().empty()),
                  "a captured transient entity's change records nothing");

    fixture.editing().perform("Rename Level", {}, [&scene, level] {
        scene.registry().get<NameComponent>(level).name = "Level 1";
        buildPreview(scene, level);
        return SceneResult{.success = true};
    });
    CHECK_MESSAGE(fixture.history.size() == historyBefore + 1, "a mixed edit records its saved change");
    const size_t previews = scene.transientEntities().size();
    fixture.editing().undo();
    CHECK_MESSAGE((scene.registry().get<NameComponent>(scene.findEntity(levelId)).name == "Level" &&
                   scene.transientEntities().size() == previews),
                  "undo reverts the saved change and keeps the preview");

    // Delete the level, undo, generate under it again, and redo the delete:
    // the redo removes the level with the new preview under it.
    fixture.editing().perform("Delete Level", {}, [&scene, levelId] {
        scene.destroyEntity(scene.findEntity(levelId));
        return SceneResult{.success = true};
    });
    fixture.editing().undo();
    buildPreview(scene, scene.findEntity(levelId));
    fixture.editing().redo();
    CHECK_MESSAGE((scene.findEntity(levelId) == entt::null && fixture.diagnostics.entries().empty()),
                  "redoing a removal takes the transient children along");
    fixture.editing().undo();
    CHECK_MESSAGE((scene.findEntity(levelId) != entt::null), "undo brings the level back without its preview");
    scene.destroyTransient();
}

TEST_CASE("TransientEditing: copy paste and outliner") {
    EditorTestFixture fixture;
    Scene& scene = fixture.scene;
    const Entity preview = buildPreview(scene);
    const SceneEntityId previewId = scene.id(preview);
    const SceneEntityId roomId = scene.id(scene.registry().get<RelationshipComponent>(preview).children.front());

    const std::vector<OutlinerNode> rows = buildOutlinerTree(scene, {});
    const OutlinerNode* previewRow = findRow(rows, previewId);
    const OutlinerNode* roomRow = findRow(rows, roomId);
    const Entity saved = scene.createEntity("Saved");
    const std::vector<OutlinerNode> rowsWithSaved = buildOutlinerTree(scene, {});
    const OutlinerNode* savedRow = findRow(rowsWithSaved, scene.id(saved));
    CHECK_MESSAGE((previewRow != nullptr && previewRow->transient && roomRow != nullptr && roomRow->transient),
                  "the outliner marks generated rows");
    CHECK_MESSAGE((savedRow != nullptr && !savedRow->transient), "saved rows are not marked");

    std::string fragment;
    const std::vector<SceneEntityId> roots{previewId, scene.id(saved)};
    CHECK_MESSAGE((SceneFragment::copy(scene, fixture.assets, fixture.document.options(), roots, fragment).success &&
                   fragment.find("Preview") == std::string::npos && fragment.find("Saved") != std::string::npos),
                  "copying skips transient entities");
    std::vector<SceneEntityId> pasted;
    CHECK_MESSAGE(
        (SceneFragment::paste(fragment, scene, fixture.assets, fixture.document.options(), preview, pasted).success &&
         pasted.size() == 1),
        "pasting under a transient parent succeeds");
    const Entity copy = pasted.empty() ? Entity{entt::null} : scene.findEntity(pasted.front());
    CHECK_MESSAGE((copy != entt::null && scene.registry().get<RelationshipComponent>(copy).parent == entt::null &&
                   !scene.isTransient(copy)),
                  "it pastes at the top level, where the copy is saved");
    CHECK_MESSAGE((scene.findEntity(previewId) == preview && scene.isTransient(preview)),
                  "paste creates the copies in place, so the preview stays as it was");
}

TEST_CASE("TransientEditing: save and play") {
    EditorTestFixture fixture;
    Scene& scene = fixture.scene;
    const Entity preview = buildPreview(scene);
    const SceneEntityId previewId = scene.id(preview);

    const std::filesystem::path path = fixture.scratch / "zenith_transient_editing.zscene";
    const SceneDocumentResult saved = fixture.document.saveAs(path, scene, fixture.assets);
    std::string text;
    SceneDocument::serialize(scene, fixture.assets, fixture.document.options(), text);
    CHECK_MESSAGE((saved.success && text.find("Preview") == std::string::npos), "saving leaves the preview out");

    EditorPlayMode play;
    CHECK_MESSAGE(play.enter(scene, fixture.assets, fixture.document.options(), {}, EditorRuntime::Play).success,
                  "Play starts");
    CHECK_MESSAGE((scene.findEntity(previewId) == entt::null && scene.transientEntities().empty()),
                  "the preview does not enter Play");
    std::vector<SceneEntityId> selection;
    play.stop(scene, selection);
    CHECK_MESSAGE((scene.findEntity(previewId) != entt::null && scene.isTransient(scene.findEntity(previewId))),
                  "the preview is back after Stop");
    scene.destroyTransient();
}

TEST_SUITE_END();
