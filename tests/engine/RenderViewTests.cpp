#include "engine/graphics/RenderViewPlanning.hpp"
#include "engine/graphics/Rendering.hpp"

#include <array>
#include <iostream>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void testDefaultView() {
    RenderRuntime rendering;
    const RenderViewId defaultView = rendering.defaultView();
    expect(static_cast<bool>(defaultView) && defaultView.index == 0, "the default view exists in slot 0");
    expect(rendering.viewAlive(defaultView) && rendering.viewCount() == 1, "only the default view is alive");
    expect(rendering.activeView() == defaultView, "the default view starts active");
    expect(!rendering.destroyView(defaultView), "the default view cannot be destroyed");
    expect(rendering.viewAlive(defaultView), "the default view survives a destroy request");
    expect(!rendering.viewTexture(defaultView), "no texture before the renderer publishes one");
}

void testCreateAndDestroy() {
    RenderRuntime rendering;
    const RenderViewId first = rendering.createView();
    const RenderViewId second = rendering.createView();
    expect(first && second && first.index != second.index && first.index != 0 && second.index != 0,
           "created views get distinct non-default slots");
    expect(rendering.viewCount() == 3, "created views count as alive");
    expect(rendering.destroyView(first), "a created view can be destroyed");
    expect(!rendering.viewAlive(first), "a destroyed view is no longer alive");
    expect(!rendering.destroyView(first), "destroying twice is rejected");
    expect(!rendering.requestViewSize(first, 64, 64), "a stale handle is rejected by setters");
    expect(rendering.viewWidth(first) == 0 && !rendering.viewTexture(first), "a stale handle reads as empty");
    const RenderViewId reused = rendering.createView();
    expect(reused.index == first.index, "a freed slot is reused");
    expect(reused.generation != first.generation, "a reused slot gets a new generation");
    expect(!rendering.viewAlive(first) && rendering.viewAlive(reused), "the old handle stays stale after reuse");
    expect(!rendering.viewAlive(RenderViewId{}), "the empty handle is never alive");
    expect(!rendering.viewAlive({.index = 40, .generation = 1}), "an out-of-range slot is never alive");
}

void testViewCapacity() {
    RenderRuntime rendering;
    std::vector<RenderViewId> views;
    for (uint32_t index = 1; index < RenderRuntime::maximumViews; ++index) {
        views.push_back(rendering.createView());
    }
    expect(rendering.viewCount() == RenderRuntime::maximumViews, "views fill up to the maximum");
    expect(!rendering.createView(), "creating beyond the maximum returns an empty handle");
    expect(rendering.destroyView(views.back()), "a view can be destroyed at capacity");
    expect(static_cast<bool>(rendering.createView()), "destroying frees capacity");
}

void testActiveView() {
    RenderRuntime rendering;
    const RenderViewId view = rendering.createView();
    expect(rendering.setActiveView(view) && rendering.activeView() == view, "a created view can become active");
    expect(rendering.destroyView(view), "the active view can be destroyed");
    expect(rendering.activeView() == rendering.defaultView(), "destroying the active view activates the default");
    expect(!rendering.setActiveView(view), "a destroyed view cannot become active");
}

void testViewRequests() {
    RenderRuntime rendering;
    const RenderViewId view = rendering.createView();
    expect(rendering.requestViewSize(view, 320, 200), "a created view accepts a size request");
    expect(rendering.setViewOverride(view, RenderView{.verticalFovRadians = 1.0f, .nearPlane = 0.5f}),
           "a created view accepts a camera override");
    expect(rendering.clearViewOverride(view), "a created view clears its override");
    expect(rendering.setViewGridOverlay(view, GridOverlay{.spacing = 2.0f}), "a created view accepts a grid");
    const std::array<RenderObjectId, 4> outlines{7, 3, 7, 1};
    expect(rendering.setViewOutlinedObjects(view, outlines), "a created view accepts outlines");
    RenderSettings cheap;
    cheap.antialiasing = AntialiasingMode::Off;
    expect(rendering.setViewSettings(view, cheap), "a created view accepts a settings override");
    expect(rendering.setViewInputActive(view, true), "a created view accepts input focus");
    // The default-view shorthands keep addressing the default view only.
    rendering.requestViewportSize(640, 360);
    rendering.setViewportInputActive(false);
    expect(!rendering.viewStats(view).rendered, "nothing renders without a renderer");
}

