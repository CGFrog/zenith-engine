// CPU reference renders of shading features (colour space, shadows, skinning, UI, ...), each
// compared with the hash of its approved image.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

#include "support/Doctest.hpp"

namespace {
constexpr int width = 48;
constexpr int height = 32;
using Image = std::vector<uint8_t>;

uint8_t toByte(float value) {
    return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

uint64_t hash(const Image& image) {
    uint64_t value = 1469598103934665603ull;
    for (uint8_t byte : image) {
        value = (value ^ byte) * 1099511628211ull;
    }
    return value;
}

void writePpm(const std::filesystem::path& path, const Image& image) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "P6\n" << width << ' ' << height << "\n255\n";
    output.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
}

// Checks the capture's hash; a mismatch keeps the capture as a PPM for
// inspection (testsupport::failureArtifactPath) and names it.
void checkCapture(std::string_view name, const Image& image, uint64_t expected) {
    const uint64_t actual = hash(image);
    if (actual != expected) {
        const std::filesystem::path output =
            testsupport::failureArtifactPath("render-regression-" + std::string(name) + ".ppm");
        writePpm(output, image);
        MESSAGE("the capture is in " << output.string());
    }
    CHECK_MESSAGE(actual == expected, (std::string(name) + " screenshot hash"));
}

Image colorSpaceCapture() {
    Image image(width * height * 3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float linear = static_cast<float>(x) / (width - 1);
            const float srgb = linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
            const size_t pixel = static_cast<size_t>(y * width + x) * 3;
            image[pixel] = toByte(srgb);
            image[pixel + 1] = toByte(std::sqrt(linear));
            image[pixel + 2] = toByte(linear);
        }
    }
    return image;
}

Image shadowCapture() {
    Image image(width * height * 3);
    constexpr std::array<std::array<float, 3>, 3> colors{
        {{1.0f, 0.35f, 0.35f}, {0.35f, 1.0f, 0.35f}, {0.35f, 0.55f, 1.0f}}};
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int cascade = std::min(x / 16, 2);
            const float receiver = 0.35f + 0.65f * static_cast<float>(y) / (height - 1);
            const size_t pixel = static_cast<size_t>(y * width + x) * 3;
            for (int channel = 0; channel < 3; ++channel) {
                image[pixel + channel] = toByte(receiver * colors[cascade][channel]);
            }
        }
    }
    return image;
}

Image antialiasingCapture() {
    Image image(width * height * 3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float signedDistance = static_cast<float>(x) - (0.7f * y + 10.0f);
            const float coverage = std::clamp(0.5f - signedDistance, 0.0f, 1.0f);
            const uint8_t value = toByte(coverage);
            const size_t pixel = static_cast<size_t>(y * width + x) * 3;
            image[pixel] = value;
            image[pixel + 1] = value;
            image[pixel + 2] = value;
        }
    }
    return image;
}

Image gameUiCapture() {
    Image image(width * height * 3);
    const std::array<float, 3> sceneColor{0.18f, 0.24f, 0.32f};
    const std::array<float, 4> panelColor{0.025f, 0.035f, 0.055f, 0.94f};
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            std::array<float, 3> color = sceneColor;
            if (x >= 10 && x < 38 && y >= 5 && y < 27) {
                for (size_t channel = 0; channel < color.size(); ++channel) {
                    color[channel] = panelColor[channel] * panelColor[3] + color[channel] * (1.0f - panelColor[3]);
                }
            }
            if ((x >= 23 && x <= 24 && y >= 12 && y <= 20) || (y >= 15 && y <= 16 && x >= 19 && x <= 28)) {
                color = {1.0f, 1.0f, 1.0f};
            }
            const size_t pixel = static_cast<size_t>(y * width + x) * 3;
            for (size_t channel = 0; channel < color.size(); ++channel) {
                image[pixel + channel] = toByte(color[channel]);
            }
        }
    }
    return image;
}

