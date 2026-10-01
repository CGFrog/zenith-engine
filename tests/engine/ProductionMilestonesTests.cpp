#include "engine/application/Application.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/graphics/EnvironmentLighting.hpp"
#include "engine/graphics/Lighting.hpp"
#include "engine/graphics/RenderConfiguration.hpp"
#include "engine/jobs/JobSystem.hpp"
#include "engine/resources/ContentManifest.hpp"
#include "engine/resources/DeferredReleaseQueue.hpp"
#include "engine/scene/AdditiveSceneManager.hpp"
#include "engine/scene/Prefab.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
int failures = 0;

class DynamicAssetScene final : public ApplicationScene {
  public:
    SceneResult load(ApplicationContext&) override {
        return {.success = true};
    }

    void update(ApplicationContext& context, const InputState&, float) override {
        if (m_loaded) {
            return;
        }
        const auto model = context.assets.loadModel(std::filesystem::path{ZENITH_ASSET_DIR} / "sample.gltf");
        if (!model) {
            m_error = model.error;
            return;
        }
        m_model = model.handle;
        const Entity root = context.scene.createEntity("Dynamic model");
        if (const SceneResult instantiated = context.scene.instantiateModel(root, model.handle, context.assets);
            !instantiated) {
            m_error = instantiated.error;
            return;
        }
        m_loaded = true;
    }

    void fixedUpdate(ApplicationContext&, const InputState&, float) override {}

    void unload(ApplicationContext&) override {}

    bool loaded() const {
        return m_loaded;
    }

    const std::string& error() const {
        return m_error;
    }

    ModelHandle model() const {
        return m_model;
    }

  private:
    bool m_loaded{false};
    ModelHandle m_model{};
    std::string m_error;
};

class DynamicAssetApplication final : public Application {
  public:
    SceneKey initialScene() const override {
        return {1};
    }

    std::unique_ptr<ApplicationScene> createScene(SceneKey) override {
        auto scene = std::make_unique<DynamicAssetScene>();
        activeScene = scene.get();
        return scene;
    }

    DynamicAssetScene* activeScene{nullptr};
};

class ReloadingAssetScene final : public ApplicationScene {
  public:
    explicit ReloadingAssetScene(SceneKey nextScene)
        : m_nextScene(nextScene) {}

    SceneResult load(ApplicationContext& context) override {
        const auto model = context.assets.reloadModel(std::filesystem::path{ZENITH_ASSET_DIR} / "sample.gltf");
        if (!model) {
            return {.error = model.error};
        }
        const Entity root = context.scene.createEntity("Reloaded model");
        return context.scene.instantiateModel(root, model.handle, context.assets);
    }

    void update(ApplicationContext& context, const InputState&, float) override {
        context.control.requestSceneChange(m_nextScene);
    }

    void fixedUpdate(ApplicationContext&, const InputState&, float) override {}

    void unload(ApplicationContext&) override {}

  private:
    SceneKey m_nextScene;
};

class ReloadingAssetApplication final : public Application {
  public:
    SceneKey initialScene() const override {
        return {1};
    }

    std::unique_ptr<ApplicationScene> createScene(SceneKey scene) override {
        return std::make_unique<ReloadingAssetScene>(SceneKey{scene.value == 1 ? 2u : 1u});
    }
};

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

