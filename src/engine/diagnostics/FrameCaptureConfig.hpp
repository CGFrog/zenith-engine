#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

// Writes the presented image of one frame to a PNG file.
struct FrameCaptureRequest {
    // 1-based count of frames the engine loop has run; frame 1 is the first.
    uint64_t frame{0};
    std::filesystem::path outputPath;
};

// Window size in SDL window units (equal to pixels without display scaling).
struct WindowSizeOverride {
    uint32_t width{0};
    uint32_t height{0};
};

// Diagnostic runtime options for deterministic visual checks, built from the
// ZENITH_CAPTURE_FRAME, ZENITH_CAPTURE_PATH, ZENITH_FIXED_FRAME_TIME and
// ZENITH_WINDOW_SIZE environment variables (docs/RENDERER_VALIDATION.md).
struct FrameCaptureConfig {
    std::optional<FrameCaptureRequest> capture;
    // Advance simulation, animation, and exposure adaptation by the physics
    // fixed step every frame instead of by wall-clock time. Implied by a
    // capture so its result does not depend on frame pacing.
    bool fixedFrameTime{false};
    std::optional<WindowSizeOverride> windowSize;
    std::string error;

    explicit operator bool() const {
        return error.empty();
    }
};

struct FrameCaptureEnvironment {
    // Raw environment values; null when unset.
    const char* captureFrame{nullptr};
    const char* capturePath{nullptr};
    const char* fixedFrameTime{nullptr};
    const char* windowSize{nullptr};
};

// A capture needs both a positive frame number and a path; giving only one is
// an error. The fixed frame time switch is on when its value is present and
// not "0". The window size is "WIDTHxHEIGHT" with each side in [16, 16384].
FrameCaptureConfig parseFrameCaptureConfig(const FrameCaptureEnvironment& environment);

FrameCaptureConfig frameCaptureConfigFromEnvironment();
