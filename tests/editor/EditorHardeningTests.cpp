#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/EditorRecovery.hpp"
#include "editor/document/SceneFragment.hpp"
#include "engine/assets/Assets.hpp"

#include <filesystem>
#include <iostream>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}
} // namespace

int main() {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "zenith_hardening_tests";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    const SceneDocumentOptions options{.assetRoot = directory};
    AssetManager assets;

    Scene scene;
    const Entity root = scene.createEntity("Root");
    const Entity child = scene.createEntity("Child");
    scene.setParent(child, root);
    std::string fragment;
    const SceneEntityId rootId = scene.id(root);
    expect(SceneFragment::copy(scene, assets, options, std::span(&rootId, 1), fragment).success,
           "a hierarchy should serialize to a fragment");
    std::vector<SceneEntityId> pasted;
    expect(SceneFragment::paste(fragment, scene, assets, options, entt::null, pasted).success,
           "a fragment should paste atomically");
    expect(pasted.size() == 1 && pasted.front() != rootId, "pasted roots should receive fresh stable IDs");
    const Entity pastedRoot = scene.findEntity(pasted.front());
    expect(scene.registry().get<RelationshipComponent>(pastedRoot).children.size() == 1,
           "pasting should retain fragment hierarchy");

    // Collections travel by name: same scene keeps them, another scene maps
    // them to its collection of that name, and drops unknown ones.
    {
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
        expect(SceneFragment::copy(source, assets, options, copied, collectionFragment).success,
               "entities in collections should copy");

        Scene destination;
        SceneCollections destinationCollections;
        SceneCollectionId disabled;
        SceneCollectionId destinationProps;
        destinationCollections.create("Disabled", {}, disabled);
        destinationCollections.create("Props", {}, destinationProps);
        destination.setCollections(destinationCollections);
        std::vector<SceneEntityId> pastedIds;
        expect(SceneFragment::paste(collectionFragment, destination, assets, options, entt::null, pastedIds).success &&
                   pastedIds.size() == 2,
               "a fragment from another scene should paste");
        const Entity pastedCrate = destination.findEntity(pastedIds.front());
        const Entity pastedInstance = destination.findEntity(pastedIds.back());
        expect(destination.registry().get<CollectionMembershipComponent>(pastedCrate).collections ==
                       std::vector<SceneCollectionId>{destinationProps} &&
                   destination.registry().get<CollectionInstanceComponent>(pastedInstance).collection ==
                       destinationProps,
               "pasted collection references should follow collection names, not ids");

        std::vector<SceneEntityId> again;
        expect(SceneFragment::paste(collectionFragment, source, assets, options, entt::null, again).success &&
                   source.registry().get<CollectionMembershipComponent>(source.findEntity(again.front())).collections ==
                       std::vector<SceneCollectionId>{props, lights},
               "pasting into the source scene should keep its collections");
    }

    EditorRecovery recovery(directory / "recovery.zenith-scene");
    expect(recovery.autosave(scene, assets, options).success && recovery.available(),
           "autosave should create a separate recovery document");
    scene.clear();
    expect(recovery.recover(scene, assets, options).success && scene.findEntity(pasted.front()) != entt::null,
           "recovery should restore autosaved work");
    std::string discardError;
    expect(recovery.discard(discardError) && !recovery.available(), "discard should remove only the recovery file");

    EditorDiagnostics diagnostics;
    const uint64_t id = diagnostics.add(EditorDiagnosticSeverity::Error, "Document", "Save failed", "Save As");
    expect(diagnostics.entries().size() == 1 && diagnostics.entries().front().action == "Save As",
           "diagnostics should retain severity, source, and action");
    diagnostics.dismiss(id);
    expect(diagnostics.entries().empty(), "diagnostics should be individually dismissible");

    std::filesystem::remove_all(directory, error);
    return failures == 0 ? 0 : 1;
}
