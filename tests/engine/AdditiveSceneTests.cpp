// AdditiveSceneManager's public contract: broken cross-scene references give
// a diagnostic rather than a crash, and additive loads and unloads (in any
// order, of the same file more than once) leave the application's main scene
// and the other instances' entities and asset ownership intact.

#include "engine/assets/Assets.hpp"
#include "engine/jobs/JobSystem.hpp"
#include "engine/scene/AdditiveSceneManager.hpp"
#include "engine/scene/Scene.hpp"
#include "engine/scene/SceneDocument.hpp"

#include "support/Doctest.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace {

const std::filesystem::path sampleModelPath = std::filesystem::path{ZENITH_ASSET_DIR} / "sample.gltf";

size_t sceneEntityCount(const Scene& scene) {
    return scene.registry().view<const SceneEntityIdComponent>().size();
}

// A deterministic job system, an asset manager, and the manager under test,
// declared so the manager is destroyed before what it references.
struct AdditiveSceneFixture {
    testsupport::TempDirectory root{"additive-scenes"};
    JobSystem jobs{{.queueCapacity = 16, .deterministic = true}};
    AssetManager assets;
    SceneDocumentOptions options{.assetRoot = ZENITH_ASSET_DIR};
    AdditiveSceneManager layers{jobs, assets, options};

    // Saves a scene with one named root entity (holding the sample model when
    // withModel) and returns that root's stable id.
    SceneEntityId writeLayer(const std::filesystem::path& path, const std::string& rootName, bool withModel) {
        Scene authored;
        const Entity layerRoot = authored.createEntity(rootName);
        if (withModel) {
            const AssetLoadResult<ModelHandle> model = assets.loadModel(sampleModelPath);
            REQUIRE(static_cast<bool>(model));
            REQUIRE(authored.instantiateModel(layerRoot, model.handle, assets).success);
        }
        const SceneDocumentResult saved = SceneDocument::save(path, authored, assets, options);
        REQUIRE_MESSAGE(saved.success, saved.error);
        const SceneEntityId id = authored.id(layerRoot);
        authored.clear();
        assets.collectUnused(true);
        return id;
    }

    // Runs every queued load and lets the manager commit the results.
    void finishLoads() {
        while (jobs.pumpOne()) {
        }
        layers.update();
    }

    AdditiveSceneStatus status(SceneInstanceId id) const {
        const std::optional<AdditiveSceneState> state = layers.state(id);
        REQUIRE(state.has_value());
        return state->status;
    }
};

} // namespace

TEST_SUITE_BEGIN("AdditiveScene");

TEST_CASE("AdditiveScene: a reference to an unknown instance reports an error") {
    AdditiveSceneFixture fixture;

    for (const SceneInstanceId instance : {SceneInstanceId{}, SceneInstanceId{12345}}) {
        CAPTURE(instance.value);
        const SceneInstanceEntityResult resolved = fixture.layers.resolve({instance, SceneEntityId{1}});
        CHECK_FALSE(static_cast<bool>(resolved));
        CHECK(resolved.scene == nullptr);
        CHECK((resolved.entity == entt::null));
        CHECK_FALSE(resolved.error.empty());
    }
}

TEST_CASE("AdditiveScene: a reference to a missing entity in a committed instance reports an error") {
    AdditiveSceneFixture fixture;
    const SceneEntityId layerRoot = fixture.writeLayer(fixture.root / "layer.zenith-scene", "Layer root", false);
    const SceneInstanceId instance = fixture.layers.requestLoad(fixture.root / "layer.zenith-scene");
    fixture.finishLoads();
    REQUIRE(fixture.status(instance) == AdditiveSceneStatus::Committed);

    CHECK(static_cast<bool>(fixture.layers.resolve({instance, layerRoot})));
    const SceneInstanceEntityResult missing = fixture.layers.resolve({instance, SceneEntityId{layerRoot.value + 1000}});
    CHECK_FALSE(static_cast<bool>(missing));
    CHECK(missing.scene == nullptr);
    CHECK_FALSE(missing.error.empty());
}

