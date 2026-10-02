#include "engine/graphics/DepthOfField.hpp"
#include "engine/graphics/RenderConfiguration.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <iostream>
#include <limits>
#include <string>

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

constexpr float nearPlane = 0.1f;
constexpr float farPlane = 500.0f;

// Reversed-Z projection built like MeshPass::record, optionally jittered.
glm::mat4 sceneProjection(float verticalFovRadians, glm::vec2 jitterNdc = glm::vec2(0.0f)) {
    glm::mat4 projection = glm::perspectiveRH_ZO(verticalFovRadians, 16.0f / 9.0f, farPlane, nearPlane);
    projection[1][1] *= -1.0f;
    projection[2][0] += jitterNdc.x;
    projection[2][1] += jitterNdc.y;
    return projection;
}

float depthAt(const glm::mat4& projection, float viewDistance) {
    const glm::vec4 clip = projection * glm::vec4(0.0f, 0.0f, -viewDistance, 1.0f);
    return clip.z / clip.w;
}

void testCameraTerms() {
    const float fov = glm::radians(50.0f);
    const glm::mat4 projection = sceneProjection(fov, {0.001f, -0.002f});
    const depth_of_field::CameraTerms camera = depth_of_field::cameraFromProjection(projection);
    expect(near(camera.verticalFovRadians, fov, 1e-5f), "the field of view should be recovered from the projection");
    for (float distance : {0.5f, 3.0f, 12.0f, 140.0f}) {
        const float depth = depthAt(projection, distance);
        expect(near(depth_of_field::viewDistanceFromDepth(depth, camera), distance, distance * 1e-4f),
               "view distance should invert the reversed-Z depth");
    }
    expect(near(depth_of_field::viewDistanceFromDepth(0.0f, camera), farPlane, 0.05f),
           "the cleared depth (sky) should lie at the far plane");
    expect(near(depth_of_field::viewDistanceFromDepth(1.0f, camera), nearPlane, 1e-5f),
           "depth 1 should lie at the near plane");
}

void testFocalLength() {
    // tan(fov / 2) = 0.5 makes the focal length equal the sensor height.
    const float fov = 2.0f * std::atan(0.5f);
    expect(near(depth_of_field::focalLengthMeters(fov, 0.024f), 0.024f, 1e-6f),
           "focal length should follow the sensor height and field of view");
    expect(depth_of_field::focalLengthMeters(glm::radians(30.0f), 0.024f) >
               depth_of_field::focalLengthMeters(glm::radians(60.0f), 0.024f),
           "narrower fields of view should have longer lenses");
}

DepthOfFieldSettings lens(float focusDistance, float fStop) {
    return {.quality = DepthOfFieldQuality::High,
            .focusDistance = focusDistance,
            .fStop = fStop,
            .sensorHeightMillimeters = 24.0f,
            .maximumCocRadiusPixels = 32.0f};
}

void testCircleOfConfusion() {
    const float fov = glm::radians(40.0f);
    const float height = 1080.0f;
    const depth_of_field::CircleOfConfusion circle = depth_of_field::circleOfConfusion(lens(4.0f, 1.4f), fov, height);
    expect(near(circle.radiusPixels(4.0f), 0.0f, 1e-5f), "the focus plane should be sharp");
    expect(circle.radiusPixels(2.0f) < 0.0f, "the foreground should have a negative CoC");
    expect(circle.radiusPixels(10.0f) > 0.0f, "the background should have a positive CoC");
    expect(circle.radiusPixels(3.0f) > circle.radiusPixels(2.0f) &&
               circle.radiusPixels(10.0f) > circle.radiusPixels(6.0f),
           "the signed CoC should grow with view distance");

    // Classic thin-lens diameter on the sensor, converted to a pixel radius.
    const float sensor = 0.024f;
    const float focal = depth_of_field::focalLengthMeters(fov, sensor);
    const float aperture = focal / 1.4f;
    const float distance = 9.0f;
    const float diameter = aperture * focal * (distance - 4.0f) / (distance * (4.0f - focal));
    const float expectedRadius = 0.5f * diameter / sensor * height;
    expect(near(circle.radiusPixels(distance), expectedRadius, expectedRadius * 1e-4f),
           "the CoC should match the thin-lens formula");
    expect(circle.scale < circle.maximumRadius && near(circle.radiusPixels(1e7f), circle.scale, circle.scale * 1e-4f),
           "the CoC at infinity should approach the scale");

    const depth_of_field::CircleOfConfusion stoppedDown =
        depth_of_field::circleOfConfusion(lens(4.0f, 2.8f), fov, height);
    expect(near(stoppedDown.radiusPixels(distance), 0.5f * circle.radiusPixels(distance), 1e-3f),
           "doubling the f-number should halve the blur");
    const depth_of_field::CircleOfConfusion doubled =
        depth_of_field::circleOfConfusion(lens(4.0f, 1.4f), fov, 2.0f * height);
    expect(near(doubled.radiusPixels(distance), 2.0f * circle.radiusPixels(distance), 1e-3f),
           "the CoC in pixels should scale with the image height");

    DepthOfFieldSettings clamped = lens(4.0f, 0.5f);
    clamped.maximumCocRadiusPixels = 3.0f;
    const depth_of_field::CircleOfConfusion limited = depth_of_field::circleOfConfusion(clamped, fov, height);
    expect(limited.radiusPixels(1000.0f) == 3.0f && limited.radiusPixels(0.2f) == -3.0f,
           "the CoC should be clamped to the maximum radius on both sides");

    // A focus distance inside the focal length cannot be focused.
    DepthOfFieldSettings tooClose = lens(0.05f, 1.4f);
    tooClose.sensorHeightMillimeters = 100.0f;
    const depth_of_field::CircleOfConfusion moved =
        depth_of_field::circleOfConfusion(tooClose, glm::radians(5.0f), height);
    expect(std::isfinite(moved.scale) && moved.scale > 0.0f &&
               moved.focusDistance > depth_of_field::focalLengthMeters(glm::radians(5.0f), 0.1f),
           "a focus distance inside the focal length should move out to where the lens can focus");
}