Image skinningCapture() {
    Image image(width * height * 3);
    for (int y = 0; y < height; ++y) {
        const float jointWeight = static_cast<float>(y) / (height - 1);
        const int center = 16 + static_cast<int>(std::lround(12.0f * jointWeight * jointWeight));
        for (int x = 0; x < width; ++x) {
            const float coverage = std::clamp(2.5f - std::abs(static_cast<float>(x - center)), 0.0f, 1.0f);
            const size_t pixel = static_cast<size_t>(y * width + x) * 3;
            image[pixel] = toByte(0.2f * coverage);
            image[pixel + 1] = toByte(0.75f * coverage);
            image[pixel + 2] = toByte(1.0f * coverage);
        }
    }
    return image;
}

Image maskedEdgeCapture() {
    Image image(width * height * 3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float alpha = static_cast<float>((x * 13 + y * 7) % 17) / 16.0f;
            const uint8_t value = alpha >= 0.5f ? 230 : 26;
            const size_t pixel = static_cast<size_t>(y * width + x) * 3;
            image[pixel] = value;
            image[pixel + 1] = alpha >= 0.5f ? 190 : 26;
            image[pixel + 2] = alpha >= 0.5f ? 64 : 26;
        }
    }
    return image;
}

Image blendingOrderCapture() {
    Image image(width * height * 3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            std::array<float, 3> color{0.08f, 0.1f, 0.15f};
            if (x >= 8 && x < 38 && y >= 5 && y < 27) {
                for (size_t channel = 0; channel < color.size(); ++channel) {
                    color[channel] = std::array<float, 3>{0.1f, 0.3f, 1.0f}[channel] * 0.45f + color[channel] * 0.55f;
                }
            }
            if (x >= 18 && x < 45 && y >= 9 && y < 30) {
                for (size_t channel = 0; channel < color.size(); ++channel) {
                    color[channel] = std::array<float, 3>{1.0f, 0.2f, 0.1f}[channel] * 0.6f + color[channel] * 0.4f;
                }
            }
            const size_t pixel = static_cast<size_t>(y * width + x) * 3;
            for (size_t channel = 0; channel < color.size(); ++channel) {
                image[pixel + channel] = toByte(color[channel]);
            }
        }
    }
    return image;
}

Image localLightsCapture() {
    Image image(width * height * 3);
    const std::array<std::array<float, 2>, 3> centers{{{10.0f, 10.0f}, {25.0f, 22.0f}, {39.0f, 9.0f}}};
    const std::array<std::array<float, 3>, 3> colors{
        {{1.0f, 0.15f, 0.08f}, {0.08f, 0.85f, 0.25f}, {0.12f, 0.3f, 1.0f}}};
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            std::array<float, 3> color{0.015f, 0.02f, 0.03f};
            for (size_t light = 0; light < centers.size(); ++light) {
                const float dx = x - centers[light][0];
                const float dy = y - centers[light][1];
                const float attenuation = std::max(1.0f - std::sqrt(dx * dx + dy * dy) / 13.0f, 0.0f);
                for (size_t channel = 0; channel < color.size(); ++channel) {
                    color[channel] += colors[light][channel] * attenuation * attenuation;
                }
            }
            const size_t pixel = static_cast<size_t>(y * width + x) * 3;
            for (size_t channel = 0; channel < color.size(); ++channel) {
                image[pixel + channel] = toByte(color[channel]);
            }
        }
    }
    return image;
}

Image reflectionProbeCapture() {
    Image image(width * height * 3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float blend = std::clamp((static_cast<float>(x) - 14.0f) / 20.0f, 0.0f, 1.0f);
            const float normal = 0.35f + 0.65f * static_cast<float>(y) / (height - 1);
            const std::array<float, 3> interior{0.9f, 0.45f, 0.12f};
            const std::array<float, 3> exterior{0.12f, 0.38f, 0.95f};
            const size_t pixel = static_cast<size_t>(y * width + x) * 3;
            for (size_t channel = 0; channel < 3; ++channel) {
                image[pixel + channel] =
                    toByte((interior[channel] * (1.0f - blend) + exterior[channel] * blend) * normal);
            }
        }
    }
    return image;
}

