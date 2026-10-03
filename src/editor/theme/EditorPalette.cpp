#include "editor/theme/EditorPalette.hpp"

#include <algorithm>
#include <cmath>

namespace {
float linearChannel(float encoded) {
    return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
}
} // namespace

float relativeLuminance(ThemeColor color) {
    return 0.2126f * linearChannel(color.r) + 0.7152f * linearChannel(color.g) + 0.0722f * linearChannel(color.b);
}

float contrastRatio(ThemeColor first, ThemeColor second) {
    const float lighter = std::max(relativeLuminance(first), relativeLuminance(second));
    const float darker = std::min(relativeLuminance(first), relativeLuminance(second));
    return (lighter + 0.05f) / (darker + 0.05f);
}

ThemeColor composite(ThemeColor over, ThemeColor under) {
    const float alpha = std::clamp(over.a, 0.0f, 1.0f);
    return {over.r * alpha + under.r * (1.0f - alpha), over.g * alpha + under.g * (1.0f - alpha),
            over.b * alpha + under.b * (1.0f - alpha), 1.0f};
}
