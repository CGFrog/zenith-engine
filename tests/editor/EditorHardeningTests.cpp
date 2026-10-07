#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/EditorRecovery.hpp"
#include "editor/document/SceneFragment.hpp"
#include "engine/assets/Assets.hpp"

#include "support/Doctest.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

TEST_SUITE_BEGIN("EditorHardening");

// Recovery autosaves the scene the paste built and checks that the pasted
// root comes back, so both stay in one case.
TEST_CASE("EditorHardening: a pasted hierarchy keeps its shape and recovery restores it") {
    const testsupport::TempDirectory directory("editor-hardening-recovery");
    const SceneDocumentOptions options{.assetRoot = directory.path()};
    AssetManager assets;

    Scene scene;
    const Entity root = scene.createEntity("Root");
    const Entity child = scene.createEntity("Child");
    scene.setParent(child, root);
    std::string fragment;
    const SceneEntityId rootId = scene.id(root);
    CHECK_MESSAGE(SceneFragment::copy(scene, assets, options, std::span(&rootId, 1), fragment).success,
                  "a hierarchy should serialize to a fragment");
    std::vector<SceneEntityId> pasted;
    CHECK_MESSAGE(SceneFragment::paste(fragment, scene, assets, options, entt::null, pasted).success,
                  "a fragment should paste atomically");
    CHECK_MESSAGE((pasted.size() == 1 && pasted.front() != rootId), "pasted roots should receive fresh stable IDs");
    const Entity pastedRoot = scene.findEntity(pasted.front());
    CHECK_MESSAGE(scene.registry().get<RelationshipComponent>(pastedRoot).children.size() == 1,
                  "pasting should retain fragment hierarchy");

    EditorRecovery recovery(directory / "recovery.zenith-scene");
    CHECK_MESSAGE((recovery.autosave(scene, assets, options).success && recovery.available()),
                  "autosave should create a separate recovery document");
    scene.clear();
    CHECK_MESSAGE((recovery.recover(scene, assets, options).success && scene.findEntity(pasted.front()) != entt::null),
                  "recovery should restore autosaved work");
    std::string discardError;
    CHECK_MESSAGE((recovery.discard(discardError) && !recovery.available()),
                  "discard should remove only the recovery file");
}

// Collections travel by name: same scene keeps them, another scene maps
// them to its collection of that name, and drops unknown ones.
TEST_CASE("EditorHardening: pasted collection references follow collection names") {
    const testsupport::TempDirectory directory("editor-hardening-collections");
    const SceneDocumentOptions options{.assetRoot = directory.path()};
    AssetManager assets;

    Scene source;
    SceneCollections sourceCollections;
    SceneCollectionId props;
    SceneCollectionId lights;
    sourceCollections.create("Props", {}, props);
    sourceCollections.create("Lights", {}, lights);
    source.setCollections(sourceCollections);
    const Entity crate = source.createEntity("Crate");
    linkToCollection(source, crate, props);
    linkToCollection(source, crate, lights);
    const Entity instance = source.createEntity("Instance");
    source.registry().emplace<CollectionInstanceComponent>(instance, props);
    const std::vector<SceneEntityId> copied{source.id(crate), source.id(instance)};
    std::string collectionFragment;
    CHECK_MESSAGE(SceneFragment::copy(source, assets, options, copied, collectionFragment).success,
                  "entities in collections should copy");

    Scene destination;
    SceneCollections destinationCollections;
    SceneCollectionId disabled;
    SceneCollectionId destinationProps;
    destinationCollections.create("Disabled", {}, disabled);
    destinationCollections.create("Props", {}, destinationProps);
    destination.setCollections(destinationCollections);
    std::vector<SceneEntityId> pastedIds;
    CHECK_MESSAGE(
        (SceneFragment::paste(collectionFragment, destination, assets, options, entt::null, pastedIds).success &&
         pastedIds.size() == 2),
        "a fragment from another scene should paste");
    const Entity pastedCrate = destination.findEntity(pastedIds.front());
    const Entity pastedInstance = destination.findEntity(pastedIds.back());
    CHECK_MESSAGE(
        (destination.registry().get<CollectionMembershipComponent>(pastedCrate).collections ==
             std::vector<SceneCollectionId>{destinationProps} &&
         destination.registry().get<CollectionInstanceComponent>(pastedInstance).collection == destinationProps),
        "pasted collection references should follow collection names, not ids");

    std::vector<SceneEntityId> again;
    CHECK_MESSAGE((SceneFragment::paste(collectionFragment, source, assets, options, entt::null, again).success &&
                   source.registry().get<CollectionMembershipComponent>(source.findEntity(again.front())).collections ==
                       std::vector<SceneCollectionId>{props, lights}),
                  "pasting into the source scene should keep its collections");
}

TEST_CASE("EditorHardening: diagnostics keep their details and are individually dismissible") {
    EditorDiagnostics diagnostics;
    const uint64_t id = diagnostics.add(EditorDiagnosticSeverity::Error, "Document", "Save failed", "Save As");
    CHECK_MESSAGE((diagnostics.entries().size() == 1 && diagnostics.entries().front().action == "Save As"),
                  "diagnostics should retain severity, source, and action");
    diagnostics.dismiss(id);
    CHECK_MESSAGE(diagnostics.entries().empty(), "diagnostics should be individually dismissible");
}

TEST_SUITE_END();
