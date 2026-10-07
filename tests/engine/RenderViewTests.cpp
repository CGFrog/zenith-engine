#include "engine/graphics/RenderViewPlanning.hpp"
#include "engine/graphics/Rendering.hpp"

#include "support/Doctest.hpp"

#include <array>
#include <iostream>
#include <vector>

TEST_SUITE_BEGIN("RenderView");

namespace {

TEST_CASE("RenderView: default view") {
    RenderRuntime rendering;
    const RenderViewId defaultView = rendering.defaultView();
    CHECK_MESSAGE((static_cast<bool>(defaultView) && defaultView.index == 0), "the default view exists in slot 0");
    CHECK_MESSAGE((rendering.viewAlive(defaultView) && rendering.viewCount() == 1), "only the default view is alive");
    CHECK_MESSAGE(rendering.activeView() == defaultView, "the default view starts active");
    CHECK_MESSAGE(!rendering.destroyView(defaultView), "the default view cannot be destroyed");
    CHECK_MESSAGE(rendering.viewAlive(defaultView), "the default view survives a destroy request");
    CHECK_MESSAGE(!rendering.viewTexture(defaultView), "no texture before the renderer publishes one");
}

TEST_CASE("RenderView: create and destroy") {
    RenderRuntime rendering;
    const RenderViewId first = rendering.createView();
    const RenderViewId second = rendering.createView();
    CHECK_MESSAGE((first && second && first.index != second.index && first.index != 0 && second.index != 0),
                  "created views get distinct non-default slots");
    CHECK_MESSAGE(rendering.viewCount() == 3, "created views count as alive");
    CHECK_MESSAGE(rendering.destroyView(first), "a created view can be destroyed");
    CHECK_MESSAGE(!rendering.viewAlive(first), "a destroyed view is no longer alive");
    CHECK_MESSAGE(!rendering.destroyView(first), "destroying twice is rejected");
    CHECK_MESSAGE(!rendering.requestViewSize(first, 64, 64), "a stale handle is rejected by setters");
    CHECK_MESSAGE((rendering.viewWidth(first) == 0 && !rendering.viewTexture(first)), "a stale handle reads as empty");
    const RenderViewId reused = rendering.createView();
    CHECK_MESSAGE(reused.index == first.index, "a freed slot is reused");
    CHECK_MESSAGE(reused.generation != first.generation, "a reused slot gets a new generation");
    CHECK_MESSAGE((!rendering.viewAlive(first) && rendering.viewAlive(reused)),
                  "the old handle stays stale after reuse");
    CHECK_MESSAGE(!rendering.viewAlive(RenderViewId{}), "the empty handle is never alive");
    CHECK_MESSAGE(!rendering.viewAlive({.index = 40, .generation = 1}), "an out-of-range slot is never alive");
}

TEST_CASE("RenderView: view capacity") {
    RenderRuntime rendering;
    std::vector<RenderViewId> views;
    for (uint32_t index = 1; index < RenderRuntime::maximumViews; ++index) {
        views.push_back(rendering.createView());
    }
    CHECK_MESSAGE(rendering.viewCount() == RenderRuntime::maximumViews, "views fill up to the maximum");
    CHECK_MESSAGE(!rendering.createView(), "creating beyond the maximum returns an empty handle");
    CHECK_MESSAGE(rendering.destroyView(views.back()), "a view can be destroyed at capacity");
    CHECK_MESSAGE((static_cast<bool>(rendering.createView())), "destroying frees capacity");
}

TEST_CASE("RenderView: active view") {
    RenderRuntime rendering;
    const RenderViewId view = rendering.createView();
    CHECK_MESSAGE((rendering.setActiveView(view) && rendering.activeView() == view),
                  "a created view can become active");
    CHECK_MESSAGE(rendering.destroyView(view), "the active view can be destroyed");
    CHECK_MESSAGE(rendering.activeView() == rendering.defaultView(),
                  "destroying the active view activates the default");
    CHECK_MESSAGE(!rendering.setActiveView(view), "a destroyed view cannot become active");
}

TEST_CASE("RenderView: view requests") {
    RenderRuntime rendering;
    const RenderViewId view = rendering.createView();
    CHECK_MESSAGE(rendering.requestViewSize(view, 320, 200), "a created view accepts a size request");
    CHECK_MESSAGE(rendering.setViewOverride(view, RenderView{.verticalFovRadians = 1.0f, .nearPlane = 0.5f}),
                  "a created view accepts a camera override");
    CHECK_MESSAGE(rendering.clearViewOverride(view), "a created view clears its override");
    CHECK_MESSAGE(rendering.setViewGridOverlay(view, GridOverlay{.spacing = 2.0f}), "a created view accepts a grid");
    const std::array<RenderObjectId, 4> outlines{7, 3, 7, 1};
    CHECK_MESSAGE(rendering.setViewOutlinedObjects(view, outlines), "a created view accepts outlines");
    RenderSettings cheap;
    cheap.antialiasing = AntialiasingMode::Off;
    CHECK_MESSAGE(rendering.setViewSettings(view, cheap), "a created view accepts a settings override");
    CHECK_MESSAGE(rendering.setViewInputActive(view, true), "a created view accepts input focus");
    // The default-view shorthands keep addressing the default view only.
    rendering.requestViewportSize(640, 360);
    rendering.setViewportInputActive(false);
    CHECK_MESSAGE(!rendering.viewStats(view).rendered, "nothing renders without a renderer");
}

TEST_CASE("RenderView: view isolation") {
    RenderRuntime rendering;
    const RenderViewId view = rendering.createView();
    CHECK_MESSAGE(!rendering.viewVisibleEntities(view), "views start without isolation");
    CHECK_MESSAGE(rendering.setViewVisibleEntities(view, std::vector<RenderObjectId>{9, 2, 9, 4}),
                  "a created view accepts an isolation set");
    CHECK_MESSAGE((rendering.viewVisibleEntities(view) == std::vector<RenderObjectId>{2, 4, 9}),
                  "isolation sets should be sorted and deduplicated");
    CHECK_MESSAGE((rendering.setViewVisibleEntities(rendering.defaultView(), std::vector<RenderObjectId>{}) &&
                   rendering.viewVisibleEntities(rendering.defaultView())->empty()),
                  "the default view accepts an empty isolation set, which draws nothing");
    CHECK_MESSAGE((rendering.setViewVisibleEntities(view, std::nullopt) && !rendering.viewVisibleEntities(view)),
                  "nullopt should leave isolation");
    rendering.setViewVisibleEntities(view, std::vector<RenderObjectId>{1});
    CHECK_MESSAGE(rendering.destroyView(view), "the view is destroyed");
    CHECK_MESSAGE((!rendering.setViewVisibleEntities(view, std::vector<RenderObjectId>{1}) &&
                   !rendering.viewVisibleEntities(view)),
                  "stale handles should be rejected");
    const RenderViewId reused = rendering.createView();
    CHECK_MESSAGE(!rendering.viewVisibleEntities(reused), "a reused slot should not inherit isolation");
    CHECK_MESSAGE(rendering.sceneVisibilityMode() == SceneVisibilityMode::Runtime,
                  "the renderer should default to the runtime visibility mode");
}

TEST_CASE("RenderView: compose view settings") {
    RenderSettings frame;
    frame.shadows = ShadowQuality::CascadedHigh;
    frame.localLights = LocalLightingQuality::Clustered;
    frame.particles = ParticleQuality::High;
    frame.environment = EnvironmentQuality::ImageBasedHigh;
    frame.antialiasing = AntialiasingMode::Taa;
    frame.ambientOcclusion = AmbientOcclusionQuality::HalfResolution;
    const RenderSettings same = composeViewSettings(frame, std::nullopt);
    CHECK_MESSAGE((same.antialiasing == AntialiasingMode::Taa && same.shadows == ShadowQuality::CascadedHigh),
                  "without an override a view uses the frame settings");

    RenderSettings cheap;
    cheap.antialiasing = AntialiasingMode::Off;
    cheap.ambientOcclusion = AmbientOcclusionQuality::Off;
    cheap.bloom = BloomQuality::Off;
    cheap.volumetricFog = VolumetricFogQuality::Off;
    cheap.shadows = ShadowQuality::Off;
    cheap.particles = ParticleQuality::Off;
    cheap.environment = EnvironmentQuality::Hemisphere;
    const RenderSettings composed = composeViewSettings(frame, cheap);
    CHECK_MESSAGE(
        (composed.antialiasing == AntialiasingMode::Off && composed.ambientOcclusion == AmbientOcclusionQuality::Off),
        "per-view features follow the view override");
    CHECK_MESSAGE((composed.shadows == ShadowQuality::CascadedHigh && composed.particles == ParticleQuality::High &&
                   composed.environment == EnvironmentQuality::ImageBasedHigh &&
                   composed.localLights == LocalLightingQuality::Clustered),
                  "shared once-per-frame work follows the frame settings");
}

TEST_CASE("RenderView: order rendered views") {
    std::vector<uint32_t> views{0, 2, 5};
    orderRenderedViews(views, 5);
    CHECK_MESSAGE((views == std::vector<uint32_t>{5, 0, 2}), "the active view leads, the rest keep slot order");
    views = {0, 2, 5};
    orderRenderedViews(views, 3);
    CHECK_MESSAGE((views == std::vector<uint32_t>{0, 2, 5}), "an active view that does not render changes nothing");
    views = {0};
    orderRenderedViews(views, 0);
    CHECK_MESSAGE((views == std::vector<uint32_t>{0}), "a single view stays alone");
}

TEST_CASE("RenderView: remap local shadow tiles") {
    const std::vector<LocalLight> tiled{{.stableId = 10}, {.stableId = 20}, {.stableId = 30}};
    const std::vector<int32_t> tiles{0, -1, 6};
    const std::vector<LocalLight> viewLights{{.stableId = 30}, {.stableId = 40}, {.stableId = 20}, {.stableId = 10}};
    const std::vector<int32_t> remapped = remapLocalShadowTiles(tiled, tiles, viewLights);
    CHECK_MESSAGE((remapped == std::vector<int32_t>{6, -1, -1, 0}), "shadow tiles follow lights by stable id");
    CHECK_MESSAGE(remapLocalShadowTiles(tiled, tiles, {}).empty(), "a view without lights gets no tiles");
}
} // namespace