std::filesystem::path temporaryRoot() {
    const auto root = std::filesystem::temp_directory_path() / "zenith-production-milestones-tests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

void writePartiallyInvalidModel(const std::filesystem::path& root) {
    const auto bufferPath = root / "invalid-model.bin";
    const float positions[] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const uint16_t indices[] = {0, 1, 9};
    {
        std::ofstream output(bufferPath, std::ios::binary);
        output.write(reinterpret_cast<const char*>(positions), sizeof(positions));
        output.write(reinterpret_cast<const char*>(indices), sizeof(indices));
    }
    std::ofstream output(root / "invalid-model.gltf");
    output << R"({
  "asset":{"version":"2.0"},
  "scene":0,
  "scenes":[{"nodes":[0]}],
  "nodes":[{"mesh":0}],
  "meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,"material":0}]}],
  "materials":[{"pbrMetallicRoughness":{"baseColorFactor":[1,0,0,1]}}],
  "buffers":[{"byteLength":42,"uri":"invalid-model.bin"}],
  "bufferViews":[
    {"buffer":0,"byteOffset":0,"byteLength":36,"target":34962},
    {"buffer":0,"byteOffset":36,"byteLength":6,"target":34963}
  ],
  "accessors":[
    {"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},
    {"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}
  ]
})";
}

void testAssetLifetime(const std::filesystem::path& root) {
    const auto fontPath = root / "font.bin";
    {
        std::ofstream output(fontPath, std::ios::binary);
        output << "font bytes";
    }
    AssetManager assets;
    const auto loaded = assets.loadFont(fontPath);
    expect(static_cast<bool>(loaded), "font fixture loads");
    const FontHandle old = loaded.handle;
    {
        auto firstLease = assets.lease(old);
        auto secondLease = firstLease;
        expect(firstLease && secondLease, "shared leases resolve");
        const AssetCollectionResult retained = assets.collectUnused(true);
        expect(retained.assetsEvicted == 0 && assets.font(old) != nullptr, "live leases prevent eviction");
    }
    expect(assets.collectUnused(true).assetsEvicted == 1, "unreferenced asset is evicted deterministically");
    expect(assets.font(old) == nullptr, "stale handle fails resolution");
    const auto reloaded = assets.loadFont(fontPath);
    expect(reloaded.handle.index == old.index && reloaded.handle.generation != old.generation,
           "recycled slot increments its generation");
    expect(assets.fontHandles() == std::vector<FontHandle>{reloaded.handle},
           "font enumeration preserves recycled handle generations");
    assets.setMemoryBudget({0});
    const auto pressure = assets.collectUnused();
    expect(pressure.budgetSatisfied && pressure.bytesRetained == 0, "budget pressure collects unused bytes");

    const auto retainedFont = assets.loadFont(fontPath);
    const auto retainedLease = assets.lease(retainedFont.handle);
    const auto diagnosticsBeforeFailure = assets.diagnostics();
    const uint64_t bytesBeforeFailure = assets.cpuBytes();
    const uint64_t revisionBeforeFailure = assets.revision();
    writePartiallyInvalidModel(root);
    const auto invalidModel = assets.loadModel(root / "invalid-model.gltf");
    expect(!invalidModel, "invalid model fails after importer allocation begins");
    expect(assets.diagnostics().size() == diagnosticsBeforeFailure.size() && assets.cpuBytes() == bytesBeforeFailure &&
               assets.revision() == revisionBeforeFailure && assets.font(retainedFont.handle) != nullptr &&
               retainedLease,
           "failed imports restore recycled slots, ownership, accounting, and revision state");
}

void testRecoverableReload(const std::filesystem::path& root) {
    const auto source = std::filesystem::path{ZENITH_ASSET_DIR} / "sample.gltf";
    const auto reloadPath = root / "reload.gltf";
    std::filesystem::copy_file(source, reloadPath, std::filesystem::copy_options::overwrite_existing);

    AssetManager assets;
    const auto original = assets.loadModel(reloadPath);
    expect(static_cast<bool>(original), "reload fixture initially loads");
    const auto liveLease = assets.lease(original.handle);

    writePartiallyInvalidModel(root);
    std::filesystem::copy_file(root / "invalid-model.gltf", reloadPath,
                               std::filesystem::copy_options::overwrite_existing);
    const auto failedReload = assets.reloadModel(reloadPath);
    const auto cachedAfterFailure = assets.loadModel(reloadPath);
    expect(!failedReload && cachedAfterFailure.handle == original.handle && assets.model(original.handle) != nullptr,
           "failed reload preserves the cached immutable generation");
    expect(assets.collectUnused(true).assetsEvicted > 0 && assets.model(original.handle) != nullptr && liveLease,
           "reload failure collection preserves the leased previous generation");
}

void testLiveSceneOwnership() {
    DynamicAssetApplication application;
    Scene scene;
    AssetManager assets;
    PhysicsWorld physics;
    AudioSystem audio;
    ApplicationSession session(application, scene, assets, physics, audio);
    expect(session.start().success, "dynamic-asset application starts");
    session.update({}, 0.0f);
    expect(application.activeScene != nullptr && application.activeScene->loaded(),
           "application update adds a model to the live scene");
    if (application.activeScene != nullptr) {
        expect(application.activeScene->error().empty(), "dynamic model load has no error");
    }
    const ModelHandle liveModel = application.activeScene != nullptr ? application.activeScene->model() : ModelHandle{};
    assets.collectUnused(true);
    expect(assets.model(liveModel) != nullptr, "scene assets added after load remain strongly referenced");
    bool foundGpuEstimate = false;
    for (const AssetDiagnostic& diagnostic : assets.diagnostics()) {
        foundGpuEstimate |= diagnostic.kind == AssetKind::Mesh && diagnostic.gpuBytes > 0;
    }
    expect(foundGpuEstimate, "asset diagnostics report deterministic GPU byte estimates");
    session.shutdown();
    expect(assets.collectUnused(true).assetsEvicted > 0 && assets.model(liveModel) == nullptr,
           "scene assets become collectible and stale after application shutdown");
}

void testTransitionCollection() {
    ReloadingAssetApplication application;
    Scene scene;
    AssetManager assets;
    PhysicsWorld physics;
    AudioSystem audio;
    ApplicationSession session(application, scene, assets, physics, audio);
    expect(session.start().success, "reloading application starts");
    assets.collectUnused(true);
    const uint64_t retainedBaseline = assets.cpuBytes();
    expect(retainedBaseline > 0, "active reloading scene retains its model graph");
    for (int transition = 0; transition < 12; ++transition) {
        session.update({}, 0.0f);
        expect(session.finishFrame().success, "reloading scene transition succeeds");
        assets.collectUnused(true);
        expect(assets.cpuBytes() == retainedBaseline,
               "repeated transitions return retained asset memory to the baseline");
    }
    session.shutdown();
    assets.collectUnused(true);
    expect(assets.cpuBytes() == 0, "transition assets are fully released after session shutdown");
}

void testDeferredFrameOwnership() {
    AssetManager assets;
    const auto loaded = assets.loadModel(std::filesystem::path{ZENITH_ASSET_DIR} / "sample.gltf");
    Scene scene;
    const Entity root = scene.createEntity("In-flight model");
    expect(loaded && scene.instantiateModel(root, loaded.handle, assets).success,
           "in-flight ownership fixture instantiates");

    std::array<AssetLeaseSet, 3> frameLeases;
    for (AssetLeaseSet& leases : frameLeases) {
        leases = assets.leaseSceneAssets(scene);
    }
    scene.clear();
    assets.collectUnused(true);
    expect(assets.model(loaded.handle) != nullptr, "submitted frame leases defer asset destruction");
    frameLeases[0].clear();
    frameLeases[1].clear();
    assets.collectUnused(true);
    expect(assets.model(loaded.handle) != nullptr, "any unfinished frame continues to retain its resources");
    frameLeases[2].clear();
    assets.collectUnused(true);
    expect(assets.model(loaded.handle) == nullptr, "resources are reclaimed after the final frame retires");
}

void testJobsAndAdditiveScenes(const std::filesystem::path& root) {
    JobSystem jobs({.queueCapacity = 8, .deterministic = true});
    std::vector<int> order;
    const JobHandle low = jobs.submit(
        [&](JobContext&) {
            order.push_back(1);
            return JobResult{};
        },
        1);
    const JobHandle high = jobs.submit(
        [&](JobContext&) {
            order.push_back(2);
            return JobResult{};
        },
        2);
    jobs.pumpOne();
    jobs.pumpOne();
    expect(order == std::vector<int>({2, 1}), "job priorities are deterministic");
    expect(low.status() == JobStatus::Succeeded && high.status() == JobStatus::Succeeded,
           "completed jobs publish status");
    jobs.takeCompleted();

    AssetManager assets;
    const SceneDocumentOptions options{.assetRoot = ZENITH_ASSET_DIR};
    const auto scenePath = root / "layer.zenith-scene";
    const auto model = assets.loadModel(std::filesystem::path{ZENITH_ASSET_DIR} / "sample.gltf");
    Scene authored;
    const Entity authoredRoot = authored.createEntity("Layer model");
    expect(model && authored.instantiateModel(authoredRoot, model.handle, assets).success &&
               SceneDocument::save(scenePath, authored, assets, options).success,
           "asset-bearing additive scene fixture saves");
    authored.clear();
    assets.collectUnused(true);

    AdditiveSceneManager layers(jobs, assets, options);
    const SceneInstanceId first = layers.requestLoad(scenePath, 2);
    const JobHandle unrelated =
        jobs.submit([](JobContext&) { return JobResult{.payload = "unrelated completion"}; }, 0);
    jobs.pumpOne();
    jobs.pumpOne();
    layers.update();
    expect(layers.state(first)->status == AdditiveSceneStatus::Committed && layers.scene(first) != nullptr,
           "background parse commits an intact additive instance");
    PrefabDocument sharedPrefab;
    sharedPrefab.entities.push_back({{1}, "Shared prefab", std::nullopt, glm::mat4(1.0f)});
    expect(savePrefab(root / "shared.prefab", sharedPrefab).success, "shared layer prefab fixture saves");
    PrefabSystem sharedPrefabs(root);
    Scene primaryScene;
    const PrefabInstanceResult primaryPrefab = sharedPrefabs.instantiate("shared.prefab", {300}, primaryScene);
    const PrefabInstanceResult layeredPrefab = sharedPrefabs.instantiate("shared.prefab", {301}, *layers.scene(first));
    expect(primaryPrefab && layeredPrefab,
           "primary scenes and additive scene layers share runtime prefab definitions without editor state");
    const auto unrelatedCompletions = jobs.takeCompleted();
    expect(unrelated && unrelatedCompletions.size() == 1 && unrelatedCompletions.front().handle.id() == unrelated.id(),
           "additive loading leaves other consumers' job completions queued");
    assets.collectUnused(true);
    const auto committedModels = layers.scene(first)->registry().view<const ModelComponent>();
    const ModelHandle committedModel = committedModels.empty()
                                           ? ModelHandle{}
                                           : committedModels.get<const ModelComponent>(*committedModels.begin()).model;
    expect(assets.model(committedModel) != nullptr, "committed additive instances retain their asset generations");
    const SceneInstanceId missing = layers.requestLoad(root / "missing.scene");
    jobs.pumpOne();
    layers.update();
    expect(layers.state(missing)->status == AdditiveSceneStatus::Failed && layers.scene(first) != nullptr,
           "failed additive loads preserve committed instances");
    expect(layers.unload(first) && layers.committedInstances().empty(), "additive instances unload explicitly");
    assets.collectUnused(true);
    expect(assets.model(committedModel) == nullptr, "additive unload releases instance asset ownership");

    const auto highPath = root / "high-priority.zenith-scene";
    const auto lowPath = root / "low-priority.zenith-scene";
    std::filesystem::copy_file(scenePath, highPath, std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(scenePath, lowPath, std::filesystem::copy_options::overwrite_existing);
    const std::array streamingRequests = {
        StreamingSceneRequest{lowPath, 1},
        StreamingSceneRequest{highPath, 10},
    };
    layers.synchronizeStreamingSet(streamingRequests);
    const SceneInstanceId highInstance = *layers.streamingInstance(highPath);
    const SceneInstanceId lowInstance = *layers.streamingInstance(lowPath);
    jobs.pumpOne();
    layers.update();
    expect(layers.state(highInstance)->status == AdditiveSceneStatus::Committed &&
               layers.state(lowInstance)->status == AdditiveSceneStatus::Loading,
           "streaming requests execute in application-provided priority order");

    const Scene* highScene = layers.scene(highInstance);
    const Entity highEntity = *highScene->registry().view<const SceneEntityIdComponent>().begin();
    const SceneEntityId stableEntity = highScene->id(highEntity);
    expect(static_cast<bool>(layers.resolve({highInstance, stableEntity})),
           "cross-scene references resolve committed stable entity IDs");
    expect(!layers.resolve({highInstance, SceneEntityId{999999}}), "cross-scene references reject missing entity IDs");
    expect(!layers.resolve({SceneInstanceId{999999}, stableEntity}),
           "cross-scene references reject missing scene instances");

    const std::array retainedStreamingRequest = {StreamingSceneRequest{highPath, 10}};
    layers.synchronizeStreamingSet(retainedStreamingRequest);
    expect(!layers.streamingInstance(lowPath) && layers.streamingInstance(highPath),
           "leaving the streaming set cancels and unloads the departing request");
    jobs.pumpOne();
    layers.update();
    layers.synchronizeStreamingSet(std::span<const StreamingSceneRequest>{});
    expect(layers.committedInstances().empty(), "empty streaming set unloads committed streamed scenes");

    const SceneInstanceId cancelled = layers.requestLoad(scenePath);
    layers.cancel(cancelled);
    jobs.pumpOne();
    layers.update();
    expect(layers.state(cancelled)->status == AdditiveSceneStatus::Cancelled,
           "cancellation before execution publishes a terminal cancelled state");

    const auto malformedScenePath = root / "malformed-layer.zenith-scene";
    {
        std::ofstream malformed(malformedScenePath);
        malformed << R"({
  "version": 1,
  "entities": [{
    "id": 1,
    "name": "Missing asset",
    "parent": null,
    "local_transform": [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1],
    "components": {"model": {"path": "missing-model.gltf"}}
  }]
})";
    }
    const SceneInstanceId failedCommit = layers.requestLoad(malformedScenePath);
    jobs.pumpOne();
    layers.update();
    expect(layers.state(failedCommit)->status == AdditiveSceneStatus::Failed && layers.committedInstances().empty(),
           "failed main-thread commits preserve all previously committed instances");

    for (int iteration = 0; iteration < 16; ++iteration) {
        const SceneInstanceId repeated = layers.requestLoad(scenePath);
        jobs.pumpOne();
        layers.update();
        expect(layers.state(repeated)->status == AdditiveSceneStatus::Committed,
               "repeated additive load commits deterministically");
        assets.collectUnused(true);
        expect(layers.unload(repeated), "repeated additive unload succeeds");
        assets.collectUnused(true);
        expect(assets.cpuBytes() == 0, "repeated additive unload and collection release all asset memory");
    }

    JobSystem shutdownJobs({.queueCapacity = 8, .deterministic = true});
    AdditiveSceneManager shutdownLayers(shutdownJobs, assets, options);
    shutdownLayers.requestLoad(scenePath);
    shutdownLayers.requestLoad(scenePath);
    shutdownLayers.clear();
    shutdownJobs.shutdown(true);
    shutdownLayers.update();
    expect(shutdownJobs.takeCompleted().empty(),
           "shutdown during loading publishes and consumes every cancelled completion");
}

