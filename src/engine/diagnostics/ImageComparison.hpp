#pragma once

// Thresholds for comparing a rendered capture against a reference.

#include "RgbaImage.hpp"

#include <string>

// Thresholds for comparing a rendered capture against a reference. Differences
// are measured per RGB channel in 8-bit encoded units (0-255); alpha is ignored
// because presented frames are opaque. Every limit must hold for a match.
struct ImageComparisonTolerance {
    // A channel difference at or below this counts as equal when counting
    // differing pixels.
    uint32_t channelTolerance{2};
    // Largest allowed absolute difference of any single channel.
    uint32_t maximumChannelDifference{255};
    // Root-mean-square error over every RGB channel of every pixel.
    double maximumRootMeanSquareError{1.0};
    // Fraction (0-1) of pixels allowed to have a channel beyond channelTolerance.
    double maximumDifferingPixelFraction{0.001};
};

struct ImageComparison {
    bool dimensionsMatch{false};
    double rootMeanSquareError{0.0};
    uint32_t maximumChannelDifference{0};
    uint64_t differingPixels{0};
    double differingPixelFraction{0.0};
    bool withinTolerance{false};

    // One line describing the measurements, for logs and test output.
    std::string summary() const;
};

ImageComparison compareImages(const RgbaImage& actual, const RgbaImage& reference,
                              const ImageComparisonTolerance& tolerance);

// Per-pixel absolute RGB difference scaled by `gain` and clamped, with opaque
// alpha, for inspecting a failed comparison. Requires matching dimensions;
// returns an empty image otherwise.
RgbaImage differenceImage(const RgbaImage& actual, const RgbaImage& reference, uint32_t gain = 8);
