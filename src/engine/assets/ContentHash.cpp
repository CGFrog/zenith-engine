#include "engine/assets/ContentHash.hpp"

#include <array>
#include <fstream>

ContentHasher& ContentHasher::append(std::span<const uint8_t> bytes) {
    for (const uint8_t value : bytes) {
        m_state ^= value;
        m_state *= prime;
    }
    return *this;
}

ContentHasher& ContentHasher::append(std::string_view text) {
    return append(std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(text.data()), text.size()});
}

bool ContentHasher::appendFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    std::array<uint8_t, 64 * 1024> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        append(std::span<const uint8_t>{buffer.data(), static_cast<size_t>(input.gcount())});
    }
    return !input.bad();
}

std::string ContentHasher::hex() const {
    return formatContentHash(m_state);
}

uint64_t contentHash(std::span<const uint8_t> bytes) {
    return ContentHasher{}.append(bytes).value();
}

uint64_t contentHash(std::string_view text) {
    return ContentHasher{}.append(text).value();
}

std::string formatContentHash(uint64_t hash) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string encoded(16, '0');
    for (size_t index = 0; index < encoded.size(); ++index) {
        encoded[encoded.size() - 1 - index] = digits[(hash >> (index * 4)) & 0xf];
    }
    return encoded;
}
