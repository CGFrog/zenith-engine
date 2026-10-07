#include "editor/document/EditorDocument.hpp"

#include "engine/assets/Assets.hpp"

#include "support/Doctest.hpp"

#include <filesystem>

TEST_SUITE_BEGIN("EditorDocument");

// The checks build on each other: Save needs the path Save As established,
// and the failed Open and New check that state is preserved or reset.
TEST_CASE("EditorDocument: save and open and new keep the path and dirty state and scene consistent") {
    const testsupport::TempDirectory directory("editor-document");

    AssetManager assets;
    EditorDocument document(directory.path());
    Scene scene;
    const Entity entity = scene.createEntity("Original");
    const SceneEntityId originalId = scene.id(entity);
    document.markDirty();
    CHECK_MESSAGE((document.dirty() && document.displayName() == "Untitled*"),
                  "edits should mark an untitled document dirty");
    CHECK_MESSAGE(!document.save(scene, assets).success, "untitled document should require Save As");

    const std::filesystem::path path = directory / "test.zenith-scene";
    CHECK_MESSAGE(document.saveAs(path, scene, assets).success, "Save As should write the document");
    CHECK_MESSAGE((!document.dirty() && document.path() == path), "successful Save As should establish a clean path");

    scene.registry().get<NameComponent>(entity).name = "Changed";
    document.markDirty();
    CHECK_MESSAGE(document.save(scene, assets).success, "Save should use the established path");
    CHECK_MESSAGE(!document.dirty(), "successful Save should clear dirty state");

    const std::filesystem::path missing = directory / "missing.zenith-scene";
    document.markDirty();
    const SceneDocumentResult failedOpen = document.open(missing, scene, assets);
    CHECK_MESSAGE(!failedOpen.success, "opening a missing document should fail");
    CHECK_MESSAGE((document.path() == path && document.dirty()), "failed Open should preserve path and dirty state");
    CHECK_MESSAGE((scene.findEntity(originalId) != entt::null), "failed Open should preserve the scene");

    document.createNew(scene);
    CHECK_MESSAGE(scene.registry().view<SceneEntityIdComponent>().empty(), "New should clear the runtime scene");
    CHECK_MESSAGE((!document.path() && !document.dirty()), "New should reset path and dirty state");
    CHECK_MESSAGE(document.open(path, scene, assets).success, "Open should load a saved document");
    CHECK_MESSAGE(scene.registry().get<NameComponent>(scene.findEntity(originalId)).name == "Changed",
                  "Open should restore saved component data");
}

TEST_SUITE_END();