void testPrefabs(const std::filesystem::path& root) {
    PrefabDocument child;
    child.entities.push_back({{1}, "Child", std::nullopt, glm::mat4(1.0f)});
    expect(savePrefab(root / "child.prefab", child).success, "child prefab saves");
    PrefabDocument parent;
    parent.entities.push_back({{1}, "Root", std::nullopt, glm::mat4(1.0f)});
    parent.nested.push_back({2, "child.prefab", PrefabEntityId{1}, {{{1}, "name", R"("Overridden")"}}});
    std::string encoded;
    expect(serializePrefab(parent, encoded).success, "prefab serializes");
    PrefabDocument decoded;
    expect(deserializePrefab(encoded, decoded).success && decoded.nested.size() == 1, "prefab round trips");
    expect(savePrefab(root / "parent.prefab", parent).success, "parent prefab saves");
    Scene scene;
    PrefabSystem prefabs(root);
    const PrefabInstanceResult instance = prefabs.instantiate("parent.prefab", {42}, scene);
    expect(instance.success && instance.entities.size() == 2, "nested prefab instantiates deterministically");
    bool foundOverride = false;
    for (Entity entity : instance.entities) {
        foundOverride |= scene.registry().get<NameComponent>(entity).name == "Overridden";
    }
    expect(foundOverride, "nested overrides remain separate and are applied");

    Scene secondScene;
    const PrefabInstanceResult secondInstance = prefabs.instantiate("parent.prefab", {42}, secondScene);
    std::vector<uint64_t> firstIds;
    std::vector<uint64_t> secondIds;
    for (Entity entity : instance.entities) {
        firstIds.push_back(scene.id(entity).value);
    }
    for (Entity entity : secondInstance.entities) {
        secondIds.push_back(secondScene.id(entity).value);
    }
    std::ranges::sort(firstIds);
    std::ranges::sort(secondIds);
    expect(firstIds == secondIds, "prefab instance IDs remap deterministically across scenes");

    child.entities.front().name = "Updated child";
    expect(savePrefab(root / "child.prefab", child).success, "prefab source update saves");
    const PrefabInstanceResult refreshed = prefabs.refresh("parent.prefab", {42}, scene, instance.root);
    foundOverride = false;
    for (Entity entity : refreshed.entities) {
        foundOverride |= scene.registry().get<NameComponent>(entity).name == "Overridden";
    }
    expect(refreshed && foundOverride, "source refresh preserves valid instance overrides");
    const std::array orphanedOverrides = {PrefabOverride{{999}, "name", R"("Missing")"}};
    Scene orphanScene;
    const PrefabInstanceResult orphaned =
        prefabs.instantiate("child.prefab", {91}, orphanScene, entt::null, orphanedOverrides);
    expect(orphaned && orphaned.diagnostics.size() == 1 &&
               orphaned.diagnostics.front().find("Orphaned override") != std::string::npos,
           "source refresh reports orphaned overrides as actionable diagnostics");

    const auto entityCountBeforeFailure = scene.registry().view<const SceneEntityIdComponent>().size();
    const PrefabInstanceResult missing = prefabs.instantiate("missing.prefab", {77}, scene);
    expect(!missing && scene.registry().view<const SceneEntityIdComponent>().size() == entityCountBeforeFailure,
           "missing prefab sources preserve previously valid runtime instances");

    PrefabDocument cycle;
    cycle.entities.push_back({{1}, "Cycle", std::nullopt, glm::mat4(1.0f)});
    cycle.nested.push_back({1, "cycle.prefab", std::nullopt, {}});
    savePrefab(root / "cycle.prefab", cycle);
    Scene cycleScene;
    expect(!prefabs.instantiate("cycle.prefab", {7}, cycleScene), "nested prefab cycles are rejected");

    PrefabDocument duplicateEntities;
    duplicateEntities.entities.push_back({{1}, "One", std::nullopt, glm::mat4(1.0f)});
    duplicateEntities.entities.push_back({{1}, "Two", std::nullopt, glm::mat4(1.0f)});
    std::string duplicateEncoding;
    expect(serializePrefab(duplicateEntities, duplicateEncoding).success,
           "duplicate prefab fixture can be encoded for validation");
    PrefabDocument invalid;
    expect(!deserializePrefab(duplicateEncoding, invalid), "duplicate prefab entity IDs are rejected");

    PrefabDocument duplicateNested;
    duplicateNested.entities.push_back({{1}, "Root", std::nullopt, glm::mat4(1.0f)});
    duplicateNested.nested.push_back({2, "child.prefab", std::nullopt, {}});
    duplicateNested.nested.push_back({2, "child.prefab", std::nullopt, {}});
    std::string duplicateNestedEncoding;
    expect(serializePrefab(duplicateNested, duplicateNestedEncoding).success &&
               !deserializePrefab(duplicateNestedEncoding, invalid),
           "duplicate nested-prefab IDs are rejected");

    PrefabDocument hierarchyCycle;
    hierarchyCycle.entities.push_back({{1}, "One", PrefabEntityId{2}, glm::mat4(1.0f)});
    hierarchyCycle.entities.push_back({{2}, "Two", PrefabEntityId{1}, glm::mat4(1.0f)});
    std::string hierarchyCycleEncoding;
    expect(serializePrefab(hierarchyCycle, hierarchyCycleEncoding).success &&
               !deserializePrefab(hierarchyCycleEncoding, invalid),
           "local prefab hierarchy cycles are rejected");

    std::filesystem::copy_file(std::filesystem::path{ZENITH_ASSET_DIR} / "sample.gltf", root / "sample.gltf",
                               std::filesystem::copy_options::overwrite_existing);
    PrefabDocument assetPrefab;
    PrefabEntity assetEntity{{1}, "Asset", std::nullopt, glm::mat4(1.0f)};
    assetEntity.modelAsset = "sample.gltf";
    assetPrefab.entities.push_back(std::move(assetEntity));
    expect(savePrefab(root / "asset.prefab", assetPrefab).success, "project-relative prefab asset saves");
    AssetManager assets;
    PrefabSystem assetPrefabs(root, &assets);
    Scene assetScene;
    const PrefabInstanceResult assetInstance = assetPrefabs.instantiate("asset.prefab", {101}, assetScene);
    expect(assetInstance && !assetScene.registry().view<const ModelComponent>().empty(),
           "runtime prefab instantiation resolves project-relative asset references without editor state");
}