TEST_CASE("AdditiveScene: a reference to an entity destroyed after commit stops resolving") {
    AdditiveSceneFixture fixture;
    const SceneEntityId layerRoot = fixture.writeLayer(fixture.root / "layer.zenith-scene", "Layer root", false);
    const SceneInstanceId instance = fixture.layers.requestLoad(fixture.root / "layer.zenith-scene");
    fixture.finishLoads();
    Scene* layer = fixture.layers.scene(instance);
    REQUIRE(layer != nullptr);

    layer->destroyEntity(layer->findEntity(layerRoot));

    const SceneInstanceEntityResult resolved = fixture.layers.resolve({instance, layerRoot});
    CHECK_FALSE(static_cast<bool>(resolved));
    CHECK_FALSE(resolved.error.empty());
}

TEST_CASE("AdditiveScene: a reference to a loading or failed instance reports an error") {
    AdditiveSceneFixture fixture;
    const SceneEntityId layerRoot = fixture.writeLayer(fixture.root / "layer.zenith-scene", "Layer root", false);
    const SceneInstanceId loading = fixture.layers.requestLoad(fixture.root / "layer.zenith-scene");
    const SceneInstanceId failed = fixture.layers.requestLoad(fixture.root / "missing.zenith-scene");

    REQUIRE(fixture.status(loading) == AdditiveSceneStatus::Loading);
    CHECK_FALSE(static_cast<bool>(fixture.layers.resolve({loading, layerRoot})));

    fixture.finishLoads();
    REQUIRE(fixture.status(failed) == AdditiveSceneStatus::Failed);
    const SceneInstanceEntityResult resolved = fixture.layers.resolve({failed, layerRoot});
    CHECK_FALSE(static_cast<bool>(resolved));
    CHECK_FALSE(resolved.error.empty());
}

TEST_CASE("AdditiveScene: a missing scene file fails with a diagnostic and commits nothing") {
    AdditiveSceneFixture fixture;
    const SceneInstanceId instance = fixture.layers.requestLoad(fixture.root / "missing.zenith-scene");
    fixture.finishLoads();

    const std::optional<AdditiveSceneState> state = fixture.layers.state(instance);
    REQUIRE(state.has_value());
    CHECK(state->status == AdditiveSceneStatus::Failed);
    CHECK_MESSAGE(state->error.find("missing.zenith-scene") != std::string::npos, "the error names the file");
    CHECK(fixture.layers.scene(instance) == nullptr);
    CHECK(fixture.layers.committedInstances().empty());
}

TEST_CASE("AdditiveScene: a scene that references a missing asset fails without publishing a partial scene") {
    AdditiveSceneFixture fixture;
    const std::filesystem::path broken = fixture.root / "broken-asset.zenith-scene";
    testsupport::writeText(broken, R"({
  "version": 1,
  "entities": [{
    "id": 1,
    "name": "Missing asset",
    "parent": null,
    "local_transform": [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1],
    "components": {"model": {"path": "missing-model.gltf"}}
  }]
})");

    const SceneInstanceId instance = fixture.layers.requestLoad(broken);
    fixture.finishLoads();

    const std::optional<AdditiveSceneState> state = fixture.layers.state(instance);
    REQUIRE(state.has_value());
    CHECK(state->status == AdditiveSceneStatus::Failed);
    CHECK_FALSE(state->error.empty());
    CHECK(fixture.layers.scene(instance) == nullptr);
    CHECK(fixture.layers.committedInstances().empty());
    CHECK_FALSE(static_cast<bool>(fixture.layers.resolve({instance, SceneEntityId{1}})));
}

