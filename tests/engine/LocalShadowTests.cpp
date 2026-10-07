#include "engine/graphics/LocalShadows.hpp"
#include "engine/graphics/RenderConfiguration.hpp"
#include "engine/scene/SceneDocument.hpp"

#include "support/Doctest.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

TEST_SUITE_BEGIN("LocalShadow");

namespace {

LocalLight spotLight(uint64_t stableId, const glm::vec3& position) {
    LocalLight light;
    light.type = LocalLightType::Spot;
    light.position = position;
    light.direction = {0.0f, -1.0f, 0.0f};
    light.range = 6.0f;
    light.innerConeCosine = std::cos(0.25f);
    light.outerConeCosine = std::cos(0.45f);
    light.stableId = stableId;
    light.castsShadows = true;
    return light;
}

LocalLight pointLight(uint64_t stableId, const glm::vec3& position) {
    LocalLight light;
    light.type = LocalLightType::Point;
    light.position = position;
    light.range = 4.0f;
    light.stableId = stableId;
    light.castsShadows = true;
    return light;
}

glm::vec3 project(const glm::mat4& viewProjection, const glm::vec3& world) {
    const glm::vec4 clip = viewProjection * glm::vec4(world, 1.0f);
    return glm::vec3(clip) / clip.w;
}

LocalShadowAtlas allocate(std::span<const LocalLight> lights, const LocalShadowProfile& profile) {
    const std::vector<LocalShadowRequest> requests = buildLocalShadowRequests(lights, glm::vec3(0.0f), profile);
    return allocateLocalShadowAtlas(requests, profile.atlasSize, profile.atlasSize);
}

TEST_CASE("LocalShadow: request order") {
    const LocalShadowProfile profile = localShadowProfile(LocalShadowQuality::Medium);
    std::vector<LocalLight> lights{pointLight(1, {0.0f, 1.0f, 0.0f}), spotLight(2, {5.0f, 3.0f, 0.0f}),
                                   pointLight(3, {2.0f, 1.0f, 0.0f}), spotLight(4, {-5.0f, 3.0f, 0.0f})};
    LocalLight unshadowed = spotLight(5, {0.0f, 3.0f, 0.0f});
    unshadowed.castsShadows = false;
    lights.push_back(unshadowed);

    const std::vector<LocalShadowRequest> requests = buildLocalShadowRequests(lights, glm::vec3(0.0f), profile);
    CHECK_MESSAGE(requests.size() == 4, "only shadow-casting lights should request tiles");
    const auto firstPoint = std::ranges::find(requests, LocalLightType::Point, &LocalShadowRequest::type);
    CHECK_MESSAGE(
        (std::all_of(requests.begin(), firstPoint,
                     [](const LocalShadowRequest& request) { return request.type == LocalLightType::Spot; }) &&
         std::all_of(firstPoint, requests.end(),
                     [](const LocalShadowRequest& request) { return request.type == LocalLightType::Point; })),
        "spot lights should be requested before point lights");
    CHECK_MESSAGE(std::ranges::is_sorted(requests, std::ranges::greater{}, &LocalShadowRequest::priority),
                  "request priorities should preserve the request order through the allocator");
    CHECK_MESSAGE((requests.front().resolution == profile.spotResolution &&
                   requests.back().resolution == profile.pointResolution),
                  "lights without a hint should use the quality's tile sizes");

    const LocalShadowAtlas atlas = allocateLocalShadowAtlas(requests, profile.atlasSize, profile.atlasSize);
    CHECK_MESSAGE((atlas.rejectedLights == 0 && atlas.tiles.size() == 2 + 2 * pointShadowFaceCount),
                  "the medium atlas should hold two spots and two point lights");
    CHECK_MESSAGE(atlas.tiles.front().stableId == requests.front().stableId,
                  "the allocator should pack the requests in their order");

    const glm::vec3 farCamera{100.0f, 0.0f, 0.0f};
    CHECK_MESSAGE((buildLocalShadowRequests(lights, farCamera, profile).size() == requests.size() &&
                   std::ranges::equal(buildLocalShadowRequests(lights, farCamera, profile), requests, {},
                                      &LocalShadowRequest::stableId, &LocalShadowRequest::stableId)),
                  "camera motion should not reorder the atlas while the selected set is unchanged");
    CHECK_MESSAGE(
        buildLocalShadowRequests(lights, glm::vec3(0.0f), localShadowProfile(LocalShadowQuality::Off)).empty(),
        "Off should request no tiles");
}

TEST_CASE("LocalShadow: request selection") {
    LocalShadowProfile profile = localShadowProfile(LocalShadowQuality::Low);
    profile.maximumLights = 2;
    std::vector<LocalLight> lights{spotLight(1, {50.0f, 0.0f, 0.0f}), spotLight(2, {1.0f, 0.0f, 0.0f}),
                                   spotLight(3, {60.0f, 0.0f, 0.0f}), pointLight(4, {2.0f, 0.0f, 0.0f})};
    lights[2].shadowPriority = 5;
    lights[0].shadowResolution = 4096;
    lights[1].shadowResolution = 16;
    std::vector<LocalShadowRequest> requests = buildLocalShadowRequests(lights, glm::vec3(0.0f), profile);
    std::vector<uint64_t> ids;
    for (const LocalShadowRequest& request : requests) {
        ids.push_back(request.stableId);
    }
    std::ranges::sort(ids);
    CHECK_MESSAGE((ids == std::vector<uint64_t>{2, 3}),
                  "selection should keep priority first, then the nearest lights");
    const auto light2 = std::ranges::find(requests, uint64_t{2}, &LocalShadowRequest::stableId);
    CHECK_MESSAGE((light2 != requests.end() && light2->resolution == 64),
                  "small resolution hints should clamp to 64 texels");
    profile.maximumLights = 4;
    requests = buildLocalShadowRequests(lights, glm::vec3(0.0f), profile);
    const auto light1 = std::ranges::find(requests, uint64_t{1}, &LocalShadowRequest::stableId);
    CHECK_MESSAGE((light1 != requests.end() && light1->resolution == profile.maximumResolution),
                  "large resolution hints should clamp to the quality maximum");
}

TEST_CASE("LocalShadow: face math") {
    const LocalLight point = pointLight(1, {1.0f, 2.0f, 3.0f});
    const glm::vec3 axes[pointShadowFaceCount]{{1.0f, 0.0f, 0.0f},  {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
                                               {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},  {0.0f, 0.0f, -1.0f}};
    for (uint32_t face = 0; face < pointShadowFaceCount; ++face) {
        CHECK_MESSAGE(localShadowFace(axes[face] * 2.0f) == face,
                      ("the major axis should select face " + std::to_string(face)));
        const glm::vec3 projected = project(localShadowViewProjection(point, face), point.position + axes[face] * 2.0f);
        CHECK_MESSAGE((std::abs(projected.x) < 1e-4f && std::abs(projected.y) < 1e-4f && projected.z > 0.0f &&
                       projected.z < 1.0f),
                      "a face's axis should project to the tile center inside the depth range");
        const glm::vec3 atRange =
            project(localShadowViewProjection(point, face), point.position + axes[face] * point.range);
        CHECK_MESSAGE(std::abs(atRange.z - 1.0f) < 1e-4f, "the light range should map to depth 1");
    }
    // Every direction lands inside the tile of the face it selects, with the
    // field-of-view margin left for filtering.
    for (int x = -3; x <= 3; ++x) {
        for (int y = -3; y <= 3; ++y) {
            for (int z = -3; z <= 3; ++z) {
                const glm::vec3 offset(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z) + 0.1f);
                const uint32_t face = localShadowFace(offset);
                const glm::vec3 projected =
                    project(localShadowViewProjection(point, face), point.position + glm::normalize(offset));
                CHECK_MESSAGE((std::abs(projected.x) <= 1.0f / 1.04f && std::abs(projected.y) <= 1.0f / 1.04f),
                              "a selected face should contain its direction with a filtering margin");
            }
        }
    }

    const LocalLight spot = spotLight(2, {0.0f, 4.0f, 0.0f});
    const glm::mat4 spotProjection = localShadowViewProjection(spot, 0);
    const glm::vec3 center = project(spotProjection, {0.0f, 1.0f, 0.0f});
    CHECK_MESSAGE((std::abs(center.x) < 1e-4f && std::abs(center.y) < 1e-4f),
                  "the spot axis should hit the tile center");
    const float coneAngle = std::acos(spot.outerConeCosine);
    const glm::vec3 coneEdge =
        project(spotProjection, spot.position + glm::vec3(std::sin(coneAngle), -std::cos(coneAngle), 0.0f));
    CHECK_MESSAGE((std::abs(coneEdge.x) < 1.0f && std::abs(coneEdge.x) > 0.9f),
                  "the outer cone should fill the tile up to its filtering margin");
    CHECK_MESSAGE(localShadowTanHalfFov(spot) > std::tan(coneAngle), "the spot tile should cover the whole cone");
}

TEST_CASE("LocalShadow: cache invalidation") {
    const LocalShadowProfile profile = localShadowProfile(LocalShadowQuality::Medium);
    std::vector<LocalLight> lights{spotLight(1, {0.0f, 3.0f, 0.0f}), pointLight(2, {10.0f, 1.0f, 0.0f})};
    LocalShadowCache cache;
    cache.synchronize(allocate(lights, profile), lights, {}, false);
    CHECK_MESSAGE((!cache.shadowTile(1) && !cache.shadowTile(2)), "new tiles should start unshadowed");
    CHECK_MESSAGE(cache.pendingTileCount() == 7, "every new tile should be pending");
    std::vector<uint64_t> scheduled = cache.scheduleUpdates(glm::vec3(0.0f), 64);
    CHECK_MESSAGE(scheduled.size() == 2, "both lights should be scheduled within a large budget");
    for (uint64_t light : scheduled) {
        cache.markRendered(light);
    }
    CHECK_MESSAGE((cache.shadowTile(1) == 0u && cache.shadowTile(2) == 1u), "rendered lights should be shadowed");
    CHECK_MESSAGE(cache.pendingTileCount() == 0, "rendered tiles should be clean");

    cache.synchronize(allocate(lights, profile), lights, {}, false);
    CHECK_MESSAGE(cache.scheduleUpdates(glm::vec3(0.0f), 64).empty(), "an unchanged frame should reuse every tile");

    const ShadowCasterBounds farCaster{{40.0f, 0.0f, 0.0f}, 1.0f};
    cache.synchronize(allocate(lights, profile), lights, {&farCaster, 1}, false);
    CHECK_MESSAGE(cache.scheduleUpdates(glm::vec3(0.0f), 64).empty(),
                  "a caster outside every light's range changes nothing");

    const ShadowCasterBounds nearCaster{{0.0f, 0.5f, 0.0f}, 0.5f};
    cache.synchronize(allocate(lights, profile), lights, {&nearCaster, 1}, false);
    scheduled = cache.scheduleUpdates(glm::vec3(0.0f), 64);
    CHECK_MESSAGE((scheduled == std::vector<uint64_t>{1}),
                  "a caster inside the spot's range should stale only the spot");
    CHECK_MESSAGE(cache.shadowTile(1).has_value(), "a stale light should keep shading with its previous depth");
    cache.markRendered(1);

    const glm::mat4 before = cache.tiles()[*cache.firstTile(2)].renderedViewProjection;
    lights[1].position.x += 0.5f;
    cache.synchronize(allocate(lights, profile), lights, {}, false);
    scheduled = cache.scheduleUpdates(glm::vec3(0.0f), 64);
    CHECK_MESSAGE((scheduled == std::vector<uint64_t>{2}), "a moved light should be re-rendered");
    CHECK_MESSAGE(cache.tiles()[*cache.firstTile(2)].renderedViewProjection == before,
                  "a moved light should shade with the projection its depth was rendered with until redrawn");
    cache.markRendered(2);

    cache.synchronize(allocate(lights, profile), lights, {}, true);
    CHECK_MESSAGE(cache.scheduleUpdates(glm::vec3(0.0f), 64).size() == 2,
                  "invalidating casters should stale every tile");
    cache.markRendered(1);
    cache.markRendered(2);

    // A new spot packs ahead of the point light and moves its tiles.
    lights.push_back(spotLight(3, {-10.0f, 3.0f, 0.0f}));
    cache.synchronize(allocate(lights, profile), lights, {}, false);
    CHECK_MESSAGE(cache.shadowTile(1).has_value(), "tiles that kept their placement should keep their depth");
    CHECK_MESSAGE((!cache.shadowTile(2) && !cache.shadowTile(3)),
                  "moved and new tiles should be unshadowed until rendered");
    cache.invalidate(1);
    CHECK_MESSAGE(!cache.shadowTile(1), "an invalidated light should lose its shadow");
}

TEST_CASE("LocalShadow: update budget") {
    const LocalShadowProfile profile = localShadowProfile(LocalShadowQuality::Medium);
    std::vector<LocalLight> lights{spotLight(1, {30.0f, 3.0f, 0.0f}), spotLight(2, {5.0f, 3.0f, 0.0f}),
                                   spotLight(3, {15.0f, 3.0f, 0.0f}), pointLight(4, {0.0f, 1.0f, 0.0f})};
    LocalShadowCache cache;
    cache.synchronize(allocate(lights, profile), lights, {}, false);
    std::vector<uint64_t> scheduled = cache.scheduleUpdates(glm::vec3(0.0f), 2);
    CHECK_MESSAGE((scheduled == std::vector<uint64_t>({2, 3})),
                  "lights that do not fit should leave room for smaller ones");
    scheduled = cache.scheduleUpdates(glm::vec3(0.0f), 8);
    CHECK_MESSAGE((scheduled == std::vector<uint64_t>({2, 4, 3})), "whole lights should fill the budget nearest first");
    for (uint64_t light : scheduled) {
        cache.markRendered(light);
    }
    CHECK_MESSAGE(cache.pendingTileCount() == 1, "the light beyond the budget should stay pending");

    const ShadowCasterBounds everywhere{{0.0f, 0.0f, 0.0f}, 100.0f};
    cache.synchronize(allocate(lights, profile), lights, {&everywhere, 1}, false);
    scheduled = cache.scheduleUpdates(glm::vec3(0.0f), 2);
    CHECK_MESSAGE((scheduled == std::vector<uint64_t>({1, 2})),
                  "lights without any depth should come before stale ones, then the nearest");

    const std::vector<LocalLight> onlyPoint{pointLight(5, {0.0f, 1.0f, 0.0f})};
    LocalShadowCache pointCache;
    pointCache.synchronize(allocate(onlyPoint, profile), onlyPoint, {}, false);
    CHECK_MESSAGE((pointCache.scheduleUpdates(glm::vec3(0.0f), 2) == std::vector<uint64_t>{5}),
                  "the first light is always scheduled so a small budget cannot starve point lights");
}

TEST_CASE("LocalShadow: settings") {
    RenderSettings settings;
    settings.localShadows = LocalShadowQuality::High;
    settings.budgets.localShadows = {0.1, 0.4};
    std::string document;
    CHECK_MESSAGE(serializeRenderSettings(settings, document).success, "settings should serialize");
    RenderSettings decoded;
    CHECK_MESSAGE((deserializeRenderSettings(document, decoded).success &&
                   decoded.localShadows == LocalShadowQuality::High &&
                   decoded.budgets.localShadows.gpuMilliseconds == 0.4),
                  "local-shadow quality and budget should round trip");
    RenderSettings legacy;
    CHECK_MESSAGE(
        (deserializeRenderSettings(R"({"version": 1, "antialiasing": 1, "shadows": 1, "lighting": 1, "bloom": 0,
                                          "environment": 0, "localLights": 1})",
                                   legacy)
             .success &&
         legacy.localShadows == RenderSettings{}.localShadows),
        "documents without local shadows should keep the default");
    RenderSettings invalid;
    CHECK_MESSAGE(
        !deserializeRenderSettings(R"({"version": 1, "antialiasing": 1, "shadows": 1, "lighting": 1, "bloom": 0,
                                           "environment": 0, "localLights": 1, "localShadows": 9})",
                                   invalid)
             .success,
        "unknown local-shadow qualities should be rejected");

    RenderCapabilities capabilities;
    capabilities.maximumLocalLights = 0;
    const ValidatedRenderSettings validated = validateRenderSettings(settings, capabilities);
    CHECK_MESSAGE((validated.settings.localShadows == LocalShadowQuality::Off &&
                   std::ranges::any_of(validated.fallbacks,
                                       [](const std::string& text) { return text.find("Local-light shadows") == 0; })),
                  "local shadows should fall back to Off without clustered local lighting");
}