void testLightingAndConfiguration() {
    Scene scene;
    for (int index = 0; index < 5; ++index) {
        const Entity entity = scene.createEntity("Light");
        scene.registry().emplace<PointLightComponent>(entity, glm::vec3(1.0f), 10.0f + index, 5.0f);
        scene.registry().get<TransformComponent>(entity).local[3] = glm::vec4(index - 2.0f, 0.0f, -4.0f, 1.0f);
    }
    const Entity probeA = scene.createEntity("Probe A");
    scene.registry().emplace<ReflectionProbeComponent>(probeA, glm::vec3(5.0f), 1.0f, 0, "a.hdr");
    const Entity probeB = scene.createEntity("Probe B");
    scene.registry().emplace<ReflectionProbeComponent>(probeB, glm::vec3(5.0f), 1.0f, 1, "b.hdr");
    scene.updateWorldTransforms();
    const auto lights = gatherLocalLights(scene);
    const auto selected = selectLocalLights(lights, glm::vec3(-2.0f, 0.0f, -4.0f), 2);
    expect(lights.size() == 5 && std::ranges::is_sorted(lights, {}, &LocalLight::stableId),
           "gathered local lights are complete and in stable-id order");
    expect(selected.size() == 2 && selected[0].stableId < selected[1].stableId,
           "renderer-facing light selection is bounded and deterministic");
    const auto probes = blendReflectionProbes(scene, glm::vec3(0.0f));
    expect(probes.size() == 2 && probes.front().entity == probeB, "probe priority and blending are deterministic");
    const LocalShadowRequest shadowRequests[]{
        {.stableId = 20, .type = LocalLightType::Point, .resolution = 128, .priority = 1.0f},
        {.stableId = 10, .type = LocalLightType::Spot, .resolution = 256, .priority = 2.0f},
        {.stableId = 30, .type = LocalLightType::Spot, .resolution = 512, .priority = 0.5f}};
    const LocalShadowAtlas shadowAtlas = allocateLocalShadowAtlas(shadowRequests, 512, 512);
    expect(shadowAtlas.tiles.size() == 7 && shadowAtlas.tiles.front().stableId == 10 && shadowAtlas.rejectedLights == 1,
           "local-shadow atlas allocation is deterministic, face-aware, and rejects over-budget lights");
    expect(std::ranges::all_of(
               shadowAtlas.tiles,
               [](const LocalShadowTile& tile) { return tile.x + tile.size <= 512 && tile.y + tile.size <= 512; }),
           "local-shadow atlas tiles remain within the configured memory extent");

    RenderSettings requested;
    requested.environment = EnvironmentQuality::ImageBasedHigh;
    requested.localLights = LocalLightingQuality::Clustered;
    requested.maximumLocalLights = 4096;
    requested.preferGpuDrivenSubmission = true;
    requested.visualizeShadowCascades = true;
    requested.antialiasing = AntialiasingMode::Taa;
    requested.ambientOcclusion = AmbientOcclusionQuality::HalfResolution;
    requested.reflections = ReflectionQuality::ScreenSpaceFullResolution;
    requested.dynamicResolution = {.enabled = true,
                                   .minimumScale = 0.7f,
                                   .maximumScale = 0.95f,
                                   .targetGpuMilliseconds = 13.0f,
                                   .adjustmentRate = 0.02f,
                                   .quantization = 32};
    requested.budgets.shadow = {1.25, 0.75};
    requested.budgets.upload = {0.5, 0.25};
    requested.budgets.temporalResolve = {0.0, 0.4};
    requested.budgets.ambientOcclusion = {0.0, 0.6};
    requested.budgets.reflections = {0.0, 0.8};
    const ValidatedRenderSettings validated = validateRenderSettings(requested, {});
    expect(validated.settings.environment == EnvironmentQuality::Hemisphere &&
               validated.settings.localLights == LocalLightingQuality::Clustered &&
               validated.settings.maximumLocalLights == RenderCapabilities{}.maximumLocalLights &&
               !validated.settings.preferGpuDrivenSubmission && !validated.fallbacks.empty(),
           "unsupported quality choices select explicit fallbacks");
    std::string encoded;
    RenderSettings decoded;
    expect(serializeRenderSettings(requested, encoded) && deserializeRenderSettings(encoded, decoded) &&
               decoded.environment == requested.environment && decoded.budgets.shadow.cpuMilliseconds == 1.25 &&
               decoded.budgets.upload.gpuMilliseconds == 0.25 && decoded.visualizeShadowCascades &&
               decoded.antialiasing == AntialiasingMode::Taa &&
               decoded.ambientOcclusion == AmbientOcclusionQuality::HalfResolution &&
               decoded.reflections == ReflectionQuality::ScreenSpaceFullResolution &&
               decoded.dynamicResolution.enabled && decoded.dynamicResolution.quantization == 32 &&
               decoded.budgets.temporalResolve.gpuMilliseconds == 0.4 &&
               decoded.budgets.ambientOcclusion.gpuMilliseconds == 0.6 &&
               decoded.budgets.reflections.gpuMilliseconds == 0.8,
           "render quality settings and per-pass CPU/GPU budgets round trip");

    TextureAsset environment;
    environment.width = 2;
    environment.height = 2;
    environment.rgba = {255, 128, 64, 255, 128, 64, 32, 255, 16, 32, 64, 255, 32, 64, 128, 255};
    EnvironmentLightingCache cache;
    const PrefilteredEnvironment& first = cache.preprocess(environment, 4);
    const uint64_t firstHash = first.sourceHash;
    const PrefilteredEnvironment& cached = cache.preprocess(environment, 4);
    expect(cache.entryCount() == 1 && cached.sourceHash == firstHash && cached.specularRoughnessLevels.size() == 4,
           "environment preprocessing is deterministic and content-cached");
}

