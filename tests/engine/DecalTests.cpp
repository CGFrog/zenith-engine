#include "engine/assets/Assets.hpp"
#include "engine/graphics/Decals.hpp"
#include "engine/graphics/RenderConfiguration.hpp"
#include "engine/graphics/passes/DecalClusterPass.hpp"
#include "engine/scene/SceneDocument.hpp"

#include "support/Doctest.hpp"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

TEST_SUITE_BEGIN("Decal");

namespace {

bool near(float actual, float expected, float tolerance = 1.0e-4f) {
    return std::abs(actual - expected) <= tolerance;
}

bool near(const glm::vec3& actual, const glm::vec3& expected, float tolerance = 1.0e-4f) {
    return glm::all(glm::lessThanEqual(glm::abs(actual - expected), glm::vec3(tolerance)));
}

constexpr float nearPlane = 0.1f;
constexpr float farPlane = 200.0f;
const glm::uvec2 renderExtent{960, 540};

// Matches the main-view projection in MeshPass: reversed Z and Vulkan Y.
glm::mat4 sceneProjection() {
    glm::mat4 projection = glm::perspectiveRH_ZO(
        glm::radians(60.0f), static_cast<float>(renderExtent.x) / static_cast<float>(renderExtent.y), farPlane,
        nearPlane);
    projection[1][1] *= -1.0f;
    return projection;
}

DecalProjector projector(const DecalComponent& decal, const glm::mat4& world) {
    const std::optional<DecalProjector> result = makeDecalProjector(decal, world);
    CHECK_MESSAGE(result.has_value(), "valid decal boxes make a projector");
    return result.value_or(DecalProjector{});
}

TEST_CASE("Decal: projection") {
    // A floor decal turned 90 degrees about Y and stretched along its local X
    // by the entity scale: local +X maps to world -Z, local +Z to world +X.
    const glm::mat4 world = glm::scale(
        glm::rotate(glm::translate(glm::mat4(1.0f), {2.0f, 0.0f, -1.0f}), glm::half_pi<float>(), {0.0f, 1.0f, 0.0f}),
        {2.0f, 1.0f, 1.0f});
    const DecalProjector decal = projector(DecalComponent{.halfExtents = {1.0f, 0.5f, 0.75f}}, world);
    CHECK_MESSAGE(near(decalSpacePosition(decal, {2.0f, 0.0f, -1.0f}), glm::vec3(0.0f)),
                  "the box center is the origin");
    const std::optional<glm::vec2> center = decalUv(decal, {2.0f, 0.0f, -1.0f});
    CHECK_MESSAGE((center && near(center->x, 0.5f) && near(center->y, 0.5f)), "the center samples the texture center");
    // Local x = 1 sits half extent times scale (2 m) along world -Z.
    const std::optional<glm::vec2> uEdge = decalUv(decal, {2.0f, 0.0f, -2.999f});
    CHECK_MESSAGE((uEdge && near(uEdge->x, 1.0f, 1.0e-3f) && near(uEdge->y, 0.5f)),
                  "u grows along the decal's local +X");
    const std::optional<glm::vec2> vEdge = decalUv(decal, {2.0f + 0.75f * 0.5f, 0.0f, -1.0f});
    CHECK_MESSAGE((vEdge && near(vEdge->x, 0.5f) && near(vEdge->y, 0.75f)), "v grows along the decal's local +Z");
    CHECK_MESSAGE(!decalUv(decal, {2.0f, 0.0f, -3.1f}), "points past the scaled box are outside");
    CHECK_MESSAGE(!decalUv(decal, {2.0f, 0.6f, -1.0f}), "points above the box depth are outside");
    CHECK_MESSAGE(decalUv(decal, {2.0f, 0.45f, -1.0f}).has_value(), "points within the box depth are inside");
    CHECK_MESSAGE(near(decalFacingAxis(decal), {0.0f, 1.0f, 0.0f}), "an unrotated decal faces floors");
    CHECK_MESSAGE(near(decal.boundingRadius, 2.0f + 0.5f + 0.75f), "the bounding radius covers the scaled axes");

    // Turned +90 degrees about X, local -Y points into a wall facing +Z.
    const DecalProjector wall =
        projector(DecalComponent{}, glm::rotate(glm::mat4(1.0f), glm::half_pi<float>(), {1.0f, 0.0f, 0.0f}));
    CHECK_MESSAGE(near(decalFacingAxis(wall), {0.0f, 0.0f, 1.0f}), "a pitched decal faces the wall it projects onto");
    const std::optional<glm::vec2> top = decalUv(wall, {0.0f, 0.4f, 0.0f});
    CHECK_MESSAGE((top && top->y < 0.5f), "the image's top stays up on a wall decal");

    CHECK_MESSAGE(!makeDecalProjector(DecalComponent{.halfExtents = {1.0f, 0.0f, 1.0f}}, glm::mat4(1.0f)),
                  "flat boxes have no projector");
    CHECK_MESSAGE(!makeDecalProjector(DecalComponent{}, glm::scale(glm::mat4(1.0f), {1.0f, 1.0f, 0.0f})),
                  "zero-scaled entities have no projector");
}

TEST_CASE("Decal: fades") {
    const DecalProjector decal = projector(DecalComponent{}, glm::mat4(1.0f));
    const auto tilted = [](float degrees) {
        const float radians = glm::radians(degrees);
        return glm::vec3(std::sin(radians), std::cos(radians), 0.0f);
    };
    CHECK_MESSAGE(near(decalAngleFade(decal, {0.0f, 1.0f, 0.0f}), 1.0f),
                  "surfaces facing the projector are fully covered");
    CHECK_MESSAGE(near(decalAngleFade(decal, tilted(60.0f)), 1.0f, 1.0e-3f), "full coverage reaches the start angle");
    const float expected = (std::cos(glm::radians(70.0f)) - std::cos(glm::radians(80.0f))) /
                           (std::cos(glm::radians(60.0f)) - std::cos(glm::radians(80.0f)));
    CHECK_MESSAGE(near(decalAngleFade(decal, tilted(70.0f)), expected, 1.0e-3f), "the fade is linear in the cosine");
    CHECK_MESSAGE(near(decalAngleFade(decal, tilted(80.0f)), 0.0f, 1.0e-3f), "coverage ends at the end angle");
    CHECK_MESSAGE(decalAngleFade(decal, {1.0f, 0.0f, 0.0f}) == 0.0f, "perpendicular walls stay clean");
    CHECK_MESSAGE(decalAngleFade(decal, {0.0f, -1.0f, 0.0f}) == 0.0f, "back faces stay clean");

    const DecalProjector hard = projector(
        DecalComponent{.angleFadeStartRadians = glm::radians(45.0f), .angleFadeEndRadians = glm::radians(45.0f)},
        glm::mat4(1.0f));
    CHECK_MESSAGE((decalAngleFade(hard, tilted(44.0f)) == 1.0f && decalAngleFade(hard, tilted(46.0f)) == 0.0f),
                  "equal fade angles give a hard edge");

    CHECK_MESSAGE((decalDepthFade(0.0f) == 1.0f && near(decalDepthFade(0.9f), 0.4f) && decalDepthFade(1.0f) == 0.0f &&
                   near(decalDepthFade(-0.9f), 0.4f)),
                  "the depth fade spans the outer quarter of the box depth");

    const DecalProjector faded = projector(DecalComponent{.opacity = 0.5f}, glm::mat4(1.0f));
    CHECK_MESSAGE(near(decalCoverage(faded, {0.1f, 0.0f, 0.2f}, {0.0f, 1.0f, 0.0f}), 0.5f),
                  "coverage scales by opacity");
    CHECK_MESSAGE(decalCoverage(faded, {0.1f, 0.2f, 0.2f}, {1.0f, 0.0f, 0.0f}) == 0.0f,
                  "a perpendicular surface inside the box receives nothing");
    CHECK_MESSAGE(decalCoverage(faded, {0.6f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}) == 0.0f,
                  "points outside receive nothing");
}

TEST_CASE("Decal: gather and select") {
    Scene scene;
    const auto add = [&](const char* name, int32_t sortOrder, const glm::vec3& position,
                         const glm::vec3& halfExtents = glm::vec3(0.5f)) {
        const Entity entity = scene.createEntity(name);
        scene.registry().get<TransformComponent>(entity).local = glm::translate(glm::mat4(1.0f), position);
        scene.registry().emplace<DecalComponent>(entity,
                                                 DecalComponent{.halfExtents = halfExtents, .sortOrder = sortOrder});
        return entity;
    };
    const Entity late = add("late", 2, {0.0f, 0.0f, 0.0f});
    const Entity firstEarly = add("early-a", -1, {10.0f, 0.0f, 0.0f});
    const Entity secondEarly = add("early-b", -1, {20.0f, 0.0f, 0.0f});
    add("flat", 0, {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 1.0f});
    scene.updateWorldTransforms();
    const std::vector<DecalProjector> decals = gatherDecals(scene);
    CHECK_MESSAGE(decals.size() == 3, "degenerate decals are skipped");
    CHECK_MESSAGE((decals.size() == 3 && decals[0].entity == firstEarly && decals[1].entity == secondEarly &&
                   decals[2].entity == late),
                  "decals apply in sort order, then entity order");

    const std::vector<DecalProjector> nearest = selectDecals(decals, {19.0f, 0.0f, 0.0f}, 2);
    CHECK_MESSAGE((nearest.size() == 2 && nearest[0].entity == firstEarly && nearest[1].entity == secondEarly),
                  "selection keeps the nearest decals in application order");
    CHECK_MESSAGE(selectDecals(decals, glm::vec3(0.0f), 8).size() == 3, "selection keeps every decal under the limit");
}

TEST_CASE("Decal: cluster assignment") {
    const ClusterGrid grid =
        makeClusterGrid(ClusterGridConfiguration{}, sceneProjection(), renderExtent, nearPlane, farPlane);
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 3.0f, 6.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));

    // The view bounds hold every box corner.
    const glm::mat4 world = glm::rotate(glm::translate(glm::mat4(1.0f), {1.0f, 0.2f, -2.0f}), 0.7f,
                                        glm::normalize(glm::vec3(1.0f, 2.0f, 0.5f)));
    const DecalComponent component{.halfExtents = {1.5f, 0.3f, 0.8f}};
    const DecalProjector decal = projector(component, world);
    const ClusterBounds bounds = decalViewBounds(decal, view);
    bool containsCorners = true;
    for (int corner = 0; corner < 8; ++corner) {
        const glm::vec3 local{corner & 1 ? 1.0f : -1.0f, corner & 2 ? 1.0f : -1.0f, corner & 4 ? 1.0f : -1.0f};
        const glm::vec3 viewPoint = glm::vec3(view * world * glm::vec4(local * component.halfExtents, 1.0f));
        containsCorners = containsCorners && glm::all(glm::lessThanEqual(bounds.minimum, viewPoint + 1.0e-4f)) &&
                          glm::all(glm::lessThanEqual(viewPoint - 1.0e-4f, bounds.maximum));
    }
    CHECK_MESSAGE(containsCorners, "view bounds contain every corner of the box");

    // Overflow: more small decals in one spot than a cluster lists.
    std::vector<ClusterBounds> crowded;
    for (uint32_t index = 0; index < maximumDecalsPerCluster + 9; ++index) {
        const DecalProjector small = projector(DecalComponent{.halfExtents = glm::vec3(0.05f)}, glm::mat4(1.0f));
        crowded.push_back(decalViewBounds(small, view));
    }
    crowded.push_back(bounds);
    const ClusterDecalLists lists = assignDecalsToClusters(grid, crowded);
    CHECK_MESSAGE(lists.maximumDecalsInCluster >= maximumDecalsPerCluster + 9, "the busiest cluster count is reported");
    CHECK_MESSAGE(lists.discardedAssignments > 0, "assignments beyond a cluster's capacity are counted");
    bool matchesDirect = true;
    bool sortedAndCapped = true;
    uint32_t expectedDiscarded = 0;
    for (uint32_t slice = 0; slice < grid.configuration.depthSlices; ++slice) {
        for (uint32_t row = 0; row < grid.configuration.rows; ++row) {
            for (uint32_t column = 0; column < grid.configuration.columns; ++column) {
                const uint32_t cluster = grid.clusterIndex(column, row, slice);
                std::vector<uint32_t> expected;
                for (uint32_t index = 0; index < crowded.size(); ++index) {
                    if (decalAffectsCluster(crowded[index], grid.bounds(column, row, slice))) {
                        expected.push_back(index);
                    }
                }
                const uint32_t stored =
                    std::min<uint32_t>(static_cast<uint32_t>(expected.size()), maximumDecalsPerCluster);
                expectedDiscarded += static_cast<uint32_t>(expected.size()) - stored;
                matchesDirect = matchesDirect && lists.counts[cluster] == stored;
                for (uint32_t entry = 0; entry < lists.counts[cluster] && entry < expected.size(); ++entry) {
                    sortedAndCapped =
                        sortedAndCapped && lists.indices[cluster * maximumDecalsPerCluster + entry] == expected[entry];
                }
            }
        }
    }
    CHECK_MESSAGE(matchesDirect, "cluster counts match the per-cluster overlap test");
    CHECK_MESSAGE(sortedAndCapped, "each cluster lists the first decals in application order");
    CHECK_MESSAGE(lists.discardedAssignments == expectedDiscarded,
                  "the discarded count covers every overflowing cluster");
}

