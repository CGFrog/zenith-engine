#include "engine/assets/Assets.hpp"
#include "engine/assets/IrradianceVolumeAsset.hpp"
#include "engine/graphics/IrradianceVolume.hpp"
#include "engine/graphics/RenderConfiguration.hpp"
#include "engine/scene/Scene.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

bool near(float actual, float expected, float tolerance) {
    return std::abs(actual - expected) <= tolerance;
}

bool near(const glm::vec2& actual, const glm::vec2& expected, float tolerance) {
    return near(actual.x, expected.x, tolerance) && near(actual.y, expected.y, tolerance);
}

bool near(const glm::vec3& actual, const glm::vec3& expected, float tolerance) {
    return near(actual.x, expected.x, tolerance) && near(actual.y, expected.y, tolerance) &&
           near(actual.z, expected.z, tolerance);
}

// Band-0 coefficient that evaluates to `radiance` for every normal.
constexpr float shBand0 = 0.282095f;

IrradianceProbe uniformProbe(const glm::vec3& radiance, float depth) {
    IrradianceProbe probe;
    probe.irradiance[0] = radiance / shBand0;
    probe.depthMoments.fill(glm::vec2(depth, depth * depth));
    return probe;
}

IrradianceVolumeData uniformVolume(const glm::uvec3& counts, const glm::vec3& radiance) {
    IrradianceVolumeData data{.minimum = glm::vec3(0.0f), .maximum = glm::vec3(counts), .probeCounts = counts};
    data.probes.assign(static_cast<size_t>(counts.x) * counts.y * counts.z, uniformProbe(radiance, 10.0f));
    return data;
}