void testViewIsolation() {
    RenderRuntime rendering;
    const RenderViewId view = rendering.createView();
    expect(!rendering.viewVisibleEntities(view), "views start without isolation");
    expect(rendering.setViewVisibleEntities(view, std::vector<RenderObjectId>{9, 2, 9, 4}),
           "a created view accepts an isolation set");
    expect(rendering.viewVisibleEntities(view) == std::vector<RenderObjectId>{2, 4, 9},
           "isolation sets should be sorted and deduplicated");
    expect(rendering.setViewVisibleEntities(rendering.defaultView(), std::vector<RenderObjectId>{}) &&
               rendering.viewVisibleEntities(rendering.defaultView())->empty(),
           "the default view accepts an empty isolation set, which draws nothing");
    expect(rendering.setViewVisibleEntities(view, std::nullopt) && !rendering.viewVisibleEntities(view),
           "nullopt should leave isolation");
    rendering.setViewVisibleEntities(view, std::vector<RenderObjectId>{1});
    expect(rendering.destroyView(view), "the view is destroyed");
    expect(!rendering.setViewVisibleEntities(view, std::vector<RenderObjectId>{1}) &&
               !rendering.viewVisibleEntities(view),
           "stale handles should be rejected");
    const RenderViewId reused = rendering.createView();
    expect(!rendering.viewVisibleEntities(reused), "a reused slot should not inherit isolation");
    expect(rendering.sceneVisibilityMode() == SceneVisibilityMode::Runtime,
           "the renderer should default to the runtime visibility mode");
}

void testComposeViewSettings() {
    RenderSettings frame;
    frame.shadows = ShadowQuality::CascadedHigh;
    frame.localLights = LocalLightingQuality::Clustered;
    frame.particles = ParticleQuality::High;
    frame.environment = EnvironmentQuality::ImageBasedHigh;
    frame.antialiasing = AntialiasingMode::Taa;
    frame.ambientOcclusion = AmbientOcclusionQuality::HalfResolution;
    const RenderSettings same = composeViewSettings(frame, std::nullopt);
    expect(same.antialiasing == AntialiasingMode::Taa && same.shadows == ShadowQuality::CascadedHigh,
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
    expect(composed.antialiasing == AntialiasingMode::Off && composed.ambientOcclusion == AmbientOcclusionQuality::Off,
           "per-view features follow the view override");
    expect(composed.shadows == ShadowQuality::CascadedHigh && composed.particles == ParticleQuality::High &&
               composed.environment == EnvironmentQuality::ImageBasedHigh &&
               composed.localLights == LocalLightingQuality::Clustered,
           "shared once-per-frame work follows the frame settings");
}

void testOrderRenderedViews() {
    std::vector<uint32_t> views{0, 2, 5};
    orderRenderedViews(views, 5);
    expect((views == std::vector<uint32_t>{5, 0, 2}), "the active view leads, the rest keep slot order");
    views = {0, 2, 5};
    orderRenderedViews(views, 3);
    expect((views == std::vector<uint32_t>{0, 2, 5}), "an active view that does not render changes nothing");
    views = {0};
    orderRenderedViews(views, 0);
    expect((views == std::vector<uint32_t>{0}), "a single view stays alone");
}

void testRemapLocalShadowTiles() {
    const std::vector<LocalLight> tiled{{.stableId = 10}, {.stableId = 20}, {.stableId = 30}};
    const std::vector<int32_t> tiles{0, -1, 6};
    const std::vector<LocalLight> viewLights{{.stableId = 30}, {.stableId = 40}, {.stableId = 20}, {.stableId = 10}};
    const std::vector<int32_t> remapped = remapLocalShadowTiles(tiled, tiles, viewLights);
    expect((remapped == std::vector<int32_t>{6, -1, -1, 0}), "shadow tiles follow lights by stable id");
    expect(remapLocalShadowTiles(tiled, tiles, {}).empty(), "a view without lights gets no tiles");
}
} // namespace

