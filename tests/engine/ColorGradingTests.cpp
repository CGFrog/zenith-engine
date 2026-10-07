#include "engine/assets/ColorLut.hpp"
#include "engine/graphics/ColorGrading.hpp"
#include "engine/graphics/RenderConfiguration.hpp"

#include "support/Doctest.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

TEST_SUITE_BEGIN("ColorGrading");

namespace {

bool near(const glm::vec3& actual, const glm::vec3& expected, float tolerance) {
    return glm::all(glm::lessThanEqual(glm::abs(actual - expected), glm::vec3(tolerance)));
}

bool contains(const std::string& text, const char* fragment) {
    return text.find(fragment) != std::string::npos;
}

// A 2^3 table that inverts each channel over a [0, 2] domain.
constexpr const char* invertingCube = R"(# comment line
TITLE "Invert"
LUT_3D_SIZE 2
DOMAIN_MIN 0 0 0
DOMAIN_MAX 2 2 2

1 1 1
0 1 1
1 0 1
0 0 1
1 1 0
0 1 0
1 0 0
0 0 0
)";

TEST_CASE("ColorGrading: parsing") {
    ColorLut lut;
    const ColorLutResult parsed = parseColorLut(invertingCube, lut);
    CHECK_MESSAGE(parsed.success, "a well-formed .cube parses");
    CHECK_MESSAGE((lut.title == "Invert" && lut.size == 2 && lut.texels.size() == 8), "title, size and data are read");
    CHECK_MESSAGE((lut.domainMinimum == glm::vec3(0.0f) && lut.domainMaximum == glm::vec3(2.0f)), "the domain is read");
    CHECK_MESSAGE(
        (lut.texel(1, 0, 0) == glm::vec3(0.0f, 1.0f, 1.0f) && lut.texel(0, 1, 1) == glm::vec3(1.0f, 0.0f, 0.0f)),
        "red varies fastest, then green, then blue");

    ColorLut resolve;
    CHECK_MESSAGE((parseColorLut("LUT_3D_SIZE 2\r\nLUT_3D_INPUT_RANGE -0.5 1.5\r\n" + std::string(8, ' ') +
                                     "\r\n0 0 0\r\n1 0 0\r\n0 1 0\r\n1 1 0\r\n0 0 1\r\n1 0 1\r\n0 1 1\r\n+1 1 1e0\r\n",
                                 resolve)
                       .success &&
                   resolve.domainMinimum == glm::vec3(-0.5f) && resolve.domainMaximum == glm::vec3(1.5f) &&
                   resolve.domainMinimum.x < 0.0f),
                  "Resolve's input range, CRLF line ends, a leading '+' and exponents are accepted");
    CHECK_MESSAGE(near(resolve.texel(1, 1, 1), glm::vec3(1.0f), 0.0f), "the last lattice point is read");
}

void checkRejected(const std::string& text, const char* fragment, const char* message) {
    ColorLut lut;
    lut.size = 7;
    const ColorLutResult result = parseColorLut(text, lut);
    CHECK_MESSAGE((!result.success && contains(result.error, fragment) && lut.size == 7), message);
}

TEST_CASE("ColorGrading: malformed input") {
    checkRejected("0 0 0\n", "before LUT_3D_SIZE", "data before the size is rejected");
    checkRejected("TITLE \"x\"\n", "Missing LUT_3D_SIZE", "a missing size is rejected");
    checkRejected("LUT_3D_SIZE 1\n", "LUT_3D_SIZE must be", "sizes below two are rejected");
    checkRejected("LUT_3D_SIZE 2.5\n", "LUT_3D_SIZE must be", "fractional sizes are rejected");
    checkRejected("LUT_1D_SIZE 16\n", "1D LUTs", "1D tables are rejected");
    checkRejected("LUT_3D_SIZE 2\n0 0 0\n", "Expected 8 data lines", "too few data lines are rejected");
    checkRejected(std::string(invertingCube) + "0 0 0\n", "more data lines", "too many data lines are rejected");
    checkRejected("LUT_3D_SIZE 2\n0 0\n", "Line 2: expected three finite numbers", "short data lines are rejected");
    checkRejected("LUT_3D_SIZE 2\n0 0 zero\n", "expected three finite numbers", "non-numeric data is rejected");
    checkRejected("LUT_3D_SIZE 2\n0 0 0 0\n", "expected three finite numbers", "extra values are rejected");
    checkRejected(
        "LUT_3D_SIZE 2\nDOMAIN_MIN 1 0 0\nDOMAIN_MAX 1 1 1\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 "
        "1 1\n",
        "DOMAIN_MIN must be below", "an empty domain is rejected");
    checkRejected("LUT_3D_SIZE 2\nGAMMA 2.2\n", "unknown keyword 'GAMMA'", "unknown keywords are rejected");
    checkRejected("LUT_3D_SIZE 2\n0 0 0\nTITLE \"late\"\n", "after the data lines", "keywords after data are rejected");

    ColorLut missing;
    const ColorLutResult result = loadColorLut("does/not/exist.cube", missing);
    CHECK_MESSAGE((!result.success && contains(result.error, "does/not/exist.cube")), "a missing file names the path");
}

