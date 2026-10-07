// The capture camera the game reads from ZENITH_CAPTURE_* variables: its pose
// and motion parse, and it places the view along its path.

#include "game/scene/CaptureCamera.hpp"

#include "support/Approx.hpp"
#include "support/Doctest.hpp"

TEST_SUITE_BEGIN("CaptureCamera");

namespace {
// Positions and directions agree to this tolerance.
constexpr float tolerance = 1e-4f;

glm::vec3 forward(const glm::mat4& transform) {
    return -glm::vec3(transform[2]);
}

TEST_CASE("CaptureCamera: capture camera options parse and reject bad values") {
    CaptureCameraResult result = parseCaptureCamera(nullptr, nullptr);
    CHECK_MESSAGE((static_cast<bool>(result) && !result.path), "unset variables mean no capture camera");
    CHECK_MESSAGE((static_cast<bool>(parseCaptureCamera("", "")) && !parseCaptureCamera("", "").path),
                  "empty variables mean no capture camera");

    result = parseCaptureCamera("1,2.5,-3,90,-10", nullptr);
    CHECK_MESSAGE((static_cast<bool>(result) && result.path), "a pose parses");
    CHECK_MESSAGE((result.path &&
                   testsupport::approxEqual(result.path->position, glm::vec3{1.0f, 2.5f, -3.0f}, tolerance) &&
                   result.path->yawDegrees == 90.0f && result.path->pitchDegrees == -10.0f),
                  "pose components land in order");
    CHECK_MESSAGE((result.path && result.path->velocity == glm::vec3(0.0f) && result.path->yawDegreesPerSecond == 0.0f),
                  "a pose alone is static");

    result = parseCaptureCamera("0,0,0,0,0", "1,0,-2,15,-5");
    CHECK_MESSAGE((result.path &&
                   testsupport::approxEqual(result.path->velocity, glm::vec3{1.0f, 0.0f, -2.0f}, tolerance) &&
                   result.path->yawDegreesPerSecond == 15.0f && result.path->pitchDegreesPerSecond == -5.0f),
                  "motion components land in order");

    CHECK_MESSAGE(!parseCaptureCamera(nullptr, "1,0,0,0,0"), "motion without a pose is an error");
    CHECK_MESSAGE(!parseCaptureCamera("1,2,3,4", nullptr), "a pose needs five components");
    CHECK_MESSAGE(!parseCaptureCamera("1,2,3,4,5,6", nullptr), "a pose rejects extra components");
    CHECK_MESSAGE(!parseCaptureCamera("1,2,,4,5", nullptr), "a pose rejects empty components");
    CHECK_MESSAGE(!parseCaptureCamera("1,2,x,4,5", nullptr), "a pose rejects non-numbers");
    CHECK_MESSAGE(!parseCaptureCamera("1,2,3,4,5", "1,2,3"), "malformed motion is an error");
}

TEST_CASE("CaptureCamera: a capture camera places the view") {
    const CaptureCameraPath still{.position = {1.0f, 2.0f, 3.0f}};
    const glm::mat4 origin = still.transformAt(5.0f);
    CHECK_MESSAGE(testsupport::approxEqual(glm::vec3(origin[3]), glm::vec3{1.0f, 2.0f, 3.0f}, tolerance),
                  "a static camera stays put");
    CHECK_MESSAGE(testsupport::approxEqual(forward(origin), glm::vec3{0.0f, 0.0f, -1.0f}, tolerance),
                  "zero yaw looks down -Z");

    const CaptureCameraPath turned{.yawDegrees = 90.0f};
    CHECK_MESSAGE(testsupport::approxEqual(forward(turned.transformAt(0.0f)), glm::vec3{-1.0f, 0.0f, 0.0f}, tolerance),
                  "positive yaw turns left");
    const CaptureCameraPath raised{.pitchDegrees = 90.0f};
    CHECK_MESSAGE(forward(raised.transformAt(0.0f)).y > 0.99f, "positive pitch looks up and is clamped below vertical");

    const CaptureCameraPath moving{
        .position = {0.0f, 1.0f, 0.0f}, .velocity = {2.0f, 0.0f, -1.0f}, .yawDegreesPerSecond = 45.0f};
    const glm::mat4 later = moving.transformAt(2.0f);
    CHECK_MESSAGE(testsupport::approxEqual(glm::vec3(later[3]), glm::vec3{4.0f, 1.0f, -2.0f}, tolerance),
                  "position advances linearly");
    CHECK_MESSAGE(testsupport::approxEqual(forward(later), glm::vec3{-1.0f, 0.0f, 0.0f}, tolerance),
                  "yaw advances by its rate");
}
} // namespace

TEST_SUITE_END();
