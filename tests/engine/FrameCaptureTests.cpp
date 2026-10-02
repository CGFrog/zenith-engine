#include "engine/diagnostics/FrameCaptureConfig.hpp"
#include "engine/diagnostics/ImageComparison.hpp"
#include "engine/diagnostics/RgbaImage.hpp"
#include "engine/graphics/resources/ImageReadback.hpp"

#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <system_error>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

RgbaImage solidImage(uint32_t width, uint32_t height, std::array<uint8_t, 4> color) {
    RgbaImage image{.width = width, .height = height, .pixels = {}};
    for (uint32_t pixel = 0; pixel < width * height; ++pixel) {
        image.pixels.insert(image.pixels.end(), color.begin(), color.end());
    }
    return image;
}

void setPixel(RgbaImage& image, uint32_t x, uint32_t y, std::array<uint8_t, 4> color) {
    const size_t base = (static_cast<size_t>(y) * image.width + x) * RgbaImage::channelCount;
    for (size_t channel = 0; channel < color.size(); ++channel) {
        image.pixels[base + channel] = color[channel];
    }
}

void testIdenticalImagesMatch() {
    const RgbaImage image = solidImage(8, 4, {10, 20, 30, 255});
    const ImageComparison comparison = compareImages(image, image, {});
    expect(comparison.dimensionsMatch, "identical images have matching dimensions");
    expect(comparison.withinTolerance, "identical images match");
    expect(comparison.rootMeanSquareError == 0.0 && comparison.maximumChannelDifference == 0 &&
               comparison.differingPixels == 0,
           "identical images report no difference");
}

void testThresholds() {
    const RgbaImage reference = solidImage(10, 10, {100, 100, 100, 255});
    RgbaImage actual = reference;
    // One pixel off by 50 in red; alpha differences are ignored.
    setPixel(actual, 3, 4, {150, 100, 100, 0});
    ImageComparisonTolerance strict;
    strict.channelTolerance = 2;
    strict.maximumDifferingPixelFraction = 0.0;
    ImageComparison comparison = compareImages(actual, reference, strict);
    expect(comparison.maximumChannelDifference == 50, "maximum channel difference is measured");
    expect(comparison.differingPixels == 1, "one pixel differs");
    expect(std::abs(comparison.differingPixelFraction - 0.01) < 1e-9, "differing fraction is per pixel");
    expect(std::abs(comparison.rootMeanSquareError - std::sqrt(2500.0 / 300.0)) < 1e-9,
           "RMSE averages over every RGB channel");
    expect(!comparison.withinTolerance, "a differing pixel fails a zero-fraction tolerance");

    ImageComparisonTolerance lenient;
    lenient.maximumDifferingPixelFraction = 0.02;
    lenient.maximumRootMeanSquareError = 3.0;
    expect(compareImages(actual, reference, lenient).withinTolerance, "a small differing fraction passes");
    lenient.maximumChannelDifference = 40;
    expect(!compareImages(actual, reference, lenient).withinTolerance, "the maximum channel difference is enforced");

    RgbaImage noisy = reference;
    setPixel(noisy, 0, 0, {102, 98, 100, 255});
    comparison = compareImages(noisy, reference, strict);
    expect(comparison.differingPixels == 0, "differences within the channel tolerance do not count");
    strict.maximumRootMeanSquareError = 0.1;
    expect(!compareImages(noisy, reference, strict).withinTolerance, "RMSE limit is enforced");
}

void testDimensionMismatch() {
    const ImageComparison comparison =
        compareImages(solidImage(4, 4, {0, 0, 0, 255}), solidImage(4, 5, {0, 0, 0, 255}), {});
    expect(!comparison.dimensionsMatch && !comparison.withinTolerance, "different sizes never match");
    expect(!compareImages(RgbaImage{}, RgbaImage{}, {}).withinTolerance, "empty images never match");
    expect(!differenceImage(solidImage(2, 2, {}), solidImage(3, 2, {})).valid(),
           "difference image needs matching sizes");
}

void testDifferenceImage() {
    const RgbaImage reference = solidImage(2, 1, {10, 10, 10, 255});
    RgbaImage actual = reference;
    setPixel(actual, 1, 0, {13, 10, 200, 7});
    const RgbaImage difference = differenceImage(actual, reference, 8);
    expect(difference.valid(), "difference image has the input size");
    expect(difference.pixels[0] == 0 && difference.pixels[3] == 255, "equal pixels are black and opaque");
    expect(difference.pixels[4] == 24 && difference.pixels[6] == 255 && difference.pixels[7] == 255,
           "differences are amplified and clamped");
}

