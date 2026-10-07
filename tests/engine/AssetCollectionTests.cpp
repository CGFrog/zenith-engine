// CPU-only coverage of runtime asset collection: a collection requested by a
// scene change or by gameplay runs once the renderer's frames in flight have
// dropped their leases, and evicts what nothing leases any more.

#include "engine/application/Application.hpp"
#include "engine/application/DeferredAssetCollection.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/graphics/DebugDraw.hpp"
#include "engine/module/GameServices.hpp"

#include "support/Doctest.hpp"

#include <array>
#include <filesystem>
#include <memory>
#include <string>

TEST_SUITE_BEGIN("AssetCollection");

namespace {

std::filesystem::path writeFont(const std::filesystem::path& root, const std::string& name) {
    const std::filesystem::path path = root / name;
    testsupport::writeText(path, "font bytes for " + name);
    return path;
}

// Frame slots that lease what they draw until they are reused, as the
// renderer's do (Renderer::framesInFlight).
struct FrameSlots {
    std::array<AssetLeaseSet, 3> slots;
    uint32_t next{0};

    void render(AssetLeaseSet drawn) {
        slots[next] = std::move(drawn);
        next = (next + 1) % static_cast<uint32_t>(slots.size());
    }
};

AssetLeaseSet leaseFont(const AssetManager& assets, FontHandle font) {
    AssetLeaseSet leases;
    leases.fonts.push_back(assets.lease(font));
    return leases;
}

class EmptyScene final : public ApplicationScene {
  public:
    SceneResult load(ApplicationContext&) override {
        return {.success = true};
    }

    void update(ApplicationContext&, const InputState&, float) override {}

    void fixedUpdate(ApplicationContext&, const InputState&, float) override {}

    void unload(ApplicationContext&) override {}
};

class TwoSceneApplication final : public Application {
  public:
    SceneKey initialScene() const override {
        return {1};
    }

    std::unique_ptr<ApplicationScene> createScene(SceneKey) override {
        return std::make_unique<EmptyScene>();
    }
};

class SwitchingScene final : public ApplicationScene {
  public:
    SceneResult load(ApplicationContext&) override {
        return {.success = true};
    }

    void update(ApplicationContext& context, const InputState&, float) override {
        context.control.requestSceneChange({2});
    }

    void fixedUpdate(ApplicationContext&, const InputState&, float) override {}

    void unload(ApplicationContext&) override {}
};

class SwitchingApplication final : public Application {
  public:
    SceneKey initialScene() const override {
        return {1};
    }

    std::unique_ptr<ApplicationScene> createScene(SceneKey) override {
        return std::make_unique<SwitchingScene>();
    }
};

} // namespace

// A floor's font stays leased by the frames that drew it; a collection
// requested at teardown runs once they all retired and evicts it, while an
// asset still in use survives.
TEST_CASE("AssetCollection: collection waits for frames in flight") {
    const testsupport::TempDirectory root("asset-collection-frames");
    AssetManager assets;
    const FontHandle floorFont = assets.loadFont(writeFont(root.path(), "floor.font")).handle;
    const FontHandle hudFont = assets.loadFont(writeFont(root.path(), "hud.font")).handle;
    const AssetLease<FontHandle> hudLease = assets.lease(hudFont);
    FrameSlots frames;
    for (int frame = 0; frame < 3; ++frame) {
        frames.render(leaseFont(assets, floorFont));
    }

    DeferredAssetCollection collection(static_cast<uint32_t>(frames.slots.size()));
    CHECK_MESSAGE(!collection.frameRendered(assets, nullptr), "no collection runs without a request");
    collection.request();
    CHECK_MESSAGE(collection.pending(), "a request is pending until it runs");
    frames.render({});
    CHECK_MESSAGE(!collection.frameRendered(assets, nullptr), "the first frame after a request does not collect");
    frames.render({});
    CHECK_MESSAGE((!collection.frameRendered(assets, nullptr) && assets.font(floorFont) != nullptr),
                  "the asset stays while a frame in flight leases it");
    // A second request (another teardown) restarts the countdown.
    collection.request();
    for (int frame = 0; frame < 2; ++frame) {
        frames.render({});
        CHECK_MESSAGE(!collection.frameRendered(assets, nullptr), "a later request restarts the countdown");
    }
    frames.render({});
    const std::optional<DeferredAssetCollectionResult> collected = collection.frameRendered(assets, nullptr);
    CHECK_MESSAGE((collected && collected->assets.assetsEvicted == 1 && !collection.pending()),
                  "the collection runs once every frame in flight retired");
    CHECK_MESSAGE(assets.font(floorFont) == nullptr, "the released asset is evicted and its handle is stale");
    CHECK_MESSAGE(assets.font(hudFont) != nullptr, "a leased asset survives the collection");
    CHECK_MESSAGE(!collection.frameRendered(assets, nullptr), "a collection runs once per request");
}