TEST_CASE("Decal: packing") {
    const glm::mat4 world = glm::rotate(glm::translate(glm::mat4(1.0f), {3.0f, 1.0f, 2.0f}), 0.4f, {0.0f, 1.0f, 0.0f});
    const DecalProjector decal = projector(
        DecalComponent{.halfExtents = {0.5f, 0.2f, 1.0f}, .tint = {1.0f, 0.5f, 0.25f}, .opacity = 0.75f}, world);
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 2.0f, 8.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const GpuDecal packed = packDecal(decal, {.albedo = 7u}, view);
    const glm::vec4 point{3.2f, 1.1f, 2.3f, 1.0f};
    const glm::vec3 viaRows{glm::dot(packed.worldToDecal[0], point), glm::dot(packed.worldToDecal[1], point),
                            glm::dot(packed.worldToDecal[2], point)};
    CHECK_MESSAGE(near(viaRows, decalSpacePosition(decal, glm::vec3(point))), "packed rows reproduce decal space");
    CHECK_MESSAGE((packed.textures.x == 7u && packed.textures.z == decalAlbedoFlag),
                  "only resolved textures are flagged");
    CHECK_MESSAGE((near(glm::vec3(packed.tintOpacity), {1.0f, 0.5f, 0.25f}) && near(packed.tintOpacity.w, 0.75f)),
                  "tint and opacity are packed");
    CHECK_MESSAGE((near(packed.blend.z, decal.angleFadeScale) && near(packed.blend.w, decal.angleFadeBias)),
                  "the angle fade is packed as scale and bias");
    const ClusterBounds bounds = decalViewBounds(decal, view);
    CHECK_MESSAGE(
        (near(glm::vec3(packed.viewMinimum), bounds.minimum) && near(glm::vec3(packed.viewMaximum), bounds.maximum)),
        "view bounds are packed for the cluster pass");
    static_assert(offsetof(GpuDecal, tintOpacity) == 48 && offsetof(GpuDecal, textures) == 96 &&
                      offsetof(GpuDecal, viewMaximum) == 128,
                  "GpuDecal must match DecalRecord in mesh.frag");
}

