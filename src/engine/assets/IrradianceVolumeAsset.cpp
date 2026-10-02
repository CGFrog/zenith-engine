#include "IrradianceVolumeAsset.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

namespace {
constexpr std::array<char, 4> magic{'Z', 'V', 'O', 'L'};
constexpr uint32_t formatVersion = 1;

static_assert(std::endian::native == std::endian::little, "The .zvol encoding assumes a little-endian host");

class ByteWriter {
  public:
    explicit ByteWriter(std::vector<std::byte>& output)
        : m_output(output) {}

    template <typename Value> void write(const Value& value) {
        const auto* bytes = reinterpret_cast<const std::byte*>(&value);
        m_output.insert(m_output.end(), bytes, bytes + sizeof(Value));
    }

  private:
    std::vector<std::byte>& m_output;
};

class ByteReader {
  public:
    explicit ByteReader(std::span<const std::byte> input)
        : m_input(input) {}

    template <typename Value> bool read(Value& value) {
        if (m_input.size() - m_offset < sizeof(Value)) {
            return false;
        }
        std::memcpy(&value, m_input.data() + m_offset, sizeof(Value));
        m_offset += sizeof(Value);
        return true;
    }

    bool finished() const {
        return m_offset == m_input.size();
    }

  private:
    std::span<const std::byte> m_input;
    size_t m_offset{0};
};

bool finite(const glm::vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool finiteProbe(const IrradianceProbe& probe) {
    const bool irradianceFinite =
        std::ranges::all_of(probe.irradiance, [](const glm::vec3& coefficient) { return finite(coefficient); });
    const bool depthFinite = std::ranges::all_of(probe.depthMoments, [](const glm::vec2& moments) {
        return std::isfinite(moments.x) && std::isfinite(moments.y) && moments.x >= 0.0f && moments.y >= 0.0f;
    });
    return irradianceFinite && depthFinite;
}
} // namespace

glm::vec3 IrradianceVolumeData::cellSize() const {
    return (maximum - minimum) / glm::vec3(glm::max(probeCounts, glm::uvec3(1u)));
}

glm::vec3 IrradianceVolumeData::probePosition(const glm::uvec3& index) const {
    return minimum + (glm::vec3(index) + 0.5f) * cellSize();
}

size_t IrradianceVolumeData::probeIndex(const glm::uvec3& index) const {
    return index.x + static_cast<size_t>(probeCounts.x) * (index.y + static_cast<size_t>(probeCounts.y) * index.z);
}

IrradianceVolumeResult validateIrradianceVolume(const IrradianceVolumeData& data) {
    for (int axis = 0; axis < 3; ++axis) {
        if (data.probeCounts[axis] == 0 || data.probeCounts[axis] > maximumIrradianceProbesPerAxis) {
            return {.error = "Irradiance volume probe counts must be between 1 and " +
                             std::to_string(maximumIrradianceProbesPerAxis) + " per axis"};
        }
    }
    const size_t probeCount = static_cast<size_t>(data.probeCounts.x) * data.probeCounts.y * data.probeCounts.z;
    if (probeCount > maximumIrradianceProbes) {
        return {.error = "Irradiance volume exceeds " + std::to_string(maximumIrradianceProbes) + " probes"};
    }
    if (!finite(data.minimum) || !finite(data.maximum) || glm::any(glm::lessThanEqual(data.maximum, data.minimum))) {
        return {.error = "Irradiance volume bounds must be finite with maximum above minimum on every axis"};
    }
    if (data.probes.size() != probeCount) {
        return {.error = "Irradiance volume holds " + std::to_string(data.probes.size()) +
                         " probes but its counts need " + std::to_string(probeCount)};
    }
    if (!std::ranges::all_of(data.probes, finiteProbe)) {
        return {.error = "Irradiance volume contains non-finite or negative probe values"};
    }
    return {.success = true};
}

IrradianceVolumeResult encodeIrradianceVolume(const IrradianceVolumeData& data, std::vector<std::byte>& output) {
    if (IrradianceVolumeResult result = validateIrradianceVolume(data); !result) {
        return result;
    }
    output.clear();
    ByteWriter writer(output);
    writer.write(magic);
    writer.write(formatVersion);
    writer.write(irradianceProbeDepthResolution);
    writer.write(data.probeCounts);
    writer.write(data.minimum);
    writer.write(data.maximum);
    for (const IrradianceProbe& probe : data.probes) {
        writer.write(static_cast<uint32_t>(probe.valid ? 1u : 0u));
        writer.write(probe.irradiance);
        writer.write(probe.depthMoments);
    }
    return {.success = true};
}

IrradianceVolumeResult decodeIrradianceVolume(std::span<const std::byte> input, IrradianceVolumeData& output) {
    ByteReader reader(input);
    std::array<char, 4> header{};
    uint32_t version = 0;
    uint32_t depthResolution = 0;
    IrradianceVolumeData decoded;
    if (!reader.read(header) || header != magic) {
        return {.error = "Not an irradiance volume file (bad signature)"};
    }
    if (!reader.read(version) || version != formatVersion) {
        return {.error = "Unsupported irradiance volume version " + std::to_string(version)};
    }
    if (!reader.read(depthResolution) || depthResolution != irradianceProbeDepthResolution) {
        return {.error = "Unsupported irradiance volume depth resolution " + std::to_string(depthResolution)};
    }
    if (!reader.read(decoded.probeCounts) || !reader.read(decoded.minimum) || !reader.read(decoded.maximum)) {
        return {.error = "Irradiance volume header is truncated"};
    }
    const glm::uvec3 counts = decoded.probeCounts;
    if (glm::any(glm::equal(counts, glm::uvec3(0u))) ||
        glm::any(glm::greaterThan(counts, glm::uvec3(maximumIrradianceProbesPerAxis)))) {
        return {.error = "Irradiance volume probe counts are out of range"};
    }
    decoded.probes.resize(static_cast<size_t>(counts.x) * counts.y * counts.z);
    for (IrradianceProbe& probe : decoded.probes) {
        uint32_t valid = 0;
        if (!reader.read(valid) || !reader.read(probe.irradiance) || !reader.read(probe.depthMoments)) {
            return {.error = "Irradiance volume probe data is truncated"};
        }
        if (valid > 1) {
            return {.error = "Irradiance volume probe has an invalid validity flag"};
        }
        probe.valid = valid == 1;
    }
    if (!reader.finished()) {
        return {.error = "Irradiance volume has trailing data"};
    }
    if (IrradianceVolumeResult result = validateIrradianceVolume(decoded); !result) {
        return result;
    }
    output = std::move(decoded);
    return {.success = true};
}

IrradianceVolumeResult loadIrradianceVolume(const std::filesystem::path& path, IrradianceVolumeData& output) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {.error = "Could not open irradiance volume: " + path.string()};
    }
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad()) {
        return {.error = "Could not read irradiance volume: " + path.string()};
    }
    IrradianceVolumeResult result =
        decodeIrradianceVolume(std::as_bytes(std::span<const char>(bytes.data(), bytes.size())), output);
    if (!result) {
        result.error = path.string() + ": " + result.error;
    }
    return result;
}