TEST_CASE("ColorGrading: shipped lut") {
    ColorLut lut;
    const ColorLutResult result =
        loadColorLut(std::filesystem::path(ZENITH_ASSET_DIR) / "color_grading" / "warm_cinematic.cube", lut);
    CHECK_MESSAGE((result.success && lut.size == 17 && lut.texels.size() == 17u * 17u * 17u),
                  "the shipped example LUT parses as a 17^3 table");
    bool inRange = true;
    for (const glm::vec3& texel : lut.texels) {
        inRange = inRange && glm::all(glm::greaterThanEqual(texel, glm::vec3(0.0f))) &&
                  glm::all(glm::lessThanEqual(texel, glm::vec3(1.0f)));
    }
    CHECK_MESSAGE(inRange, "the shipped LUT stays within the display range");
}

TEST_CASE("ColorGrading: lut sampling") {
    const ColorLut identity = identityColorLut(17);
    CHECK_MESSAGE(near(color_grading::sampleColorLut(identity, glm::vec3(0.1f, 0.5f, 0.93f)),
                       glm::vec3(0.1f, 0.5f, 0.93f), 1e-5f),
                  "an identity LUT returns its input under trilinear sampling");
    ColorLut invert;
    parseColorLut(invertingCube, invert);
    CHECK_MESSAGE(
        near(color_grading::sampleColorLut(invert, glm::vec3(0.5f, 1.0f, 1.5f)), glm::vec3(0.75f, 0.5f, 0.25f), 1e-5f),
        "inputs map through the domain before interpolation");
    CHECK_MESSAGE(
        near(color_grading::sampleColorLut(invert, glm::vec3(-1.0f, 3.0f, 2.0f)), glm::vec3(1.0f, 0.0f, 0.0f), 1e-6f),
        "inputs outside the domain clamp to it");
    const glm::vec3 coordinate =
        color_grading::lutTextureCoordinate(glm::vec3(0.0f, 1.0f, 0.5f), glm::vec3(0.0f), glm::vec3(1.0f), 4);
    CHECK_MESSAGE(near(coordinate, glm::vec3(0.125f, 0.875f, 0.5f), 1e-6f),
                  "the domain maps onto the centres of the first and last texels");
}

TEST_CASE("ColorGrading: transfer functions") {
    const glm::vec3 values{0.0f, 0.002f, 0.7f};
    CHECK_MESSAGE(near(color_grading::srgbToLinear(color_grading::linearToSrgb(values)), values, 1e-6f),
                  "the sRGB transfer functions invert each other");
    CHECK_MESSAGE(near(color_grading::linearToSrgb(glm::vec3(0.5f)), glm::vec3(0.735357f), 1e-5f),
                  "linear 0.5 encodes to the sRGB reference value");
}

TEST_CASE("ColorGrading: vignette") {
    VignetteSettings settings{.enabled = true, .intensity = 0.4f, .smoothness = 0.5f, .roundness = 1.0f};
    const float aspect = 16.0f / 9.0f;
    CHECK_MESSAGE(color_grading::vignette(glm::vec2(0.5f), aspect, settings) == 1.0f, "the centre is untouched");
    CHECK_MESSAGE((std::abs(color_grading::vignette(glm::vec2(0.0f), aspect, settings) - 0.6f) < 1e-6f &&
                   std::abs(color_grading::vignette(glm::vec2(1.0f, 0.0f), aspect, settings) - 0.6f) < 1e-6f),
                  "corners keep one minus the intensity");
    const float side = color_grading::vignette(glm::vec2(0.0f, 0.5f), aspect, settings);
    const float top = color_grading::vignette(glm::vec2(0.5f, 0.0f), aspect, settings);
    CHECK_MESSAGE(side < top, "a round vignette darkens the far wide-screen sides more than the near top edge");
    settings.roundness = 0.0f;
    CHECK_MESSAGE(std::abs(color_grading::vignette(glm::vec2(0.0f, 0.5f), aspect, settings) -
                           color_grading::vignette(glm::vec2(0.5f, 0.0f), aspect, settings)) < 1e-6f,
                  "roundness zero follows the frame's aspect");
}

TEST_CASE("ColorGrading: film grain") {
    float sum = 0.0f;
    float minimum = 1.0f;
    float maximum = -1.0f;
    constexpr uint32_t samples = 64;
    for (uint32_t y = 0; y < samples; ++y) {
        for (uint32_t x = 0; x < samples; ++x) {
            const float noise = color_grading::filmGrainNoise(glm::uvec2(x, y), 42);
            sum += noise;
            minimum = std::min(minimum, noise);
            maximum = std::max(maximum, noise);
        }
    }
    CHECK_MESSAGE((minimum >= -0.5f && maximum < 0.5f), "grain stays in [-0.5, 0.5)");
    CHECK_MESSAGE(std::abs(sum / (samples * samples)) < 0.02f, "grain is centred on zero");
    CHECK_MESSAGE(color_grading::filmGrainNoise(glm::uvec2(3, 9), 7) ==
                      color_grading::filmGrainNoise(glm::uvec2(3, 9), 7),
                  "the same cell and frame repeat exactly");
    CHECK_MESSAGE(color_grading::filmGrainNoise(glm::uvec2(3, 9), 7) !=
                      color_grading::filmGrainNoise(glm::uvec2(3, 9), 8),
                  "the pattern changes from frame to frame");
}