void testGather() {
    expect(depth_of_field::gatherExtent({1280, 720}) == glm::uvec2(640, 360), "even extents should halve");
    expect(depth_of_field::gatherExtent({1281, 721}) == glm::uvec2(641, 361), "odd extents should round up");
    expect(depth_of_field::gatherRingSpacing(DepthOfFieldQuality::Off) == 0.0f, "Off should not gather");
    expect(depth_of_field::gatherRingSpacing(DepthOfFieldQuality::Low) >
               depth_of_field::gatherRingSpacing(DepthOfFieldQuality::High),
           "Low should space its rings further apart than High");
    const float radius = 6.0f;
    const uint32_t low =
        depth_of_field::gatherSampleCount(radius, depth_of_field::gatherRingSpacing(DepthOfFieldQuality::Low));
    const uint32_t high =
        depth_of_field::gatherSampleCount(radius, depth_of_field::gatherRingSpacing(DepthOfFieldQuality::High));
    expect(low > 0 && high > low, "High should take more samples than Low");
    // radius^2 / (2 spacing) approximates the spiral's sample count.
    expect(high > 25 && high < 45, "the High spiral should take about radius^2 samples");
    expect(depth_of_field::gatherSampleCount(0.4f, 0.5f) == 0, "a radius below the first ring should not gather");
    expect(depth_of_field::gatherSampleCount(1e6f, 0.5f) == depth_of_field::maximumGatherSamples,
           "the sample count should be capped");
    expect(depth_of_field::gatherSampleCount(16.0f, 0.0f) == 0, "a zero spacing should not gather");
}

void testCompositeBlend() {
    expect(depth_of_field::compositeBlend(0.0f) == 0.0f && depth_of_field::compositeBlend(0.5f) == 0.0f,
           "sub-pixel CoCs should stay sharp");
    expect(depth_of_field::compositeBlend(2.0f) == 1.0f && depth_of_field::compositeBlend(30.0f) == 1.0f,
           "large CoCs should take the blur");
    expect(depth_of_field::compositeBlend(1.0f) < depth_of_field::compositeBlend(1.5f),
           "the blend should grow with the CoC");
}

void testValidation() {
    expect(depth_of_field::validSettings(DepthOfFieldSettings{}), "default settings should be valid");
    const auto invalid = [](auto change) {
        DepthOfFieldSettings settings;
        change(settings);
        return !depth_of_field::validSettings(settings);
    };
    expect(invalid([](DepthOfFieldSettings& s) { s.focusDistance = 0.0f; }), "a zero focus distance is invalid");
    expect(invalid([](DepthOfFieldSettings& s) { s.fStop = 0.1f; }), "an f-number below the range is invalid");
    expect(invalid([](DepthOfFieldSettings& s) { s.fStop = std::numeric_limits<float>::quiet_NaN(); }),
           "a NaN f-number is invalid");
    expect(invalid([](DepthOfFieldSettings& s) { s.sensorHeightMillimeters = 500.0f; }),
           "a sensor beyond the range is invalid");
    expect(invalid([](DepthOfFieldSettings& s) { s.maximumCocRadiusPixels = 0.0f; }),
           "a zero maximum radius is invalid");
    expect(invalid([](DepthOfFieldSettings& s) { s.focusDistance = std::numeric_limits<float>::infinity(); }),
           "an infinite focus distance is invalid");
}

