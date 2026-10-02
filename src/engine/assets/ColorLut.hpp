#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// A three-dimensional colour lookup table in the Adobe Cube LUT (".cube")
// convention, also written by DaVinci Resolve. Input colours inside
// [domainMinimum, domainMaximum] map to output colours by trilinear
// interpolation of size^3 lattice points; inputs outside the domain clamp to
// it. Lattice points are stored with red varying fastest, then green, then
// blue, which is the order of a .cube file's data lines.
//
// The table is colour-space agnostic data: whoever applies it decides which
// encoding its inputs and outputs use (the renderer applies it to
// display-encoded sRGB values). Parsing is independent of any GPU resource.
struct ColorLut {
    // Optional TITLE line; empty when absent.
    std::string title;
    uint32_t size{0};
    glm::vec3 domainMinimum{0.0f};
    glm::vec3 domainMaximum{1.0f};
    // size^3 entries.
    std::vector<glm::vec3> texels;

    const glm::vec3& texel(uint32_t red, uint32_t green, uint32_t blue) const {
        return texels[(static_cast<size_t>(blue) * size + green) * size + red];
    }
};

// Lattice sizes the Cube LUT specification allows for 3D tables.
inline constexpr uint32_t minimumColorLutSize = 2;
inline constexpr uint32_t maximumColorLutSize = 256;

struct ColorLutResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// Parses the text of a .cube file. Supports TITLE, LUT_3D_SIZE, DOMAIN_MIN,
// DOMAIN_MAX, Resolve's LUT_3D_INPUT_RANGE, and '#' comments. Keywords must
// precede the data lines. 1D tables, unknown keywords, a missing size, a data
// line count other than size^3, non-finite values, and an empty or inverted
// domain are rejected with an error naming the offending line.
ColorLutResult parseColorLut(std::string_view text, ColorLut& output);
ColorLutResult loadColorLut(const std::filesystem::path& path, ColorLut& output);

// A table that maps every input in [0, 1] to itself.
ColorLut identityColorLut(uint32_t size);