TEST_CASE("Decal: serialization") {
    const std::filesystem::path assetRoot = ZENITH_ASSET_DIR;
    const SceneDocumentOptions options{.assetRoot = assetRoot};
    AssetManager assets;
    const AssetLoadResult<TextureHandle> albedo = assets.loadTexture(assetRoot / "showcase/decals/decal_plate.png");
    const AssetLoadResult<TextureHandle> normal =
        assets.loadTexture(assetRoot / "showcase/decals/decal_plate_normal.png");
    CHECK_MESSAGE((static_cast<bool>(albedo) && static_cast<bool>(normal)), "the shipped decal textures load");

    Scene scene;
    const Entity entity = scene.createEntity("Plate");
    const DecalComponent original{.halfExtents = {0.4f, 0.1f, 0.6f},
                                  .albedoAsset = "showcase/decals/decal_plate.png",
                                  .albedoTexture = albedo.handle,
                                  .normalAsset = "showcase/decals/decal_plate_normal.png",
                                  .normalTexture = normal.handle,
                                  .tint = {0.9f, 0.8f, 0.7f},
                                  .opacity = 0.8f,
                                  .baseColorStrength = 0.6f,
                                  .normalStrength = 0.9f,
                                  .roughness = 0.3f,
                                  .roughnessStrength = 0.7f,
                                  .metallic = 1.0f,
                                  .metallicStrength = 0.5f,
                                  .angleFadeStartRadians = 0.5f,
                                  .angleFadeEndRadians = 1.2f,
                                  .sortOrder = -3};
    scene.registry().emplace<DecalComponent>(entity, original);
    std::string document;
    const SceneDocumentResult saved = SceneDocument::serialize(scene, assets, options, document);
    CHECK_MESSAGE(saved.success, ("decal scenes serialize: " + saved.error));

    Scene restored;
    const SceneDocumentResult loaded = SceneDocument::deserialize(document, assets, options, restored);
    CHECK_MESSAGE(loaded.success, ("decal scenes deserialize: " + loaded.error));
    const DecalComponent* decal = restored.registry().try_get<DecalComponent>(restored.findEntity(scene.id(entity)));
    CHECK_MESSAGE(decal != nullptr, "the decal component round trips");
    if (decal != nullptr) {
        CHECK_MESSAGE((near(decal->halfExtents, original.halfExtents) && near(decal->tint, original.tint) &&
                       near(decal->opacity, original.opacity) && near(decal->baseColorStrength, 0.6f) &&
                       near(decal->normalStrength, 0.9f) && near(decal->roughness, 0.3f) &&
                       near(decal->roughnessStrength, 0.7f) && near(decal->metallic, 1.0f) &&
                       near(decal->metallicStrength, 0.5f) && near(decal->angleFadeStartRadians, 0.5f) &&
                       near(decal->angleFadeEndRadians, 1.2f) && decal->sortOrder == -3),
                      "decal parameters round trip");
        CHECK_MESSAGE((decal->albedoAsset == original.albedoAsset && decal->normalAsset == original.normalAsset &&
                       static_cast<bool>(decal->albedoTexture) && static_cast<bool>(decal->normalTexture)),
                      "decal textures round trip as project-relative paths");
    }
    std::string resaved;
    CHECK_MESSAGE((SceneDocument::serialize(restored, assets, options, resaved).success && resaved == document),
                  "a restored decal serializes identically");

    // Only half_extents is required; everything else takes the defaults.
    const std::string minimal =
        R"({"version":1,"entities":[{"id":5,"name":"Decal","parent":null,"local_transform":)"
        R"([1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"components":{"decal":{"half_extents":[1,0.5,1]}}}]})";
    Scene defaults;
    const SceneDocumentResult minimalResult = SceneDocument::deserialize(minimal, assets, options, defaults);
    CHECK_MESSAGE(minimalResult.success, ("minimal decals load: " + minimalResult.error));
    const DecalComponent* defaulted = defaults.registry().try_get<DecalComponent>(defaults.findEntity({5}));
    const DecalComponent reference;
    CHECK_MESSAGE((defaulted != nullptr && !defaulted->albedoTexture && defaulted->opacity == reference.opacity &&
                   defaulted->angleFadeEndRadians == reference.angleFadeEndRadians && defaulted->sortOrder == 0),
                  "missing decal fields take their defaults");

    for (const char* invalid : {R"("half_extents":[1,0,1])", R"("half_extents":[1,1,1],"opacity":2)",
                                R"("half_extents":[1,1,1],"angle_fade_start":1.5,"angle_fade_end":1.0)",
                                R"("half_extents":[1,1,1],"albedo_asset":"showcase/decals/missing.png")"}) {
        const std::string rejected =
            std::string(R"({"version":1,"entities":[{"id":5,"name":"Decal","parent":null,"local_transform":)") +
            R"([1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"components":{"decal":{)" + invalid + "}}}]}";
        Scene target;
        CHECK_MESSAGE(!SceneDocument::deserialize(rejected, assets, options, target).success,
                      (std::string("invalid decals are rejected: ") + invalid));
    }
}