std::string serialized(const RenderSettings& settings) {
    std::string text;
    serializeRenderSettings(settings, text);
    return text;
}

void testSerialization() {
    RenderSettings settings;
    settings.depthOfField = {.quality = DepthOfFieldQuality::Low,
                             .focusDistance = 7.5f,
                             .fStop = 1.8f,
                             .sensorHeightMillimeters = 36.0f,
                             .maximumCocRadiusPixels = 20.0f};
    settings.budgets.depthOfField = {.cpuMilliseconds = 0.1, .gpuMilliseconds = 0.75};
    RenderSettings restored;
    expect(deserializeRenderSettings(serialized(settings), restored).success, "depth of field should deserialize");
    expect(restored.depthOfField.quality == DepthOfFieldQuality::Low && restored.depthOfField.focusDistance == 7.5f &&
               restored.depthOfField.fStop == 1.8f && restored.depthOfField.sensorHeightMillimeters == 36.0f &&
               restored.depthOfField.maximumCocRadiusPixels == 20.0f,
           "depth of field settings should round trip");
    expect(restored.budgets.depthOfField.gpuMilliseconds == 0.75, "the depth of field budget should round trip");

    RenderSettings legacy;
    expect(deserializeRenderSettings(R"({"version":1,"antialiasing":2,"shadows":1,"lighting":1,"bloom":0,
                                         "environment":0,"localLights":1})",
                                     legacy)
               .success,
           "documents without depth of field should load");
    expect(legacy.depthOfField.quality == DepthOfFieldQuality::Off, "older documents should keep depth of field Off");

    RenderSettings partial;
    expect(deserializeRenderSettings(R"({"version":1,"antialiasing":2,"shadows":1,"lighting":1,"bloom":0,
                                         "environment":0,"localLights":1,"depthOfField":{"quality":2}})",
                                     partial)
               .success,
           "a depth of field object with only a quality should load");
    expect(partial.depthOfField.quality == DepthOfFieldQuality::High &&
               partial.depthOfField.fStop == DepthOfFieldSettings{}.fStop,
           "missing lens fields should keep their defaults");

    for (const char* rejected : {R"("depthOfField":{"quality":3})", R"("depthOfField":{"fStop":0.0})",
                                 R"("depthOfField":{"maximumCocRadiusPixels":100})", R"("depthOfField":2)",
                                 R"("depthOfField":{"focusDistance":"near"})"}) {
        RenderSettings loaded;
        const std::string document = std::string(R"({"version":1,"antialiasing":2,"shadows":1,"lighting":1,)") +
                                     R"("bloom":0,"environment":0,"localLights":1,)" + rejected + "}";
        expect(!deserializeRenderSettings(document, loaded).success, "invalid depth of field settings are rejected");
    }
}

void testCapabilityFallback() {
    RenderSettings requested;
    requested.depthOfField.quality = DepthOfFieldQuality::High;
    RenderCapabilities capabilities;
    const ValidatedRenderSettings supported = validateRenderSettings(requested, capabilities);
    expect(supported.settings.depthOfField.quality == DepthOfFieldQuality::High,
           "supported depth of field should be kept");
    capabilities.depthOfField = false;
    const ValidatedRenderSettings unsupported = validateRenderSettings(requested, capabilities);
    expect(unsupported.settings.depthOfField.quality == DepthOfFieldQuality::Off,
           "unsupported depth of field should fall back to Off");
    bool reported = false;
    for (const std::string& fallback : unsupported.fallbacks) {
        reported = reported || fallback.find("Depth of field") != std::string::npos;
    }
    expect(reported, "the depth of field fallback should be reported");
    requested.depthOfField.quality = DepthOfFieldQuality::Off;
    expect(validateRenderSettings(requested, capabilities).fallbacks.size() + 1 == unsupported.fallbacks.size(),
           "Off should not report a depth of field fallback");

    RenderSettings outOfRange;
    outOfRange.depthOfField.quality = DepthOfFieldQuality::Low;
    outOfRange.depthOfField.fStop = 0.0f;
    const ValidatedRenderSettings invalidLens = validateRenderSettings(outOfRange, RenderCapabilities{});
    expect(invalidLens.settings.depthOfField.quality == DepthOfFieldQuality::Off && !invalidLens.fallbacks.empty(),
           "out-of-range lens settings set in code should fall back to Off");
}
} // namespace

int main() {
    testCameraTerms();
    testFocalLength();
    testCircleOfConfusion();
    testGather();
    testCompositeBlend();
    testValidation();
    testSerialization();
    testCapabilityFallback();
    if (failures != 0) {
        std::cerr << failures << " depth of field test(s) failed\n";
        return 1;
    }
    std::cout << "Depth of field tests passed\n";
    return 0;
}
