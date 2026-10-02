#include "CaptureCamera.hpp"

#include <SDL3/SDL_stdinc.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <glm/gtc/matrix_transform.hpp>
#include <string_view>

namespace {
constexpr size_t componentCount = 5;
using Components = std::array<float, componentCount>;

bool present(const char* value) {
    return value != nullptr && *value != '\0';
}

// Parses exactly five comma-separated finite numbers.
bool parseComponents(std::string_view text, Components& components) {
    for (size_t index = 0; index < componentCount; ++index) {
        const size_t comma = text.find(',');
        const bool lastComponent = index + 1 == componentCount;
        if (lastComponent != (comma == std::string_view::npos)) {
            return false;
        }
        const std::string field(text.substr(0, comma));
        char* end = nullptr;
        const float value = std::strtof(field.c_str(), &end);
        if (field.empty() || end != field.c_str() + field.size() || !std::isfinite(value)) {
            return false;
        }
        components[index] = value;
        if (!lastComponent) {
            text.remove_prefix(comma + 1);
        }
    }
    return true;
}
} // namespace

glm::mat4 CaptureCameraPath::transformAt(float elapsedSeconds) const {
    const glm::vec3 currentPosition = position + velocity * elapsedSeconds;
    const float yaw = glm::radians(yawDegrees + yawDegreesPerSecond * elapsedSeconds);
    const float pitch = glm::radians(std::clamp(pitchDegrees + pitchDegreesPerSecond * elapsedSeconds, -89.0f, 89.0f));
    // Same yaw-then-pitch composition as the first-person controller.
    return glm::translate(glm::mat4(1.0f), currentPosition) *
           glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0.0f, 1.0f, 0.0f)) *
           glm::rotate(glm::mat4(1.0f), pitch, glm::vec3(1.0f, 0.0f, 0.0f));
}

CaptureCameraResult parseCaptureCamera(const char* pose, const char* motion) {
    if (!present(pose)) {
        if (present(motion)) {
            return {.path = std::nullopt, .error = "ZENITH_CAPTURE_CAMERA_MOTION needs ZENITH_CAPTURE_CAMERA"};
        }
        return {};
    }
    Components values{};
    if (!parseComponents(pose, values)) {
        return {.path = std::nullopt,
                .error =
                    "ZENITH_CAPTURE_CAMERA must be 'x,y,z,yawDegrees,pitchDegrees', got '" + std::string(pose) + "'"};
    }
    CaptureCameraPath path{
        .position = {values[0], values[1], values[2]}, .yawDegrees = values[3], .pitchDegrees = values[4]};
    if (present(motion)) {
        if (!parseComponents(motion, values)) {
            return {.path = std::nullopt,
                    .error = "ZENITH_CAPTURE_CAMERA_MOTION must be 'vx,vy,vz,yawDegreesPerSecond,"
                             "pitchDegreesPerSecond', got '" +
                             std::string(motion) + "'"};
        }
        path.velocity = {values[0], values[1], values[2]};
        path.yawDegreesPerSecond = values[3];
        path.pitchDegreesPerSecond = values[4];
    }
    return {.path = path, .error = {}};
}

CaptureCameraResult captureCameraFromEnvironment() {
    return parseCaptureCamera(SDL_getenv("ZENITH_CAPTURE_CAMERA"), SDL_getenv("ZENITH_CAPTURE_CAMERA_MOTION"));
}