void testOctahedralMapping() {
    using namespace irradiance_volume;
    const glm::vec3 directions[]{{0.0f, 1.0f, 0.0f},  {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
                                 {-0.3f, 0.4f, 0.8f}, {0.6f, -0.7f, 0.2f}, {-0.5f, -0.5f, -0.7f}};
    bool roundTrips = true;
    bool inRange = true;
    for (glm::vec3 direction : directions) {
        direction = glm::normalize(direction);
        const glm::vec2 encoded = octahedralEncode(direction);
        inRange &= encoded.x >= 0.0f && encoded.x <= 1.0f && encoded.y >= 0.0f && encoded.y <= 1.0f;
        roundTrips &= near(octahedralDecode(encoded), direction, 1e-5f);
    }
    expect(roundTrips, "octahedral encoding round-trips unit directions");
    expect(inRange, "octahedral coordinates stay inside the unit square");
    expect(near(octahedralEncode({0.0f, 1.0f, 0.0f}), glm::vec2(0.5f), 1e-6f),
           "+Y maps to the center of the octahedral map");
    expect(near(glm::length(depthTexelDirection(0, 7)), 1.0f, 1e-5f), "texel directions are unit length");
}

void testTrilinearWeights() {
    using namespace irradiance_volume;
    const IrradianceVolumeData data = uniformVolume({4u, 3u, 2u}, glm::vec3(1.0f));
    float total = 0.0f;
    for (const ProbeCorner& corner : surroundingProbes(data, {1.3f, 0.9f, 1.1f})) {
        total += corner.trilinear;
    }
    expect(near(total, 1.0f, 1e-5f), "trilinear weights sum to one");

    // Probe (1, 1, 0) sits at its cell center (1.5, 1.5, 0.5).
    float atProbe = 0.0f;
    for (const ProbeCorner& corner : surroundingProbes(data, data.probePosition({1u, 1u, 0u}))) {
        if (corner.index == glm::uvec3(1u, 1u, 0u)) {
            atProbe += corner.trilinear;
        }
    }
    expect(near(atProbe, 1.0f, 1e-5f), "a point at a probe takes all of its weight");

    float left = 0.0f;
    float right = 0.0f;
    for (const ProbeCorner& corner : surroundingProbes(data, {2.0f, 1.5f, 0.5f})) {
        left += corner.index == glm::uvec3(1u, 1u, 0u) ? corner.trilinear : 0.0f;
        right += corner.index == glm::uvec3(2u, 1u, 0u) ? corner.trilinear : 0.0f;
    }
    expect(near(left, 0.5f, 1e-5f) && near(right, 0.5f, 1e-5f), "the midpoint between probes splits evenly");

    bool clamped = true;
    for (const ProbeCorner& corner : surroundingProbes(data, {-5.0f, 10.0f, 0.5f})) {
        clamped &= glm::all(glm::lessThan(corner.index, data.probeCounts));
        if (corner.trilinear > 0.0f) {
            clamped &= corner.index.x == 0u && corner.index.y == 2u;
        }
    }
    expect(clamped, "points outside the lattice clamp to its border probes");

    const IrradianceVolumeData single = uniformVolume({1u, 1u, 1u}, glm::vec3(1.0f));
    float singleTotal = 0.0f;
    for (const ProbeCorner& corner : surroundingProbes(single, {0.2f, 0.9f, 0.4f})) {
        singleTotal += corner.index == glm::uvec3(0u) ? corner.trilinear : 0.0f;
    }
    expect(near(singleTotal, 1.0f, 1e-5f), "a single-probe volume gives that probe all the weight");
}

void testVisibilityWeight() {
    using namespace irradiance_volume;
    const glm::vec2 moments(2.0f, 4.01f);
    expect(chebyshevVisibility(moments, 1.5f) == 1.0f, "points in front of the mean depth are visible");
    expect(chebyshevVisibility(moments, 2.0f) == 1.0f, "points at the mean depth are visible");
    const float justBehind = chebyshevVisibility(moments, 2.05f);
    const float farBehind = chebyshevVisibility(moments, 3.0f);
    expect(justBehind > farBehind && farBehind < 0.01f, "visibility falls off behind the mean depth");
    expect(chebyshevVisibility({2.0f, 4.0f}, 2.5f) < 0.001f,
           "a zero-variance surface hides points behind it (minimum variance applied)");

    IrradianceProbe probe = uniformProbe(glm::vec3(1.0f), 3.0f);
    expect(near(sampleDepthMoments(probe, glm::normalize(glm::vec3(0.3f, -0.4f, 0.2f))), glm::vec2(3.0f, 9.0f), 1e-5f),
           "uniform depth maps filter to their value");
}

void testEdgeWeight() {
    using namespace irradiance_volume;
    const IrradianceVolumeData data = uniformVolume({2u, 2u, 2u}, glm::vec3(1.0f));
    expect(near(edgeWeight(data, {1.0f, 1.0f, 1.0f}, 0.5f), 1.0f, 1e-6f), "the volume applies fully inside");
    expect(near(edgeWeight(data, {0.25f, 1.0f, 1.0f}, 0.5f), 0.5f, 1e-6f), "the volume fades across the blend band");
    expect(edgeWeight(data, {-0.1f, 1.0f, 1.0f}, 0.5f) == 0.0f, "the volume does not apply outside");
    expect(edgeWeight(data, {0.01f, 1.0f, 1.0f}, 0.0f) == 1.0f, "a zero blend distance is a hard edge");
}

void testSampling() {
    using namespace irradiance_volume;
    const ShadingParameters parameters{.normalBias = 0.0f, .viewBias = 0.0f, .blendDistance = 0.25f};
    const IrradianceVolumeData uniform = uniformVolume({3u, 3u, 3u}, {0.2f, 0.4f, 0.6f});
    const Sample inside = sample(uniform, parameters, {1.4f, 1.6f, 1.2f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
    expect(near(inside.irradiance, {0.2f, 0.4f, 0.6f}, 1e-4f), "a uniform volume reproduces its irradiance");
    expect(near(inside.volumeWeight, 1.0f, 1e-6f), "interior samples take the full volume weight");

    // Two probes along x; the point sits just past the left one, behind a wall
    // the right probe sees 0.2 units away, so the right probe is occluded.
    IrradianceVolumeData wall{.minimum = glm::vec3(0.0f), .maximum = {2.0f, 1.0f, 1.0f}, .probeCounts = {2u, 1u, 1u}};
    wall.probes = {uniformProbe({1.0f, 0.0f, 0.0f}, 10.0f), uniformProbe({0.0f, 0.0f, 1.0f}, 0.2f)};
    const glm::vec3 point(0.9f, 0.5f, 0.5f);
    const Sample occluded = sample(wall, parameters, point, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    expect(occluded.irradiance.r > 0.95f && occluded.irradiance.b < 0.05f,
           "Chebyshev visibility rejects a probe behind a wall");

    // Without the wall both probes contribute.
    wall.probes[1] = uniformProbe({0.0f, 0.0f, 1.0f}, 10.0f);
    const Sample open = sample(wall, parameters, point, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    expect(open.irradiance.b > 0.2f && open.irradiance.r > 0.2f, "visible probes blend trilinearly");

    // A surface facing away from a probe weights it less than one facing it.
    const Sample facingLeft = sample(wall, parameters, {1.0f, 0.5f, 0.5f}, {-1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f});
    expect(facingLeft.irradiance.r > facingLeft.irradiance.b, "the backface term favors probes in front");

    wall.probes[0].valid = false;
    const Sample skipped = sample(wall, parameters, point, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    expect(skipped.irradiance.r < 1e-5f && skipped.irradiance.b > 0.99f, "invalid probes are skipped");
    wall.probes[1].valid = false;
    const Sample none = sample(wall, parameters, point, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    expect(none.volumeWeight == 0.0f, "a point no probe can light falls back to the environment");
}

void testMeanRadiance() {
    using namespace irradiance_volume;
    // A uniform probe's mean radiance is its band-0 value in every direction.
    const IrradianceVolumeData uniform = uniformVolume({3u, 3u, 3u}, {0.2f, 0.4f, 0.6f});
    const Sample inside = sampleMeanRadiance(uniform, {1.4f, 1.6f, 1.2f}, 0.25f);
    expect(near(inside.irradiance, {0.2f, 0.4f, 0.6f}, 1e-4f), "fog ambient reproduces a uniform volume");
    expect(near(inside.volumeWeight, 1.0f, 1e-6f), "fog inside the volume takes its full weight");
    expect(sampleMeanRadiance(uniform, {-0.5f, 1.0f, 1.0f}, 0.25f).volumeWeight == 0.0f,
           "fog outside the volume keeps the environment ambient");

    // Same wall as testSampling: the right probe is occluded from the point.
    IrradianceVolumeData wall{.minimum = glm::vec3(0.0f), .maximum = {2.0f, 1.0f, 1.0f}, .probeCounts = {2u, 1u, 1u}};
    wall.probes = {uniformProbe({1.0f, 0.0f, 0.0f}, 10.0f), uniformProbe({0.0f, 0.0f, 1.0f}, 0.2f)};
    const Sample occluded = sampleMeanRadiance(wall, {0.9f, 0.5f, 0.5f}, 0.0f);
    expect(occluded.irradiance.r > 0.95f && occluded.irradiance.b < 0.05f,
           "fog ambient ignores probes that cannot see the point");
    wall.probes[0].valid = false;
    wall.probes[1].valid = false;
    expect(sampleMeanRadiance(wall, {0.9f, 0.5f, 0.5f}, 0.0f).volumeWeight == 0.0f,
           "fog no probe can light falls back to the environment ambient");
}

void testEncoding() {
    IrradianceVolumeData data = uniformVolume({2u, 1u, 3u}, {0.5f, 0.25f, 0.125f});
    data.minimum = {-1.0f, 0.0f, -2.0f};
    data.maximum = {1.0f, 3.0f, 4.0f};
    data.probes[3].valid = false;
    data.probes[4].irradiance[7] = {0.1f, -0.2f, 0.3f};
    data.probes[5].depthMoments[17] = {1.25f, 2.0f};
    std::vector<std::byte> bytes;
    expect(static_cast<bool>(encodeIrradianceVolume(data, bytes)), "valid volumes encode");
    IrradianceVolumeData decoded;
    expect(static_cast<bool>(decodeIrradianceVolume(bytes, decoded)), "encoded volumes decode");
    bool equal = decoded.minimum == data.minimum && decoded.maximum == data.maximum &&
                 decoded.probeCounts == data.probeCounts && decoded.probes.size() == data.probes.size();
    for (size_t probe = 0; equal && probe < data.probes.size(); ++probe) {
        equal = decoded.probes[probe].valid == data.probes[probe].valid &&
                decoded.probes[probe].irradiance == data.probes[probe].irradiance &&
                decoded.probes[probe].depthMoments == data.probes[probe].depthMoments;
    }
    expect(equal, "decoding restores every probe exactly");

    std::vector<std::byte> corrupt = bytes;
    corrupt[0] = std::byte{'X'};
    expect(!decodeIrradianceVolume(corrupt, decoded), "a bad signature is rejected");
    corrupt = bytes;
    corrupt[4] = std::byte{9};
    expect(!decodeIrradianceVolume(corrupt, decoded), "an unknown version is rejected");
    corrupt.assign(bytes.begin(), bytes.end() - 3);
    expect(!decodeIrradianceVolume(corrupt, decoded), "truncated data is rejected");
    corrupt = bytes;
    corrupt.push_back(std::byte{0});
    expect(!decodeIrradianceVolume(corrupt, decoded), "trailing data is rejected");

    IrradianceVolumeData mismatched = data;
    mismatched.probes.pop_back();
    expect(!encodeIrradianceVolume(mismatched, bytes), "a probe array that does not match the counts is rejected");
    IrradianceVolumeData inverted = data;
    inverted.maximum.y = inverted.minimum.y;
    expect(!encodeIrradianceVolume(inverted, bytes), "empty bounds are rejected");
    IrradianceVolumeData oversized = data;
    oversized.probeCounts = {33u, 1u, 1u};
    expect(!validateIrradianceVolume(oversized), "per-axis probe limits are enforced");

    const std::vector<irradiance_volume::GpuProbe> packed = irradiance_volume::packProbes(data);
    expect(packed.size() == data.probes.size() && packed[3].irradiance[0].w == 0.0f &&
               packed[0].irradiance[0].w == 1.0f && glm::vec3(packed[4].irradiance[7]) == data.probes[4].irradiance[7],
           "GPU records carry coefficients and the validity flag");
}

void testVolumeSelection() {
    Scene scene;
    const Entity near = scene.createEntity("Near");
    const Entity far = scene.createEntity("Far");
    const Entity unbaked = scene.createEntity("Unbaked");
    auto nearData = std::make_shared<IrradianceVolumeData>(uniformVolume({1u, 1u, 1u}, glm::vec3(1.0f)));
    auto farData = std::make_shared<IrradianceVolumeData>(uniformVolume({1u, 1u, 1u}, glm::vec3(1.0f)));
    farData->minimum = {10.0f, 0.0f, 0.0f};
    farData->maximum = {11.0f, 1.0f, 1.0f};
    scene.registry().emplace<IrradianceVolumeComponent>(near).bakedData = nearData;
    scene.registry().emplace<IrradianceVolumeComponent>(far).bakedData = farData;
    scene.registry().emplace<IrradianceVolumeComponent>(unbaked);
    expect(irradiance_volume::selectVolume(scene, {0.5f, 0.5f, 0.5f}) == near, "the containing volume is selected");
    expect(irradiance_volume::selectVolume(scene, {10.4f, 0.5f, 0.5f}) == far, "the other volume when inside it");
    expect(irradiance_volume::selectVolume(scene, {4.0f, 0.5f, 0.5f}) == near, "the nearest volume otherwise");
    Scene empty;
    empty.registry().emplace<IrradianceVolumeComponent>(empty.createEntity("Unbaked"));
    expect(irradiance_volume::selectVolume(empty, {}) == entt::null, "unbaked volumes are never bound");
}

void testSceneDocumentRoundTrip() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "zenith_irradiance_volume_test";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "baked");
    const IrradianceVolumeData data = uniformVolume({2u, 2u, 2u}, {0.3f, 0.2f, 0.1f});
    std::vector<std::byte> bytes;
    expect(static_cast<bool>(encodeIrradianceVolume(data, bytes)), "fixture volume encodes");
    {
        std::ofstream output(root / "baked" / "room.zvol", std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    Scene scene;
    const Entity entity = scene.createEntity("Volume");
    IrradianceVolumeComponent& volume = scene.registry().emplace<IrradianceVolumeComponent>(entity);
    volume.halfExtents = {3.0f, 1.5f, 2.5f};
    volume.probeCounts = {6u, 3u, 5u};
    volume.intensity = 1.25f;
    volume.blendDistance = 0.75f;
    volume.normalBias = 0.05f;
    volume.viewBias = 0.15f;
    volume.bakeAsset = "baked/room.zvol";
    AssetManager assets;
    const SceneDocumentOptions options{.assetRoot = root};
    std::string serialized;
    expect(static_cast<bool>(SceneDocument::serialize(scene, assets, options, serialized)),
           "scenes with irradiance volumes serialize");
    Scene loaded;
    const SceneDocumentResult result = SceneDocument::deserialize(serialized, assets, options, loaded);
    expect(static_cast<bool>(result), "scenes with irradiance volumes load");
    const auto view = loaded.registry().view<const IrradianceVolumeComponent>();
    expect(view.size() == 1, "the volume component survives a round trip");
    for (Entity loadedEntity : view) {
        const IrradianceVolumeComponent& restored = view.get<const IrradianceVolumeComponent>(loadedEntity);
        expect(restored.halfExtents == volume.halfExtents && restored.probeCounts == volume.probeCounts &&
                   restored.intensity == volume.intensity && restored.blendDistance == volume.blendDistance &&
                   restored.normalBias == volume.normalBias && restored.viewBias == volume.viewBias &&
                   restored.bakeAsset == volume.bakeAsset,
               "volume parameters round-trip");
        expect(restored.bakedData && restored.bakedData->probes.size() == 8 &&
                   restored.bakedData->probes[0].irradiance[0] == data.probes[0].irradiance[0],
               "the referenced bake loads with the scene");
    }

    std::string missing = serialized;
    const size_t reference = missing.find("baked/room.zvol");
    missing.replace(reference, std::string("baked/room.zvol").size(), "baked/none.zvol");
    Scene unbaked;
    expect(static_cast<bool>(SceneDocument::deserialize(missing, assets, options, unbaked)),
           "a missing bake still lets the scene load");
    const auto unbakedView = unbaked.registry().view<const IrradianceVolumeComponent>();
    for (Entity unbakedEntity : unbakedView) {
        const IrradianceVolumeComponent& restored = unbakedView.get<const IrradianceVolumeComponent>(unbakedEntity);
        expect(!restored.bakedData && restored.bakeAsset == "baked/none.zvol",
               "a missing bake leaves the volume unbaked but keeps its reference");
    }
    Scene broken;
    std::string escaping = serialized;
    escaping.replace(escaping.find("baked/room.zvol"), std::string("baked/room.zvol").size(), "../room.zvol");
    expect(!SceneDocument::deserialize(escaping, assets, options, broken), "bake references cannot leave the root");
    {
        std::ofstream corrupt(root / "baked" / "corrupt.zvol", std::ios::binary);
        corrupt << "not a volume";
    }
    std::string corrupted = serialized;
    corrupted.replace(corrupted.find("baked/room.zvol"), std::string("baked/room.zvol").size(), "baked/corrupt.zvol");
    expect(!SceneDocument::deserialize(corrupted, assets, options, broken), "a corrupt bake is reported");

    std::string invalid = serialized;
    const size_t counts = invalid.find("\"probe_counts\"");
    invalid.replace(invalid.find('6', counts), 1, "0");
    expect(!SceneDocument::deserialize(invalid, assets, options, broken), "zero probe counts are rejected");
    std::filesystem::remove_all(root, error);
}

void testRenderSettings() {
    RenderSettings settings;
    expect(settings.indirectDiffuse == IndirectDiffuseMode::ProbeVolume, "probe volumes are on by default");
    settings.indirectDiffuse = IndirectDiffuseMode::Environment;
    settings.budgets.irradianceVolume = {0.0, 0.05};
    std::string serialized;
    expect(static_cast<bool>(serializeRenderSettings(settings, serialized)), "settings serialize");
    RenderSettings decoded;
    expect(static_cast<bool>(deserializeRenderSettings(serialized, decoded)) &&
               decoded.indirectDiffuse == IndirectDiffuseMode::Environment &&
               decoded.budgets.irradianceVolume.gpuMilliseconds == 0.05,
           "the indirect-diffuse mode and its budget round-trip");
    RenderSettings legacy;
    expect(static_cast<bool>(deserializeRenderSettings(R"({"version":1,"antialiasing":1,"shadows":1,"lighting":1,)"
                                                       R"("bloom":1,"environment":0,"localLights":1})",
                                                       legacy)) &&
               legacy.indirectDiffuse == IndirectDiffuseMode::ProbeVolume,
           "documents without the field keep the default");
    expect(!deserializeRenderSettings(R"({"version":1,"antialiasing":1,"shadows":1,"lighting":1,"bloom":1,)"
                                      R"("environment":0,"localLights":1,"indirectDiffuse":7})",
                                      legacy),
           "unknown indirect-diffuse modes are rejected");

    RenderSettings requested;
    requested.indirectDiffuse = IndirectDiffuseMode::ProbeVolume;
    const ValidatedRenderSettings unsupported = validateRenderSettings(requested, RenderCapabilities{});
    expect(unsupported.settings.indirectDiffuse == IndirectDiffuseMode::Environment && !unsupported.fallbacks.empty(),
           "devices without irradiance volumes fall back to environment lighting");
    RenderCapabilities capable;
    capable.irradianceVolumes = true;
    expect(validateRenderSettings(requested, capable).settings.indirectDiffuse == IndirectDiffuseMode::ProbeVolume,
           "capable renderers keep probe volumes");
}
} // namespace

int main() {
    testOctahedralMapping();
    testTrilinearWeights();
    testVisibilityWeight();
    testEdgeWeight();
    testSampling();
    testMeanRadiance();
    testEncoding();
    testVolumeSelection();
    testSceneDocumentRoundTrip();
    testRenderSettings();
    if (failures != 0) {
        std::cerr << failures << " irradiance volume test(s) failed\n";
        return 1;
    }
    std::cout << "Irradiance volume tests passed\n";
    return 0;
}