TEST_CASE("AdditiveScene: unloading an instance leaves the main scene's entities and assets intact") {
    AdditiveSceneFixture fixture;
    fixture.writeLayer(fixture.root / "layer.zenith-scene", "Layer root", true);

    // The application's main scene shares the layer's model and leases its
    // own assets, as an application does for the scene it renders.
    Scene mainScene;
    const AssetLoadResult<ModelHandle> model = fixture.assets.loadModel(sampleModelPath);
    REQUIRE(static_cast<bool>(model));
    const Entity mainRoot = mainScene.createEntity("Main root");
    REQUIRE(mainScene.instantiateModel(mainRoot, model.handle, fixture.assets).success);
    const SceneEntityId mainRootId = mainScene.id(mainRoot);
    const size_t mainEntities = sceneEntityCount(mainScene);
    AssetLeaseSet mainLeases = fixture.assets.leaseSceneAssets(mainScene);

    const SceneInstanceId layer = fixture.layers.requestLoad(fixture.root / "layer.zenith-scene");
    fixture.finishLoads();
    REQUIRE(fixture.status(layer) == AdditiveSceneStatus::Committed);
    REQUIRE(fixture.layers.unload(layer));
    fixture.assets.collectUnused(true);

    CHECK(sceneEntityCount(mainScene) == mainEntities);
    CHECK(mainScene.findEntity(mainRootId) == mainRoot);
    CHECK_MESSAGE(fixture.assets.model(model.handle) != nullptr, "the main scene's lease keeps the shared model");

    mainLeases = {};
    fixture.assets.collectUnused(true);
    CHECK_MESSAGE(fixture.assets.model(model.handle) == nullptr, "the unloaded layer no longer holds the model");
}

TEST_CASE("AdditiveScene: loading the same scene twice gives independent instances") {
    AdditiveSceneFixture fixture;
    const std::filesystem::path path = fixture.root / "layer.zenith-scene";
    const SceneEntityId layerRoot = fixture.writeLayer(path, "Layer root", true);

    const SceneInstanceId first = fixture.layers.requestLoad(path);
    const SceneInstanceId second = fixture.layers.requestLoad(path);
    fixture.finishLoads();

    REQUIRE(first != second);
    REQUIRE(fixture.status(first) == AdditiveSceneStatus::Committed);
    REQUIRE(fixture.status(second) == AdditiveSceneStatus::Committed);
    CHECK(fixture.layers.scene(first) != fixture.layers.scene(second));
    CHECK(fixture.layers.committedInstances() == std::vector<SceneInstanceId>{first, second});

    const SceneInstanceEntityResult inFirst = fixture.layers.resolve({first, layerRoot});
    const SceneInstanceEntityResult inSecond = fixture.layers.resolve({second, layerRoot});
    REQUIRE(static_cast<bool>(inFirst));
    REQUIRE(static_cast<bool>(inSecond));
    CHECK_MESSAGE(inFirst.scene != inSecond.scene, "the same stable id resolves in each instance's own scene");

    const auto models = fixture.layers.scene(second)->registry().view<const ModelComponent>();
    REQUIRE_FALSE(models.empty());
    const ModelHandle secondModel = models.get<const ModelComponent>(*models.begin()).model;

    REQUIRE(fixture.layers.unload(first));
    fixture.assets.collectUnused(true);
    CHECK(fixture.status(second) == AdditiveSceneStatus::Committed);
    CHECK(static_cast<bool>(fixture.layers.resolve({second, layerRoot})));
    CHECK_MESSAGE(fixture.assets.model(secondModel) != nullptr, "the remaining instance keeps its own lease");
}