UiImage solidImage(uint32_t width, uint32_t height) {
    return {.width = width,
            .height = height,
            .rgba = std::vector<uint8_t>(static_cast<size_t>(width) * height * 4u, 255),
            .filter = UiImageFilter::Nearest};
}

void testUiImages() {
    RenderRuntime rendering;
    expect(!rendering.createUiImage({}), "an empty UI image is rejected");
    UiImage mismatched = solidImage(2, 2);
    mismatched.rgba.pop_back();
    expect(!rendering.createUiImage(mismatched), "a UI image whose pixels do not match its size is rejected");

    const UiImageId first = rendering.createUiImage(solidImage(4, 2));
    expect(first && rendering.uiImageAlive(first), "a valid UI image is created");
    expect(!rendering.uiImageTexture(first), "no UI image texture before the renderer publishes one");
    expect(rendering.updateUiImage(first, solidImage(8, 8)), "a live UI image can be updated");
    expect(!rendering.updateUiImage(first, {}), "an invalid update is rejected");

    expect(rendering.destroyUiImage(first), "a UI image can be destroyed");
    expect(!rendering.uiImageAlive(first) && !rendering.destroyUiImage(first), "a destroyed UI image is stale");
    expect(!rendering.updateUiImage(first, solidImage(1, 1)), "a stale UI image handle is rejected");
    const UiImageId reused = rendering.createUiImage(solidImage(1, 1));
    expect(reused.index == first.index && reused.generation != first.generation,
           "a freed UI image slot is reused with a new generation");
    expect(!rendering.uiImageAlive(first), "the old UI image handle stays stale after reuse");
}

void testSecondaryScenes() {
    RenderRuntime rendering;
    const RenderViewId view = rendering.createView();
    expect(!rendering.viewScene(view), "a view draws the application's scene by default");

    const RenderSceneId first = rendering.createScene();
    const RenderSceneId second = rendering.createScene();
    expect(first && second && !(first == second) && rendering.sceneAlive(first) && rendering.sceneAlive(second),
           "created scenes are distinct and alive");
    expect(rendering.setViewScene(view, first) && rendering.viewScene(view) == first, "a created view binds a scene");
    expect(!rendering.setViewScene(rendering.defaultView(), first) && !rendering.viewScene(rendering.defaultView()),
           "the default view always draws the application's scene");
    expect(rendering.setViewScene(view, {}) && !rendering.viewScene(view),
           "the empty handle returns a view to the application's scene");

    expect(rendering.destroyScene(first) && !rendering.sceneAlive(first), "a scene can be destroyed");
    expect(!rendering.destroyScene(first), "destroying a scene twice is rejected");
    expect(!rendering.setViewScene(view, first) && !rendering.setSceneContent(first, nullptr),
           "a destroyed scene's handle is rejected");
    const RenderSceneId reused = rendering.createScene();
    expect(reused && reused.index == first.index && reused.generation != first.generation,
           "a reused scene slot gets a new generation");

    rendering.setViewScene(view, second);
    rendering.destroyView(view);
    const RenderViewId recreated = rendering.createView();
    expect(recreated.index == view.index && !rendering.viewScene(recreated),
           "a view created in a reused slot draws the application's scene");

    std::vector<RenderSceneId> scenes{reused, second};
    while (const RenderSceneId scene = rendering.createScene()) {
        scenes.push_back(scene);
    }
    expect(scenes.size() == RenderRuntime::maximumScenes, "maximumScenes scenes can be alive at once");
}

int main() {
    testDefaultView();
    testCreateAndDestroy();
    testViewCapacity();
    testActiveView();
    testViewRequests();
    testViewIsolation();
    testComposeViewSettings();
    testOrderRenderedViews();
    testRemapLocalShadowTiles();
    testUiImages();
    testSecondaryScenes();
    if (failures != 0) {
        std::cerr << failures << " render view test(s) failed\n";
        return 1;
    }
    std::cout << "Render view tests passed\n";
    return 0;
}
