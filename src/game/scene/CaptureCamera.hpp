#pragma once

// A scripted camera that replaces the player for deterministic visual checks (docs/RENDERER_VALIDATION.md).

#include "engine/scene/Scene.hpp"

#include <glm/glm.hpp>
#include <optional>
#include <string>

// A scripted camera that replaces the player for deterministic visual checks
// (docs/RENDERER_VALIDATION.md). Positions are world-space meters. Yaw turns
// left about +Y from looking down -Z; pitch looks up from the horizon. Angles
// are degrees; rates are per second of simulated time.
struct CaptureCameraPath {
    glm::vec3 position{0.0f};
    float yawDegrees{0.0f};
    float pitchDegrees{0.0f};
    glm::vec3 velocity{0.0f};
    float yawDegreesPerSecond{0.0f};
    float pitchDegreesPerSecond{0.0f};

    // World transform after `elapsedSeconds` of linear motion. Pitch is
    // clamped to +/-89 degrees.
    glm::mat4 transformAt(float elapsedSeconds) const;
};

struct CaptureCameraResult {
    std::optional<CaptureCameraPath> path;
    std::string error;

    explicit operator bool() const {
        return error.empty();
    }
};

// `pose` is "x,y,z,yaw,pitch"; the optional `motion` is
// "vx,vy,vz,yawRate,pitchRate". A null or empty pose means no capture camera;
// motion without a pose is an error.
CaptureCameraResult parseCaptureCamera(const char* pose, const char* motion);

// Reads ZENITH_CAPTURE_CAMERA and ZENITH_CAPTURE_CAMERA_MOTION.
CaptureCameraResult captureCameraFromEnvironment();

// Creates the capture camera entity, a camera and the audio listener, posed at
// the start of path.
Entity spawnCaptureCamera(Scene& scene, const CaptureCameraPath& path);

// Poses camera where path is after elapsedSeconds of simulated time.
void moveCaptureCamera(Scene& scene, Entity camera, const CaptureCameraPath& path, float elapsedSeconds);