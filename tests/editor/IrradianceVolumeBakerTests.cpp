#include "editor/project/IrradianceVolumeBaker.hpp"

#include "engine/assets/Assets.hpp"
#include "engine/graphics/EnvironmentLighting.hpp"

#include "support/Doctest.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <vector>

TEST_SUITE_BEGIN("IrradianceVolumeBaker");

namespace {

glm::vec3 evaluate(const IrradianceProbe& probe, const glm::vec3& normal) {
    environment_lighting::IrradianceCoefficients coefficients;
    std::copy(probe.irradiance.begin(), probe.irradiance.end(), coefficients.begin());
    return environment_lighting::evaluateIrradiance(coefficients, normal);
}

// Two triangles covering an axis-aligned rectangle, front face toward `normal`.
void addQuad(std::vector<IrradianceBakeTriangle>& triangles, const glm::vec3& center, const glm::vec3& axisU,
             const glm::vec3& axisV, const glm::vec3& albedo, bool doubleSided = false) {
    const glm::vec3 a = center - axisU - axisV;
    const glm::vec3 b = center + axisU - axisV;
    const glm::vec3 c = center + axisU + axisV;
    const glm::vec3 d = center - axisU + axisV;
    triangles.push_back({.a = a, .b = b, .c = c, .albedo = albedo, .doubleSided = doubleSided});
    triangles.push_back({.a = a, .b = c, .c = d, .albedo = albedo, .doubleSided = doubleSided});
}

// A closed 4 x 3 x 4 room, faces pointing inward, with a red floor, white
// walls, and a 1 x 1 hole in the ceiling the sun shines through.
IrradianceBakeScene sunlitRoom() {
    IrradianceBakeScene scene;
    const glm::vec3 white(0.8f);
    addQuad(scene.triangles, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 2.0f}, {2.0f, 0.0f, 0.0f}, {0.8f, 0.1f, 0.1f});
    addQuad(scene.triangles, {-2.0f, 1.5f, 0.0f}, {0.0f, 1.5f, 0.0f}, {0.0f, 0.0f, 2.0f}, white);
    addQuad(scene.triangles, {2.0f, 1.5f, 0.0f}, {0.0f, 0.0f, 2.0f}, {0.0f, 1.5f, 0.0f}, white);
    addQuad(scene.triangles, {0.0f, 1.5f, -2.0f}, {2.0f, 0.0f, 0.0f}, {0.0f, 1.5f, 0.0f}, white);
    addQuad(scene.triangles, {0.0f, 1.5f, 2.0f}, {0.0f, 1.5f, 0.0f}, {2.0f, 0.0f, 0.0f}, white);
    // Ceiling in four strips around a hole spanning x, z in [-0.5, 0.5].
    addQuad(scene.triangles, {-1.25f, 3.0f, 0.0f}, {0.75f, 0.0f, 0.0f}, {0.0f, 0.0f, 2.0f}, white);
    addQuad(scene.triangles, {1.25f, 3.0f, 0.0f}, {0.75f, 0.0f, 0.0f}, {0.0f, 0.0f, 2.0f}, white);
    addQuad(scene.triangles, {0.0f, 3.0f, -1.25f}, {0.5f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.75f}, white);
    addQuad(scene.triangles, {0.0f, 3.0f, 1.25f}, {0.5f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.75f}, white);
    scene.sun = IrradianceBakeSun{.direction = {0.0f, 1.0f, 0.0f}, .irradiance = glm::vec3(5.0f)};
    scene.skyRadiance = [](const glm::vec3&) { return glm::vec3(0.2f, 0.3f, 0.5f); };
    return scene;
}

IrradianceVolumeBakeRequest roomRequest(uint32_t bounces, uint32_t workers) {
    return {.minimum = {-1.8f, 0.2f, -1.8f},
            .maximum = {1.8f, 2.8f, 1.8f},
            .probeCounts = {3u, 2u, 3u},
            .settings = {.directionsPerFace = 6, .bounces = bounces, .workerThreads = workers}};
}