UiImage solidImage(uint32_t width, uint32_t height) {
    return {.width = width,
            .height = height,
            .rgba = std::vector<uint8_t>(static_cast<size_t>(width) * height * 4u, 255),
            .filter = UiImageFilter::Nearest};
}

TEST_CASE("RenderView: ui images") {
    RenderRuntime rendering;
    CHECK_MESSAGE(!rendering.createUiImage({}), "an empty UI image is rejected");
    UiImage mismatched = solidImage(2, 2);
    mismatched.rgba.pop_back();
    CHECK_MESSAGE(!rendering.createUiImage(mismatched), "a UI image whose pixels do not match its size is rejected");

    const UiImageId first = rendering.createUiImage(solidImage(4, 2));
    CHECK_MESSAGE((first && rendering.uiImageAlive(first)), "a valid UI image is created");
    CHECK_MESSAGE(!rendering.uiImageTexture(first), "no UI image texture before the renderer publishes one");
    CHECK_MESSAGE(rendering.updateUiImage(first, solidImage(8, 8)), "a live UI image can be updated");
    CHECK_MESSAGE(!rendering.updateUiImage(first, {}), "an invalid update is rejected");

    CHECK_MESSAGE(rendering.destroyUiImage(first), "a UI image can be destroyed");
    CHECK_MESSAGE((!rendering.uiImageAlive(first) && !rendering.destroyUiImage(first)),
                  "a destroyed UI image is stale");
    CHECK_MESSAGE(!rendering.updateUiImage(first, solidImage(1, 1)), "a stale UI image handle is rejected");
    const UiImageId reused = rendering.createUiImage(solidImage(1, 1));
    CHECK_MESSAGE((reused.index == first.index && reused.generation != first.generation),
                  "a freed UI image slot is reused with a new generation");
    CHECK_MESSAGE(!rendering.uiImageAlive(first), "the old UI image handle stays stale after reuse");
}

