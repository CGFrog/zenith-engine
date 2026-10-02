#include "engine/assets/AssetGuid.hpp"

#include <array>

namespace {
std::optional<uint64_t> parseHexWord(std::string_view digits) {
    uint64_t value = 0;
    for (const char digit : digits) {
        uint64_t nibble = 0;
        if (digit >= '0' && digit <= '9') {
            nibble = static_cast<uint64_t>(digit - '0');
        } else if (digit >= 'a' && digit <= 'f') {
            nibble = static_cast<uint64_t>(digit - 'a' + 10);
        } else if (digit >= 'A' && digit <= 'F') {
            nibble = static_cast<uint64_t>(digit - 'A' + 10);
        } else {
            return std::nullopt;
        }
        value = (value << 4) | nibble;
    }
    return value;
}

void appendHexWord(std::string& output, uint64_t value) {
    static constexpr char digits[] = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4) {
        output.push_back(digits[(value >> shift) & 0xf]);
    }
}

std::mt19937_64 randomlySeededEngine() {
    std::random_device device;
    std::array<std::random_device::result_type, 8> entropy{};
    for (auto& word : entropy) {
        word = device();
    }
    std::seed_seq sequence(entropy.begin(), entropy.end());
    return std::mt19937_64(sequence);
}
} // namespace

std::optional<AssetGuid> AssetGuid::parse(std::string_view text) {
    if (text.size() != 32) {
        return std::nullopt;
    }
    const std::optional<uint64_t> high = parseHexWord(text.substr(0, 16));
    const std::optional<uint64_t> low = parseHexWord(text.substr(16));
    if (!high || !low) {
        return std::nullopt;
    }
    return AssetGuid{*high, *low};
}

std::string AssetGuid::toString() const {
    std::string output;
    output.reserve(32);
    appendHexWord(output, m_high);
    appendHexWord(output, m_low);
    return output;
}

AssetGuidGenerator::AssetGuidGenerator()
    : m_engine(randomlySeededEngine()) {}

AssetGuidGenerator::AssetGuidGenerator(uint64_t seed)
    : m_engine(seed) {}

AssetGuid AssetGuidGenerator::next() {
    while (true) {
        const AssetGuid guid{m_engine(), m_engine()};
        if (!guid.isNull()) {
            return guid;
        }
    }
}