TEST_CASE("IrradianceVolumeBaker: determinism") {
    const IrradianceBakeScene scene = sunlitRoom();
    IrradianceVolumeData single;
    IrradianceVolumeData threaded;
    CHECK_MESSAGE((static_cast<bool>(bakeIrradianceVolume(scene, roomRequest(2, 1), single))),
                  "single-threaded bake succeeds");
    CHECK_MESSAGE((static_cast<bool>(bakeIrradianceVolume(scene, roomRequest(2, 4), threaded))),
                  "threaded bake succeeds");
    std::vector<std::byte> singleBytes;
    std::vector<std::byte> threadedBytes;
    CHECK_MESSAGE((encodeIrradianceVolume(single, singleBytes) && encodeIrradianceVolume(threaded, threadedBytes) &&
                   singleBytes == threadedBytes),
                  "bakes are bit-identical regardless of worker count");
}

TEST_CASE("IrradianceVolumeBaker: bounce lighting") {
    const IrradianceBakeScene scene = sunlitRoom();
    IrradianceVolumeData oneBounce;
    IrradianceVolumeData threeBounces;
    CHECK_MESSAGE((bakeIrradianceVolume(scene, roomRequest(1, 0), oneBounce) &&
                   bakeIrradianceVolume(scene, roomRequest(3, 0), threeBounces)),
                  "room bakes succeed");
    // The probe at the bottom-left corner looks down at the red, sunlit floor.
    const IrradianceProbe& low = oneBounce.probes[oneBounce.probeIndex({0u, 0u, 0u})];
    const glm::vec3 fromFloor = evaluate(low, {0.0f, -1.0f, 0.0f});
    CHECK_MESSAGE(low.valid, "probes inside an open room are valid");
    CHECK_MESSAGE((fromFloor.r > 0.005f && fromFloor.r > 2.0f * fromFloor.g && fromFloor.r > 2.0f * fromFloor.b),
                  "light bounced off a red floor tints the irradiance from below");
    // Neither the sunlit patch nor the opening is in front of the corner wall,
    // so its light arrives only through secondary bounces.
    const glm::vec3 fromWall =
        evaluate(threeBounces.probes[threeBounces.probeIndex({0u, 0u, 0u})], {-1.0f, 0.0f, 0.0f});
    CHECK_MESSAGE((fromWall.r > 0.0f && fromWall.g > 0.0f), "unlit walls still pass on bounced light");
    float oneTotal = 0.0f;
    float threeTotal = 0.0f;
    for (size_t probe = 0; probe < oneBounce.probes.size(); ++probe) {
        oneTotal += evaluate(oneBounce.probes[probe], {1.0f, 0.0f, 0.0f}).g;
        threeTotal += evaluate(threeBounces.probes[probe], {1.0f, 0.0f, 0.0f}).g;
    }
    CHECK_MESSAGE(threeTotal > oneTotal * 1.05f, "further bounces add light inside a closed room");

    const IrradianceProbe& high = oneBounce.probes[oneBounce.probeIndex({1u, 1u, 1u})];
    const glm::vec3 fromAbove = evaluate(high, {0.0f, 1.0f, 0.0f});
    CHECK_MESSAGE(fromAbove.b > fromAbove.r, "the sky through the opening lights probes below it");

    const glm::vec2 depth = high.depthMoments[0];
    CHECK_MESSAGE((depth.x > 0.0f && depth.y >= depth.x * depth.x * 0.999f),
                  "depth moments hold a mean and a mean square");
}