TEST_CASE("Decal: settings") {
    RenderSettings requested;
    CHECK_MESSAGE(requested.decals == DecalQuality::On, "decals are on by default");
    requested.decals = DecalQuality::Off;
    requested.budgets.decalClusters = {0.0, 0.05};
    std::string document;
    CHECK_MESSAGE(serializeRenderSettings(requested, document).success, "settings serialize");
    RenderSettings decoded;
    CHECK_MESSAGE((deserializeRenderSettings(document, decoded).success && decoded.decals == DecalQuality::Off &&
                   decoded.budgets.decalClusters.gpuMilliseconds == 0.05),
                  "the decal setting and its budget round trip");
    const std::string field = "\"decals\": 0,";
    const size_t position = document.find(field);
    CHECK_MESSAGE(position != std::string::npos, "the decal setting is serialized by value");
    if (position != std::string::npos) {
        std::string legacy = document;
        legacy.erase(position, field.size());
        RenderSettings legacySettings;
        CHECK_MESSAGE(
            (deserializeRenderSettings(legacy, legacySettings).success && legacySettings.decals == DecalQuality::On),
            "documents without the field keep decals on");
        std::string invalid = document;
        invalid.replace(position, field.size(), "\"decals\": 9,");
        RenderSettings invalidSettings;
        CHECK_MESSAGE(!deserializeRenderSettings(invalid, invalidSettings).success,
                      "unknown decal qualities are rejected");
    }

    RenderCapabilities capabilities;
    // Isolates decals from the other defaults' capability requirements.
    capabilities.irradianceVolumes = true;
    capabilities.reflectionProbes = true;
    capabilities.perPixelReflectionProbes = true;
    capabilities.decals = false;
    const ValidatedRenderSettings fallback = validateRenderSettings(RenderSettings{}, capabilities);
    CHECK_MESSAGE((fallback.settings.decals == DecalQuality::Off && fallback.fallbacks.size() == 1),
                  "unsupported decals fall back to Off");
    capabilities.decals = true;
    CHECK_MESSAGE(validateRenderSettings(RenderSettings{}, capabilities).fallbacks.empty(),
                  "supported decals are kept");
}
} // namespace

TEST_SUITE_END();