TEST_CASE("AssetCollection: scene change requests collection") {
    TwoSceneApplication application;
    Scene scene;
    AssetManager assets;
    PhysicsWorld physics;
    AudioSystem audio;
    ApplicationSession session(application, scene, assets, physics, audio);
    CHECK_MESSAGE(session.start().success, "the application starts");
    CHECK_MESSAGE(!session.takeAssetCollectionRequest(), "starting requests no collection");
    CHECK_MESSAGE((session.finishFrame().success && !session.takeAssetCollectionRequest()),
                  "a frame without a scene change requests no collection");

    ApplicationControl control;
    control.requestAssetCollection();
    CHECK_MESSAGE((control.assetCollectionRequested() && !control.actionPending()),
                  "an asset collection request is not a lifecycle action");
    session.shutdown();
}

TEST_CASE("AssetCollection: transition requests collection") {
    SwitchingApplication application;
    Scene scene;
    AssetManager assets;
    PhysicsWorld physics;
    AudioSystem audio;
    ApplicationSession session(application, scene, assets, physics, audio);
    CHECK_MESSAGE(session.start().success, "the switching application starts");
    session.update({}, 0.0f);
    CHECK_MESSAGE(session.finishFrame().success, "the scene change succeeds");
    CHECK_MESSAGE(session.takeAssetCollectionRequest(), "a scene change requests an asset collection");
    CHECK_MESSAGE(!session.takeAssetCollectionRequest(), "taking the request clears it");
    session.shutdown();
}

TEST_CASE("AssetCollection: gameplay request reaches application") {
    Logger logger;
    AssetManager assets(&logger);
    DebugDraw debugDraw;
    Scene scene;
    PhysicsWorld physics;
    AudioSystem audio;
    ApplicationControl control;
    RenderRuntime rendering;
    GameUiContext gameUi;
    ApplicationContext application{scene, assets, physics, audio, control, rendering, gameUi, logger};
    GameServices services(assets, {}, debugDraw, &logger);
    auto registry = std::make_shared<SystemRegistry>();
    registry->add({.name = "floor teardown",
                   .run = [](const SystemContext& context) { context.services->requestAssetCollection(); }});
    const SystemScheduler scheduler(registry);

    runGameplayPhase(scheduler, SystemPhase::Update,
                     {.scene = scene, .mode = SimulationMode::Play, .services = &services});
    CHECK_MESSAGE(!control.assetCollectionRequested(), "without an application the request waits");
    runGameplayPhase(
        SystemScheduler{}, SystemPhase::Update,
        {.scene = scene, .application = &application, .mode = SimulationMode::Play, .services = &services});
    CHECK_MESSAGE(control.assetCollectionRequested(), "the next phase with an application hands the request over");
    CHECK_MESSAGE(!services.takeAssetCollectionRequest(), "the services forget a request they handed over");
}

TEST_SUITE_END();