TEST_CASE("IrradianceVolumeBaker: open sky") {
    IrradianceBakeScene scene;
    scene.skyRadiance = [](const glm::vec3&) { return glm::vec3(0.5f); };
    IrradianceVolumeData data;
    const IrradianceVolumeBakeRequest request{.minimum = glm::vec3(-1.0f),
                                              .maximum = glm::vec3(1.0f),
                                              .probeCounts = {1u, 1u, 1u},
                                              .settings = {.bounces = 1}};
    CHECK_MESSAGE((static_cast<bool>(bakeIrradianceVolume(scene, request, data))), "empty scenes bake");
    const glm::vec3 up = evaluate(data.probes[0], {0.0f, 1.0f, 0.0f});
    const glm::vec3 side = evaluate(data.probes[0], {0.0f, 0.0f, -1.0f});
    CHECK_MESSAGE((std::abs(up.r - 0.5f) < 0.01f && std::abs(side.g - 0.5f) < 0.01f),
                  "a constant sky reproduces its radiance in every direction");
    const float maximumDistance = 1.5f * glm::length(data.cellSize());
    CHECK_MESSAGE(std::abs(data.probes[0].depthMoments[10].x - maximumDistance) < 1e-4f,
                  "escaped rays record the maximum depth");
}

TEST_CASE("IrradianceVolumeBaker: probe inside geometry") {
    IrradianceBakeScene scene;
    // A closed single-sided cube with outward faces around the only probe.
    const glm::vec3 grey(0.5f);
    addQuad(scene.triangles, {0.0f, 0.5f, 0.0f}, {0.0f, 0.0f, 0.5f}, {0.5f, 0.0f, 0.0f}, grey);
    addQuad(scene.triangles, {0.0f, -0.5f, 0.0f}, {0.5f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.5f}, grey);
    addQuad(scene.triangles, {0.5f, 0.0f, 0.0f}, {0.0f, 0.5f, 0.0f}, {0.0f, 0.0f, 0.5f}, grey);
    addQuad(scene.triangles, {-0.5f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.5f}, {0.0f, 0.5f, 0.0f}, grey);
    addQuad(scene.triangles, {0.0f, 0.0f, 0.5f}, {0.5f, 0.0f, 0.0f}, {0.0f, 0.5f, 0.0f}, grey);
    addQuad(scene.triangles, {0.0f, 0.0f, -0.5f}, {0.0f, 0.5f, 0.0f}, {0.5f, 0.0f, 0.0f}, grey);
    IrradianceVolumeData data;
    const IrradianceVolumeBakeRequest request{.minimum = glm::vec3(-1.0f),
                                              .maximum = glm::vec3(1.0f),
                                              .probeCounts = {1u, 1u, 1u},
                                              .settings = {.bounces = 1}};
    CHECK_MESSAGE((static_cast<bool>(bakeIrradianceVolume(scene, request, data))), "enclosed probes bake");
    CHECK_MESSAGE(!data.probes[0].valid, "a probe that sees only back faces is marked invalid");

    for (IrradianceBakeTriangle& triangle : scene.triangles) {
        triangle.doubleSided = true;
    }
    CHECK_MESSAGE((bakeIrradianceVolume(scene, request, data) && data.probes[0].valid),
                  "double-sided surfaces never invalidate a probe");
}

TEST_CASE("IrradianceVolumeBaker: request validation") {
    const IrradianceBakeScene scene{};
    IrradianceVolumeData data;
    IrradianceVolumeBakeRequest request{.probeCounts = {0u, 1u, 1u}};
    CHECK_MESSAGE(!bakeIrradianceVolume(scene, request, data), "zero probe counts are rejected");
    request.probeCounts = {16u, 16u, 17u};
    CHECK_MESSAGE(!bakeIrradianceVolume(scene, request, data), "oversized grids are rejected");
    request.probeCounts = {1u, 1u, 1u};
    request.maximum = request.minimum;
    CHECK_MESSAGE(!bakeIrradianceVolume(scene, request, data), "empty bounds are rejected");
    request.maximum = glm::vec3(1.0f);
    request.settings.directionsPerFace = 0;
    CHECK_MESSAGE(!bakeIrradianceVolume(scene, request, data), "zero rays are rejected");
}