void testDeferredReleaseAndManifest(const std::filesystem::path& root) {
    DeferredReleaseQueue releases;
    std::vector<int> order;
    releases.defer(3, [&] { order.push_back(3); });
    releases.defer(1, [&] { order.push_back(1); });
    expect(releases.collect(2) == 1 && order == std::vector<int>({1}) && releases.pending() == 1,
           "resource release waits for its completion serial without an idle wait");
    releases.collect(3);
    expect(order == std::vector<int>({1, 3}), "deferred destruction order is deterministic");

    const auto package = root / "package";
    std::filesystem::create_directories(package);
    {
        std::ofstream output(package / "payload.txt", std::ios::binary);
        output << "abc";
    }
    {
        std::ofstream output(package / "content-manifest.json");
        output
            << R"({"formatVersion":1,"productVersion":"test","files":[{"path":"payload.txt","sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}]})";
    }
    expect(validateContentManifest(package).success, "content manifest validates before services initialize");
    {
        std::ofstream output(package / "payload.txt", std::ios::app);
        output << "corrupt";
    }
    expect(!validateContentManifest(package), "content corruption is rejected");
}
} // namespace

int main() {
    const std::filesystem::path root = temporaryRoot();
    testAssetLifetime(root);
    testRecoverableReload(root);
    testLiveSceneOwnership();
    testTransitionCollection();
    testDeferredFrameOwnership();
    testJobsAndAdditiveScenes(root);
    testPrefabs(root);
    testLightingAndConfiguration();
    testDeferredReleaseAndManifest(root);
    std::filesystem::remove_all(root);
    if (failures == 0) {
        std::cout << "Production milestone tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