TEST_CASE("AdditiveScene: unloading in any order keeps the remaining instances in load order") {
    AdditiveSceneFixture fixture;
    fixture.writeLayer(fixture.root / "a.zenith-scene", "A", false);
    fixture.writeLayer(fixture.root / "b.zenith-scene", "B", false);
    fixture.writeLayer(fixture.root / "c.zenith-scene", "C", false);
    const SceneInstanceId a = fixture.layers.requestLoad(fixture.root / "a.zenith-scene");
    const SceneInstanceId b = fixture.layers.requestLoad(fixture.root / "b.zenith-scene");
    const SceneInstanceId c = fixture.layers.requestLoad(fixture.root / "c.zenith-scene");
    fixture.finishLoads();
    REQUIRE(fixture.layers.committedInstances() == std::vector<SceneInstanceId>{a, b, c});

    CHECK(fixture.layers.unload(b));
    CHECK(fixture.layers.committedInstances() == std::vector<SceneInstanceId>{a, c});
    CHECK(fixture.layers.unload(a));
    CHECK(fixture.layers.committedInstances() == std::vector<SceneInstanceId>{c});
    CHECK(fixture.layers.scene(c) != nullptr);

    CHECK_FALSE_MESSAGE(fixture.layers.unload(a), "unloading an instance twice reports that it is gone");
    CHECK(fixture.layers.unload(c));
    CHECK(fixture.layers.committedInstances().empty());
}

TEST_CASE("AdditiveScene: a reference into an unloaded instance stops resolving and its id is not reused") {
    AdditiveSceneFixture fixture;
    const std::filesystem::path path = fixture.root / "layer.zenith-scene";
    const SceneEntityId layerRoot = fixture.writeLayer(path, "Layer root", false);
    const SceneInstanceId unloaded = fixture.layers.requestLoad(path);
    fixture.finishLoads();
    const SceneInstanceEntityReference reference{unloaded, layerRoot};
    REQUIRE(static_cast<bool>(fixture.layers.resolve(reference)));

    REQUIRE(fixture.layers.unload(unloaded));
    const SceneInstanceId reloaded = fixture.layers.requestLoad(path);
    fixture.finishLoads();

    CHECK(reloaded != unloaded);
    CHECK(fixture.layers.scene(unloaded) == nullptr);
    CHECK_FALSE(fixture.layers.state(unloaded).has_value());
    const SceneInstanceEntityResult stale = fixture.layers.resolve(reference);
    CHECK_FALSE_MESSAGE(static_cast<bool>(stale), "a stale reference never resolves into the reloaded instance");
    CHECK_FALSE(stale.error.empty());
    CHECK(static_cast<bool>(fixture.layers.resolve({reloaded, layerRoot})));
}

TEST_CASE("AdditiveScene: unloading an instance that is still loading cancels it and never commits") {
    AdditiveSceneFixture fixture;
    fixture.writeLayer(fixture.root / "layer.zenith-scene", "Layer root", false);
    const SceneInstanceId instance = fixture.layers.requestLoad(fixture.root / "layer.zenith-scene");

    REQUIRE(fixture.layers.unload(instance));
    fixture.finishLoads();

    CHECK_FALSE(fixture.layers.state(instance).has_value());
    CHECK(fixture.layers.scene(instance) == nullptr);
    CHECK(fixture.layers.committedInstances().empty());
    CHECK_MESSAGE(fixture.jobs.takeCompleted().empty(), "the manager consumes its cancelled job's completion");
}

TEST_CASE("AdditiveScene: clear unloads every instance and releases its assets") {
    AdditiveSceneFixture fixture;
    fixture.writeLayer(fixture.root / "layer.zenith-scene", "Layer root", true);
    const SceneInstanceId first = fixture.layers.requestLoad(fixture.root / "layer.zenith-scene");
    const SceneInstanceId second = fixture.layers.requestLoad(fixture.root / "layer.zenith-scene");
    fixture.finishLoads();
    const SceneInstanceId pending = fixture.layers.requestLoad(fixture.root / "layer.zenith-scene");
    REQUIRE(fixture.layers.committedInstances().size() == 2);

    fixture.layers.clear();
    fixture.finishLoads();
    fixture.assets.collectUnused(true);

    CHECK(fixture.layers.committedInstances().empty());
    for (const SceneInstanceId instance : {first, second, pending}) {
        CAPTURE(instance.value);
        CHECK_FALSE(fixture.layers.state(instance).has_value());
    }
    CHECK(fixture.assets.cpuBytes() == 0);
}

TEST_SUITE_END();
