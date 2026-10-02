#include "ImageComparison.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace {
constexpr uint32_t comparedChannels = 3;

uint32_t channelDifference(uint8_t first, uint8_t second) {
    return static_cast<uint32_t>(std::abs(static_cast<int>(first) - static_cast<int>(second)));
}

bool sameDimensions(const RgbaImage& first, const RgbaImage& second) {
    return first.valid() && second.valid() && first.width == second.width && first.height == second.height;
}
} // namespace

std::string ImageComparison::summary() const {
    if (!dimensionsMatch) {
        return "dimensions differ or an image is empty";
    }
    std::ostringstream text;
    text.precision(4);
    text << "rmse " << rootMeanSquareError << ", max channel difference " << maximumChannelDifference
         << ", differing pixels " << differingPixels << " (" << differingPixelFraction * 100.0 << "%)";
    return text.str();
}

ImageComparison compareImages(const RgbaImage& actual, const RgbaImage& reference,
                              const ImageComparisonTolerance& tolerance) {
    ImageComparison result;
    result.dimensionsMatch = sameDimensions(actual, reference);
    if (!result.dimensionsMatch) {
        return result;
    }
    const size_t pixelCount = static_cast<size_t>(actual.width) * actual.height;
    double squaredErrorSum = 0.0;
    for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
        const size_t base = pixel * RgbaImage::channelCount;
        bool differs = false;
        for (uint32_t channel = 0; channel < comparedChannels; ++channel) {
            const uint32_t difference =
                channelDifference(actual.pixels[base + channel], reference.pixels[base + channel]);
            squaredErrorSum += static_cast<double>(difference) * difference;
            result.maximumChannelDifference = std::max(result.maximumChannelDifference, difference);
            differs = differs || difference > tolerance.channelTolerance;
        }
        if (differs) {
            ++result.differingPixels;
        }
    }
    result.rootMeanSquareError = std::sqrt(squaredErrorSum / static_cast<double>(pixelCount * comparedChannels));
    result.differingPixelFraction = static_cast<double>(result.differingPixels) / static_cast<double>(pixelCount);
    result.withinTolerance = result.maximumChannelDifference <= tolerance.maximumChannelDifference &&
                             result.rootMeanSquareError <= tolerance.maximumRootMeanSquareError &&
                             result.differingPixelFraction <= tolerance.maximumDifferingPixelFraction;
    return result;
}

RgbaImage differenceImage(const RgbaImage& actual, const RgbaImage& reference, uint32_t gain) {
    if (!sameDimensions(actual, reference)) {
        return {};
    }
    RgbaImage result{.width = actual.width, .height = actual.height, .pixels = {}};
    result.pixels.resize(actual.pixels.size());
    for (size_t base = 0; base < actual.pixels.size(); base += RgbaImage::channelCount) {
        for (uint32_t channel = 0; channel < comparedChannels; ++channel) {
            const uint32_t difference =
                channelDifference(actual.pixels[base + channel], reference.pixels[base + channel]);
            result.pixels[base + channel] = static_cast<uint8_t>(std::min(difference * gain, 255u));
        }
        result.pixels[base + 3] = 255;
    }
    return result;
}