void testPngRoundTrip() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "zenith_frame_capture_tests" / "nested";
    const std::filesystem::path path = directory / "round_trip.png";
    RgbaImage image = solidImage(5, 3, {1, 2, 3, 255});
    setPixel(image, 4, 2, {250, 128, 7, 255});
    const ImageFileResult written = writePng(path, image);
    expect(static_cast<bool>(written), "PNG writes and creates missing directories");
    RgbaImage loaded;
    expect(static_cast<bool>(loadPng(path, loaded)), "written PNG loads");
    expect(loaded.width == 5 && loaded.height == 3 && loaded.pixels == image.pixels, "PNG round trip is lossless");
    expect(!loadPng(directory / "missing.png", loaded), "loading a missing PNG reports an error");
    expect(!writePng(directory / "empty.png", RgbaImage{}), "writing an empty image reports an error");
    std::error_code error;
    std::filesystem::remove_all(directory.parent_path(), error);
}

void testReadbackConversion() {
    // Two texels: BGRA blue-ish and red-ish with non-opaque alpha.
    const std::vector<uint8_t> bgra{200, 50, 10, 0, 1, 2, 250, 128};
    RgbaImage image;
    expect(static_cast<bool>(convertReadbackToRgba(bgra, 2, 1, VK_FORMAT_B8G8R8A8_SRGB, image)),
           "BGRA readback converts");
    expect((image.pixels == std::vector<uint8_t>{10, 50, 200, 255, 250, 2, 1, 255}),
           "BGRA swizzles to RGBA with opaque alpha");
    expect(static_cast<bool>(convertReadbackToRgba(bgra, 2, 1, VK_FORMAT_R8G8B8A8_UNORM, image)),
           "RGBA readback converts");
    expect((image.pixels == std::vector<uint8_t>{200, 50, 10, 255, 1, 2, 250, 255}), "RGBA keeps channel order");
    expect(!convertReadbackToRgba(bgra, 2, 1, VK_FORMAT_A2B10G10R10_UNORM_PACK32, image),
           "unsupported formats report an error");
    expect(!convertReadbackToRgba(bgra, 3, 1, VK_FORMAT_B8G8R8A8_SRGB, image), "size mismatches report an error");
}

void testCaptureConfig() {
    FrameCaptureConfig config = parseFrameCaptureConfig({});
    expect(static_cast<bool>(config) && !config.capture && !config.fixedFrameTime && !config.windowSize,
           "no variables means no capture");

    config = parseFrameCaptureConfig({.captureFrame = "240", .capturePath = "out/frame.png"});
    expect(static_cast<bool>(config) && config.capture && config.capture->frame == 240 &&
               config.capture->outputPath == std::filesystem::path("out/frame.png"),
           "frame and path configure a capture");
    expect(config.fixedFrameTime, "a capture implies a fixed frame time");

    expect(!parseFrameCaptureConfig({.captureFrame = "240"}), "a frame without a path is an error");
    expect(!parseFrameCaptureConfig({.capturePath = "frame.png"}), "a path without a frame is an error");
    expect(!parseFrameCaptureConfig({.captureFrame = "0", .capturePath = "frame.png"}), "frame zero is an error");
    expect(!parseFrameCaptureConfig({.captureFrame = "12x", .capturePath = "frame.png"}),
           "a malformed frame is an error");

    expect(parseFrameCaptureConfig({.fixedFrameTime = "1"}).fixedFrameTime, "fixed frame time can be set alone");
    expect(!parseFrameCaptureConfig({.fixedFrameTime = "0"}).fixedFrameTime, "fixed frame time 0 is off");

    config = parseFrameCaptureConfig({.windowSize = "640x360"});
    expect(static_cast<bool>(config) && config.windowSize && config.windowSize->width == 640 &&
               config.windowSize->height == 360,
           "window size parses");
    expect(!parseFrameCaptureConfig({.windowSize = "640"}), "window size needs both sides");
    expect(!parseFrameCaptureConfig({.windowSize = "8x8"}), "window size has a minimum");
    expect(!parseFrameCaptureConfig({.windowSize = "640x360x2"}), "window size rejects extra text");
}
} // namespace

int main() {
    testIdenticalImagesMatch();
    testThresholds();
    testDimensionMismatch();
    testDifferenceImage();
    testPngRoundTrip();
    testReadbackConversion();
    testCaptureConfig();
    if (failures != 0) {
        std::cerr << failures << " frame capture test(s) failed\n";
        return 1;
    }
    std::cout << "Frame capture tests passed\n";
    return 0;
}