TEST_CASE("ColorGrading: apply color grade") {
    RenderSettings settings;
    const glm::vec3 color{0.2f, 0.4f, 0.6f};
    const glm::uvec2 extent{64, 36};
    const ColorLut identity = identityColorLut(8);
    CHECK_MESSAGE(near(color_grading::applyColorGrade(color, {5, 7}, extent, 1, &identity, settings), color, 1e-6f),
                  "every effect is off by default");
    settings.colorLut = {.enabled = true, .path = "grade.cube", .strength = 1.0f};
    CHECK_MESSAGE(near(color_grading::applyColorGrade(color, {5, 7}, extent, 1, &identity, settings), color, 1e-5f),
                  "an identity LUT leaves the image unchanged");
    ColorLut invert;
    parseColorLut(invertingCube, invert);
    settings.colorLut.strength = 0.0f;
    CHECK_MESSAGE(near(color_grading::applyColorGrade(color, {5, 7}, extent, 1, &invert, settings), color, 1e-6f),
                  "zero LUT strength leaves the image unchanged");
    settings.colorLut = {};
    settings.filmGrain = {.enabled = true, .intensity = 0.1f, .size = 2.0f};
    CHECK_MESSAGE(color_grading::applyColorGrade(color, {4, 4}, extent, 1, nullptr, settings) ==
                      color_grading::applyColorGrade(color, {5, 5}, extent, 1, nullptr, settings),
                  "grain cells span the configured size in pixels");
}

TEST_CASE("ColorGrading: settings") {
    RenderSettings defaults;
    CHECK_MESSAGE((!color_grading::lutRequested(defaults.colorLut) &&
                   !color_grading::vignetteActive(defaults.vignette) &&
                   !color_grading::filmGrainActive(defaults.filmGrain)),
                  "colour grading is off by default");
    RenderSettings settings;
    settings.colorLut = {.enabled = true, .path = "color_grading/warm_cinematic.cube", .strength = 0.8f};
    settings.vignette = {.enabled = true, .intensity = 0.35f, .smoothness = 0.6f, .roundness = 0.5f};
    settings.filmGrain = {.enabled = true, .intensity = 0.04f, .size = 2.0f};
    settings.budgets.colorGrade = {0.05, 0.2};
    std::string document;
    RenderSettings restored;
    CHECK_MESSAGE((serializeRenderSettings(settings, document).success &&
                   deserializeRenderSettings(document, restored).success && restored.colorLut.enabled &&
                   restored.colorLut.path == settings.colorLut.path && restored.colorLut.strength == 0.8f &&
                   restored.vignette.roundness == 0.5f && restored.vignette.smoothness == 0.6f &&
                   restored.filmGrain.size == 2.0f && restored.filmGrain.intensity == 0.04f &&
                   restored.budgets.colorGrade.gpuMilliseconds == 0.2),
                  "colour-grading settings and budget round trip through the render-settings document");

    constexpr const char* legacyPrefix =
        R"({"version":1,"antialiasing":1,"shadows":1,"lighting":1,"bloom":0,"environment":0,"localLights":1)";
    RenderSettings legacy;
    CHECK_MESSAGE((deserializeRenderSettings(std::string(legacyPrefix) + "}", legacy).success &&
                   !legacy.colorLut.enabled && !legacy.vignette.enabled && !legacy.filmGrain.enabled),
                  "documents without colour grading load with every effect off");
    RenderSettings partial;
    CHECK_MESSAGE((deserializeRenderSettings(
                       std::string(legacyPrefix) + R"(,"colorGrade":{"vignette":{"enabled":true}}})", partial)
                       .success &&
                   partial.vignette.enabled && partial.vignette.intensity == VignetteSettings{}.intensity &&
                   !partial.filmGrain.enabled),
                  "each colour-grading field is optional");
    RenderSettings invalid;
    CHECK_MESSAGE(
        !deserializeRenderSettings(std::string(legacyPrefix) + R"(,"colorGrade":{"filmGrain":{"size":0.5}}})", invalid)
             .success,
        "out-of-range grain sizes are rejected");
    CHECK_MESSAGE(
        !deserializeRenderSettings(std::string(legacyPrefix) + R"(,"colorGrade":{"colorLut":{"path":3}}})", invalid)
             .success,
        "a non-string LUT path is rejected");
    CHECK_MESSAGE(!deserializeRenderSettings(std::string(legacyPrefix) + R"(,"colorGrade":[]})", invalid).success,
                  "a non-object colour-grading entry is rejected");
}
} // namespace

TEST_SUITE_END();
