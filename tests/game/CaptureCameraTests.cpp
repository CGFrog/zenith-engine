#include "game/scene/CaptureCamera.hpp"

#include <cmath>
#include <iostream>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

bool near(const glm::vec3& actual, const glm::vec3& expected) {
    return glm::all(glm::lessThan(glm::abs(actual - expected), glm::vec3(1e-4f)));
}

glm::vec3 forward(const glm::mat4& transform) {
    return -glm::vec3(transform[2]);
}

void testParsing() {
    CaptureCameraResult result = parseCaptureCamera(nullptr, nullptr);
    expect(static_cast<bool>(result) && !result.path, "unset variables mean no capture camera");
    expect(static_cast<bool>(parseCaptureCamera("", "")) && !parseCaptureCamera("", "").path,
           "empty variables mean no capture camera");

    result = parseCaptureCamera("1,2.5,-3,90,-10", nullptr);
    expect(static_cast<bool>(result) && result.path, "a pose parses");
    expect(result.path && near(result.path->position, {1.0f, 2.5f, -3.0f}) && result.path->yawDegrees == 90.0f &&
               result.path->pitchDegrees == -10.0f,
           "pose components land in order");
    expect(result.path && result.path->velocity == glm::vec3(0.0f) && result.path->yawDegreesPerSecond == 0.0f,
           "a pose alone is static");

    result = parseCaptureCamera("0,0,0,0,0", "1,0,-2,15,-5");
    expect(result.path && near(result.path->velocity, {1.0f, 0.0f, -2.0f}) &&
               result.path->yawDegreesPerSecond == 15.0f && result.path->pitchDegreesPerSecond == -5.0f,
           "motion components land in order");

    expect(!parseCaptureCamera(nullptr, "1,0,0,0,0"), "motion without a pose is an error");
    expect(!parseCaptureCamera("1,2,3,4", nullptr), "a pose needs five components");
    expect(!parseCaptureCamera("1,2,3,4,5,6", nullptr), "a pose rejects extra components");
    expect(!parseCaptureCamera("1,2,,4,5", nullptr), "a pose rejects empty components");
    expect(!parseCaptureCamera("1,2,x,4,5", nullptr), "a pose rejects non-numbers");
    expect(!parseCaptureCamera("1,2,3,4,5", "1,2,3"), "malformed motion is an error");
}

void testTransform() {
    const CaptureCameraPath still{.position = {1.0f, 2.0f, 3.0f}};
    const glm::mat4 origin = still.transformAt(5.0f);
    expect(near(glm::vec3(origin[3]), {1.0f, 2.0f, 3.0f}), "a static camera stays put");
    expect(near(forward(origin), {0.0f, 0.0f, -1.0f}), "zero yaw looks down -Z");

    const CaptureCameraPath turned{.yawDegrees = 90.0f};
    expect(near(forward(turned.transformAt(0.0f)), {-1.0f, 0.0f, 0.0f}), "positive yaw turns left");
    const CaptureCameraPath raised{.pitchDegrees = 90.0f};
    expect(forward(raised.transformAt(0.0f)).y > 0.99f, "positive pitch looks up and is clamped below vertical");

    const CaptureCameraPath moving{
        .position = {0.0f, 1.0f, 0.0f}, .velocity = {2.0f, 0.0f, -1.0f}, .yawDegreesPerSecond = 45.0f};
    const glm::mat4 later = moving.transformAt(2.0f);
    expect(near(glm::vec3(later[3]), {4.0f, 1.0f, -2.0f}), "position advances linearly");
    expect(near(forward(later), {-1.0f, 0.0f, 0.0f}), "yaw advances by its rate");
}
} // namespace

int main() {
    testParsing();
    testTransform();
    if (failures != 0) {
        std::cerr << failures << " capture camera test(s) failed\n";
        return 1;
    }
    std::cout << "Capture camera tests passed\n";
    return 0;
}
