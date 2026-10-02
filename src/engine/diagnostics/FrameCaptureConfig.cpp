#include "FrameCaptureConfig.hpp"

#include <SDL3/SDL_stdinc.h>
#include <charconv>
#include <string_view>

namespace {
constexpr uint32_t minimumWindowSide = 16;
constexpr uint32_t maximumWindowSide = 16384;

bool present(const char* value) {
    return value != nullptr && *value != '\0';
}

template <typename Integer> bool parseWhole(std::string_view text, Integer& value) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

std::optional<WindowSizeOverride> parseWindowSize(std::string_view text) {
    const size_t separator = text.find('x');
    WindowSizeOverride size;
    if (separator == std::string_view::npos || !parseWhole(text.substr(0, separator), size.width) ||
        !parseWhole(text.substr(separator + 1), size.height)) {
        return std::nullopt;
    }
    const auto inRange = [](uint32_t side) { return side >= minimumWindowSide && side <= maximumWindowSide; };
    if (!inRange(size.width) || !inRange(size.height)) {
        return std::nullopt;
    }
    return size;
}
} // namespace

FrameCaptureConfig parseFrameCaptureConfig(const FrameCaptureEnvironment& environment) {
    FrameCaptureConfig config;
    config.fixedFrameTime = present(environment.fixedFrameTime) && std::string_view(environment.fixedFrameTime) != "0";
    if (present(environment.windowSize)) {
        config.windowSize = parseWindowSize(environment.windowSize);
        if (!config.windowSize) {
            config.error = "ZENITH_WINDOW_SIZE must be WIDTHxHEIGHT with sides from 16 to 16384, got '" +
                           std::string(environment.windowSize) + "'";
            return config;
        }
    }
    if (!present(environment.captureFrame) && !present(environment.capturePath)) {
        return config;
    }
    if (!present(environment.captureFrame) || !present(environment.capturePath)) {
        config.error = "ZENITH_CAPTURE_FRAME and ZENITH_CAPTURE_PATH must be set together";
        return config;
    }
    uint64_t frame = 0;
    if (!parseWhole(std::string_view(environment.captureFrame), frame) || frame == 0) {
        config.error =
            "ZENITH_CAPTURE_FRAME must be a positive frame number, got '" + std::string(environment.captureFrame) + "'";
        return config;
    }
    config.capture = FrameCaptureRequest{.frame = frame, .outputPath = std::filesystem::path(environment.capturePath)};
    config.fixedFrameTime = true;
    return config;
}

FrameCaptureConfig frameCaptureConfigFromEnvironment() {
    return parseFrameCaptureConfig({.captureFrame = SDL_getenv("ZENITH_CAPTURE_FRAME"),
                                    .capturePath = SDL_getenv("ZENITH_CAPTURE_PATH"),
                                    .fixedFrameTime = SDL_getenv("ZENITH_FIXED_FRAME_TIME"),
                                    .windowSize = SDL_getenv("ZENITH_WINDOW_SIZE")});
}