TEST_CASE("LocalShadow: scene document") {
    AssetManager assets;
    const SceneDocumentOptions options{};
    Scene scene;
    const Entity spot = scene.createEntity("Spot");
    SpotLightComponent spotLightComponent;
    spotLightComponent.castsShadows = true;
    spotLightComponent.shadowResolution = 512;
    spotLightComponent.shadowPriority = 3;
    scene.registry().emplace<SpotLightComponent>(spot, spotLightComponent);
    const Entity point = scene.createEntity("Point");
    scene.registry().emplace<PointLightComponent>(point, PointLightComponent{.castsShadows = true});
    std::string document;
    CHECK_MESSAGE(SceneDocument::serialize(scene, assets, options, document).success, "lights should serialize");
    Scene restored;
    const SceneDocumentResult loaded = SceneDocument::deserialize(document, assets, options, restored);
    CHECK_MESSAGE(loaded.success, ("lights should load: " + loaded.error));
    const auto* restoredSpot = restored.registry().try_get<SpotLightComponent>(restored.findEntity(scene.id(spot)));
    const auto* restoredPoint = restored.registry().try_get<PointLightComponent>(restored.findEntity(scene.id(point)));
    CHECK_MESSAGE((restoredSpot && restoredSpot->castsShadows && restoredSpot->shadowResolution == 512 &&
                   restoredSpot->shadowPriority == 3),
                  "spot shadow fields should round trip");
    CHECK_MESSAGE((restoredPoint && restoredPoint->castsShadows && restoredPoint->shadowResolution == 0),
                  "point shadow fields should round trip");
}
} // namespace

TEST_SUITE_END();