Image hdrEnvironmentCapture() {
    Image image(width * height * 3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float horizon = std::abs(static_cast<float>(y) / (height - 1) - 0.5f) * 2.0f;
            const float source = 0.25f + (1.0f - horizon) * 5.0f + static_cast<float>(x) / (width - 1) * 2.0f;
            const std::array<float, 3> hdr{source * 0.55f, source * 0.72f, source};
            const size_t pixel = static_cast<size_t>(y * width + x) * 3;
            for (size_t channel = 0; channel < 3; ++channel) {
                const float toneMapped = hdr[channel] / (hdr[channel] + 1.0f);
                image[pixel + channel] = toByte(std::pow(toneMapped, 1.0f / 2.2f));
            }
        }
    }
    return image;
}

Image qualityFallbackCapture() {
    Image image(width * height * 3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const bool compatibleEnvironment = y < height / 2;
            const bool boundedLights = x < width / 2;
            const std::array<float, 3> color = compatibleEnvironment
                                                   ? (boundedLights ? std::array<float, 3>{0.22f, 0.34f, 0.52f}
                                                                    : std::array<float, 3>{0.48f, 0.22f, 0.15f})
                                                   : (boundedLights ? std::array<float, 3>{0.14f, 0.42f, 0.24f}
                                                                    : std::array<float, 3>{0.30f, 0.30f, 0.30f});
            const size_t pixel = static_cast<size_t>(y * width + x) * 3;
            for (size_t channel = 0; channel < 3; ++channel) {
                image[pixel + channel] = toByte(color[channel]);
            }
        }
    }
    return image;
}

Image productionAssetFallbackCapture() {
    Image image(width * height * 3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float morph = static_cast<float>(x) / (width - 1);
            const float basisFallback = static_cast<float>(y) / (height - 1);
            const size_t pixel = static_cast<size_t>(y * width + x) * 3;
            image[pixel] = toByte(morph);
            image[pixel + 1] = toByte((1.0f - morph) * basisFallback);
            image[pixel + 2] = toByte(1.0f - basisFallback * 0.75f);
        }
    }
    return image;
}
} // namespace

TEST_SUITE_BEGIN("RenderRegression");

TEST_CASE("RenderRegression: color space capture matches its reference") {
    checkCapture("color-space", colorSpaceCapture(), 11920415357468837699ull);
}

TEST_CASE("RenderRegression: shadow capture matches its reference") {
    checkCapture("shadow", shadowCapture(), 9332500078490701299ull);
}

TEST_CASE("RenderRegression: antialiasing capture matches its reference") {
    checkCapture("antialiasing", antialiasingCapture(), 4834795595963545455ull);
}

TEST_CASE("RenderRegression: game ui capture matches its reference") {
    checkCapture("game-ui", gameUiCapture(), 2202593522406481215ull);
}

TEST_CASE("RenderRegression: skinning capture matches its reference") {
    checkCapture("skinning", skinningCapture(), 7791851272260930333ull);
}

TEST_CASE("RenderRegression: masked edge capture matches its reference") {
    checkCapture("masked-edge", maskedEdgeCapture(), 10485245353354089817ull);
}

TEST_CASE("RenderRegression: blending order capture matches its reference") {
    checkCapture("blending-order", blendingOrderCapture(), 12337063544351879896ull);
}

TEST_CASE("RenderRegression: local lights capture matches its reference") {
    checkCapture("local-lights", localLightsCapture(), 18186517676014945946ull);
}

TEST_CASE("RenderRegression: reflection probes capture matches its reference") {
    checkCapture("reflection-probes", reflectionProbeCapture(), 14217588637060545381ull);
}

TEST_CASE("RenderRegression: hdr environment capture matches its reference") {
    checkCapture("hdr-environment", hdrEnvironmentCapture(), 9140372891774060003ull);
}

TEST_CASE("RenderRegression: quality fallbacks capture matches its reference") {
    checkCapture("quality-fallbacks", qualityFallbackCapture(), 7156661786147691523ull);
}

TEST_CASE("RenderRegression: production asset fallbacks capture matches its reference") {
    checkCapture("production-asset-fallbacks", productionAssetFallbackCapture(), 6757895526051028569ull);
}

TEST_SUITE_END();
