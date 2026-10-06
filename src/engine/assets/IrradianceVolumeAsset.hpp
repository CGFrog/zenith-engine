#pragma once

// Baked irradiance probe volume: a regular grid of probes filling a world-space axis-aligned box.

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

// Baked irradiance probe volume: a regular grid of probes filling a world-space
// axis-aligned box. Probes sit at cell centers, so probe (i, j, k) is at
// minimum + (index + 0.5) * cellSize with cellSize = (maximum - minimum) /
// probeCounts, and none lies on the box boundary. Probes are stored with x
// varying fastest, then y, then z. Distances are world units; +Y is up.
//
// The data is renderer independent: the editor bakes it, scene documents
// reference it by project-relative path, and the renderer uploads it.

// Octahedral texels per side of each probe's depth-moment map.
inline constexpr uint32_t irradianceProbeDepthResolution = 8;
inline constexpr uint32_t irradianceProbeDepthTexels = irradianceProbeDepthResolution * irradianceProbeDepthResolution;
inline constexpr uint32_t maximumIrradianceProbesPerAxis = 32;
inline constexpr uint32_t maximumIrradianceProbes = 4096;

struct IrradianceProbe {
    // Order-2 spherical harmonics of irradiance divided by pi, in the
    // environment_lighting::IrradianceCoefficients convention: evaluating them
    // at a normal yields the Lambertian outgoing radiance of a white surface.
    std::array<glm::vec3, 9> irradiance{};
    // Mean (x) and mean square (y) of the distance from the probe to the nearest
    // surface, per octahedral texel (see octahedralEncode in
    // engine/graphics/IrradianceVolume.hpp), row-major from texel (0, 0).
    std::array<glm::vec2, irradianceProbeDepthTexels> depthMoments{};
    // False when the baker found the probe embedded in geometry; shading skips it.
    bool valid{true};
};

struct IrradianceVolumeData {
    // World-space bounds the probes were baked for.
    glm::vec3 minimum{0.0f};
    glm::vec3 maximum{1.0f};
    glm::uvec3 probeCounts{1u};
    // probeCounts.x * probeCounts.y * probeCounts.z entries.
    std::vector<IrradianceProbe> probes;

    glm::vec3 cellSize() const;
    glm::vec3 probePosition(const glm::uvec3& index) const;
    size_t probeIndex(const glm::uvec3& index) const;
};

struct IrradianceVolumeResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// Checks the counts, bounds, probe array size, and that every value is finite.
IrradianceVolumeResult validateIrradianceVolume(const IrradianceVolumeData& data);

// Versioned little-endian binary encoding (".zvol"). Encoding validates first.
IrradianceVolumeResult encodeIrradianceVolume(const IrradianceVolumeData& data, std::vector<std::byte>& output);
IrradianceVolumeResult decodeIrradianceVolume(std::span<const std::byte> input, IrradianceVolumeData& output);
IrradianceVolumeResult loadIrradianceVolume(const std::filesystem::path& path, IrradianceVolumeData& output);