TEST_CASE("IrradianceVolumeBaker: gather and bake path") {
    Scene scene;
    const Entity light = scene.createEntity("Lamp");
    scene.registry().emplace<PointLightComponent>(light);
    AssetManager assets;
    const IrradianceBakeScene gathered = gatherIrradianceBakeScene(scene, assets);
    CHECK_MESSAGE((gathered.triangles.empty() && gathered.localLights.size() == 1 && gathered.sun.has_value() &&
                   static_cast<bool>(gathered.skyRadiance)),
                  "gathering collects lights and falls back to the renderer's default sun and sky");

    CHECK_MESSAGE(irradianceVolumeBakeDirectory("assets", std::filesystem::path("scenes/room.zenith-scene")) ==
                      std::filesystem::path("assets") / "baked" / "room",
                  "bakes live under the asset root, grouped by scene");
    CHECK_MESSAGE(irradianceVolumeBakeDirectory("assets", std::nullopt).filename() == "untitled",
                  "unsaved scenes bake into an untitled folder");
    const std::vector<std::byte> first{std::byte{1}, std::byte{2}};
    const std::vector<std::byte> second{std::byte{1}, std::byte{3}};
    const std::string firstName = irradianceVolumeBakeFileName(SceneEntityId{7}, first);
    CHECK_MESSAGE((firstName.starts_with("irradiance_volume_7_") && firstName.ends_with(".zvol") &&
                   firstName == irradianceVolumeBakeFileName(SceneEntityId{7}, first) &&
                   firstName != irradianceVolumeBakeFileName(SceneEntityId{7}, second)),
                  "bake file names identify the volume and its contents");

    const testsupport::TempDirectory scratch("irradiance-bake-test");
    const std::filesystem::path root = scratch.path();
    const Entity volume = scene.createEntity("Volume");
    scene.registry().emplace<IrradianceVolumeComponent>(volume).probeCounts = {2u, 1u, 2u};
    scene.updateWorldTransforms();
    IrradianceVolumeData baked;
    std::filesystem::path written;
    const std::filesystem::path directory = root / "baked";
    CHECK_MESSAGE((static_cast<bool>(bakeIrradianceVolumeFile(scene, assets, volume, directory,
                                                              {.directionsPerFace = 2}, baked, written))),
                  "baking writes the volume file");
    IrradianceVolumeData loaded;
    CHECK_MESSAGE((written.parent_path() == directory && loadIrradianceVolume(written, loaded) &&
                   loaded.probes.size() == 4 && loaded.probes[3].irradiance == baked.probes[3].irradiance),
                  "the written file loads back as the baked data");
    CHECK_MESSAGE(!std::filesystem::exists(written.string() + ".tmp"), "no temporary file is left behind");

    std::filesystem::path rebaked;
    CHECK_MESSAGE(
        (bakeIrradianceVolumeFile(scene, assets, volume, directory, {.directionsPerFace = 2}, baked, rebaked) &&
         rebaked == written),
        "an identical re-bake reuses the same file");
    scene.registry().emplace<EnvironmentLightComponent>(scene.createEntity("Red sky")).skyColor = {1.0f, 0.0f, 0.0f};
    CHECK_MESSAGE(
        (bakeIrradianceVolumeFile(scene, assets, volume, directory, {.directionsPerFace = 2}, baked, rebaked) &&
         rebaked != written && std::filesystem::exists(written)),
        "a changed bake gets a new file and keeps the previous one for undo");
    CHECK_MESSAGE(!bakeIrradianceVolumeFile(scene, assets, light, directory, {}, baked, rebaked),
                  "entities without a volume cannot be baked");

    uint32_t volumesBaked = 0;
    CHECK_MESSAGE((bakeSceneIrradianceVolumes(scene, assets, root, std::filesystem::path("room.zenith-scene"),
                                              {.directionsPerFace = 2}, volumesBaked) &&
                   volumesBaked == 1),
                  "scene bakes cover every volume");
    const IrradianceVolumeComponent& component = scene.registry().get<IrradianceVolumeComponent>(volume);
    CHECK_MESSAGE((component.bakedData && component.bakeAsset.starts_with("baked/room/irradiance_volume_") &&
                   std::filesystem::exists(root / component.bakeAsset)),
                  "scene bakes point each volume at its project-relative file");
}
} // namespace

TEST_SUITE_END();
