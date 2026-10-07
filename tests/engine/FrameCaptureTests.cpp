#include "engine/diagnostics/FrameCaptureConfig.hpp"
#include "engine/diagnostics/ImageComparison.hpp"
#include "engine/diagnostics/RgbaImage.hpp"
#include "engine/graphics/resources/ImageReadback.hpp"

#include "support/Doctest.hpp"

#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <system_error>
#include <vector>

TEST_SUITE_BEGIN("FrameCapture");

namespace {

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

TEST_CASE("FrameCapture: identical images match") {
    const RgbaImage image = solidImage(8, 4, {10, 20, 30, 255});
    const ImageComparison comparison = compareImages(image, image, {});
    CHECK_MESSAGE(comparison.dimensionsMatch, "identical images have matching dimensions");
    CHECK_MESSAGE(comparison.withinTolerance, "identical images match");
    CHECK_MESSAGE((comparison.rootMeanSquareError == 0.0 && comparison.maximumChannelDifference == 0 &&
                   comparison.differingPixels == 0),
                  "identical images report no difference");
}

TEST_CASE("FrameCapture: thresholds") {
    const RgbaImage reference = solidImage(10, 10, {100, 100, 100, 255});
    RgbaImage actual = reference;
    // One pixel off by 50 in red; alpha differences are ignored.
    setPixel(actual, 3, 4, {150, 100, 100, 0});
    ImageComparisonTolerance strict;
    strict.channelTolerance = 2;
    strict.maximumDifferingPixelFraction = 0.0;
    ImageComparison comparison = compareImages(actual, reference, strict);
    CHECK_MESSAGE(comparison.maximumChannelDifference == 50, "maximum channel difference is measured");
    CHECK_MESSAGE(comparison.differingPixels == 1, "one pixel differs");
    CHECK_MESSAGE(std::abs(comparison.differingPixelFraction - 0.01) < 1e-9, "differing fraction is per pixel");
    CHECK_MESSAGE(std::abs(comparison.rootMeanSquareError - std::sqrt(2500.0 / 300.0)) < 1e-9,
                  "RMSE averages over every RGB channel");
    CHECK_MESSAGE(!comparison.withinTolerance, "a differing pixel fails a zero-fraction tolerance");

    ImageComparisonTolerance lenient;
    lenient.maximumDifferingPixelFraction = 0.02;
    lenient.maximumRootMeanSquareError = 3.0;
    CHECK_MESSAGE(compareImages(actual, reference, lenient).withinTolerance, "a small differing fraction passes");
    lenient.maximumChannelDifference = 40;
    CHECK_MESSAGE(!compareImages(actual, reference, lenient).withinTolerance,
                  "the maximum channel difference is enforced");

    RgbaImage noisy = reference;
    setPixel(noisy, 0, 0, {102, 98, 100, 255});
    comparison = compareImages(noisy, reference, strict);
    CHECK_MESSAGE(comparison.differingPixels == 0, "differences within the channel tolerance do not count");
    strict.maximumRootMeanSquareError = 0.1;
    CHECK_MESSAGE(!compareImages(noisy, reference, strict).withinTolerance, "RMSE limit is enforced");
}

TEST_CASE("FrameCapture: dimension mismatch") {
    const ImageComparison comparison =
        compareImages(solidImage(4, 4, {0, 0, 0, 255}), solidImage(4, 5, {0, 0, 0, 255}), {});
    CHECK_MESSAGE((!comparison.dimensionsMatch && !comparison.withinTolerance), "different sizes never match");
    CHECK_MESSAGE(!compareImages(RgbaImage{}, RgbaImage{}, {}).withinTolerance, "empty images never match");
    CHECK_MESSAGE(!differenceImage(solidImage(2, 2, {}), solidImage(3, 2, {})).valid(),
                  "difference image needs matching sizes");
}

TEST_CASE("FrameCapture: difference image") {
    const RgbaImage reference = solidImage(2, 1, {10, 10, 10, 255});
    RgbaImage actual = reference;
    setPixel(actual, 1, 0, {13, 10, 200, 7});
    const RgbaImage difference = differenceImage(actual, reference, 8);
    CHECK_MESSAGE(difference.valid(), "difference image has the input size");
    CHECK_MESSAGE((difference.pixels[0] == 0 && difference.pixels[3] == 255), "equal pixels are black and opaque");
    CHECK_MESSAGE((difference.pixels[4] == 24 && difference.pixels[6] == 255 && difference.pixels[7] == 255),
                  "differences are amplified and clamped");
}

TEST_CASE("FrameCapture: png round trip") {
    const testsupport::TempDirectory root("frame-capture");
    const std::filesystem::path directory = root / "nested";
    const std::filesystem::path path = directory / "round_trip.png";
    RgbaImage image = solidImage(5, 3, {1, 2, 3, 255});
    setPixel(image, 4, 2, {250, 128, 7, 255});
    const ImageFileResult written = writePng(path, image);
    CHECK_MESSAGE((static_cast<bool>(written)), "PNG writes and creates missing directories");
    RgbaImage loaded;
    CHECK_MESSAGE((static_cast<bool>(loadPng(path, loaded))), "written PNG loads");
    CHECK_MESSAGE((loaded.width == 5 && loaded.height == 3 && loaded.pixels == image.pixels),
                  "PNG round trip is lossless");
    CHECK_MESSAGE(!loadPng(directory / "missing.png", loaded), "loading a missing PNG reports an error");
    CHECK_MESSAGE(!writePng(directory / "empty.png", RgbaImage{}), "writing an empty image reports an error");
}

TEST_CASE("FrameCapture: readback conversion") {
    // Two texels: BGRA blue-ish and red-ish with non-opaque alpha.
    const std::vector<uint8_t> bgra{200, 50, 10, 0, 1, 2, 250, 128};
    RgbaImage image;
    CHECK_MESSAGE((static_cast<bool>(convertReadbackToRgba(bgra, 2, 1, VK_FORMAT_B8G8R8A8_SRGB, image))),
                  "BGRA readback converts");
    CHECK_MESSAGE((image.pixels == std::vector<uint8_t>{10, 50, 200, 255, 250, 2, 1, 255}),
                  "BGRA swizzles to RGBA with opaque alpha");
    CHECK_MESSAGE((static_cast<bool>(convertReadbackToRgba(bgra, 2, 1, VK_FORMAT_R8G8B8A8_UNORM, image))),
                  "RGBA readback converts");
    CHECK_MESSAGE((image.pixels == std::vector<uint8_t>{200, 50, 10, 255, 1, 2, 250, 255}), "RGBA keeps channel order");
    CHECK_MESSAGE(!convertReadbackToRgba(bgra, 2, 1, VK_FORMAT_A2B10G10R10_UNORM_PACK32, image),
                  "unsupported formats report an error");
    CHECK_MESSAGE(!convertReadbackToRgba(bgra, 3, 1, VK_FORMAT_B8G8R8A8_SRGB, image),
                  "size mismatches report an error");
}

TEST_CASE("FrameCapture: capture config") {
    FrameCaptureConfig config = parseFrameCaptureConfig({});
    CHECK_MESSAGE((static_cast<bool>(config) && !config.capture && !config.fixedFrameTime && !config.windowSize),
                  "no variables means no capture");

    config = parseFrameCaptureConfig({.captureFrame = "240", .capturePath = "out/frame.png"});
    CHECK_MESSAGE((static_cast<bool>(config) && config.capture && config.capture->frame == 240 &&
                   config.capture->outputPath == std::filesystem::path("out/frame.png")),
                  "frame and path configure a capture");
    CHECK_MESSAGE(config.fixedFrameTime, "a capture implies a fixed frame time");

    CHECK_MESSAGE(!parseFrameCaptureConfig({.captureFrame = "240"}), "a frame without a path is an error");
    CHECK_MESSAGE(!parseFrameCaptureConfig({.capturePath = "frame.png"}), "a path without a frame is an error");
    CHECK_MESSAGE(!parseFrameCaptureConfig({.captureFrame = "0", .capturePath = "frame.png"}),
                  "frame zero is an error");
    CHECK_MESSAGE(!parseFrameCaptureConfig({.captureFrame = "12x", .capturePath = "frame.png"}),
                  "a malformed frame is an error");

    CHECK_MESSAGE(parseFrameCaptureConfig({.fixedFrameTime = "1"}).fixedFrameTime, "fixed frame time can be set alone");
    CHECK_MESSAGE(!parseFrameCaptureConfig({.fixedFrameTime = "0"}).fixedFrameTime, "fixed frame time 0 is off");

    config = parseFrameCaptureConfig({.windowSize = "640x360"});
    CHECK_MESSAGE((static_cast<bool>(config) && config.windowSize && config.windowSize->width == 640 &&
                   config.windowSize->height == 360),
                  "window size parses");
    CHECK_MESSAGE(!parseFrameCaptureConfig({.windowSize = "640"}), "window size needs both sides");
    CHECK_MESSAGE(!parseFrameCaptureConfig({.windowSize = "8x8"}), "window size has a minimum");
    CHECK_MESSAGE(!parseFrameCaptureConfig({.windowSize = "640x360x2"}), "window size rejects extra text");
}
} // namespace

TEST_SUITE_END();