TEST_CASE("RenderView: secondary scenes") {
    RenderRuntime rendering;
    const RenderViewId view = rendering.createView();
    CHECK_MESSAGE(!rendering.viewScene(view), "a view draws the application's scene by default");

    const RenderSceneId first = rendering.createScene();
    const RenderSceneId second = rendering.createScene();
    CHECK_MESSAGE(
        (first && second && !(first == second) && rendering.sceneAlive(first) && rendering.sceneAlive(second)),
        "created scenes are distinct and alive");
    CHECK_MESSAGE((rendering.setViewScene(view, first) && rendering.viewScene(view) == first),
                  "a created view binds a scene");
    CHECK_MESSAGE(
        (!rendering.setViewScene(rendering.defaultView(), first) && !rendering.viewScene(rendering.defaultView())),
        "the default view always draws the application's scene");
    CHECK_MESSAGE((rendering.setViewScene(view, {}) && !rendering.viewScene(view)),
                  "the empty handle returns a view to the application's scene");

    CHECK_MESSAGE((rendering.destroyScene(first) && !rendering.sceneAlive(first)), "a scene can be destroyed");
    CHECK_MESSAGE(!rendering.destroyScene(first), "destroying a scene twice is rejected");
    CHECK_MESSAGE((!rendering.setViewScene(view, first) && !rendering.setSceneContent(first, nullptr)),
                  "a destroyed scene's handle is rejected");
    const RenderSceneId reused = rendering.createScene();
    CHECK_MESSAGE((reused && reused.index == first.index && reused.generation != first.generation),
                  "a reused scene slot gets a new generation");

    rendering.setViewScene(view, second);
    rendering.destroyView(view);
    const RenderViewId recreated = rendering.createView();
    CHECK_MESSAGE((recreated.index == view.index && !rendering.viewScene(recreated)),
                  "a view created in a reused slot draws the application's scene");

    std::vector<RenderSceneId> scenes{reused, second};
    while (const RenderSceneId scene = rendering.createScene()) {
        scenes.push_back(scene);
    }
    CHECK_MESSAGE(scenes.size() == RenderRuntime::maximumScenes, "maximumScenes scenes can be alive at once");
}

TEST_SUITE_END();
