#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

// Incremental 64-bit FNV-1a: a fast, stable, non-cryptographic fingerprint for
// cache keys and change detection. The output for a given byte sequence never
// changes, so persisted keys (for example derived-asset cache names) stay valid
// across builds and platforms. Do not use it where collisions are adversarial.
class ContentHasher {
  public:
    static constexpr uint64_t offsetBasis = 14695981039346656037ull;
    static constexpr uint64_t prime = 1099511628211ull;

    ContentHasher() = default;

    // Feeding the same bytes in one call or several produces the same value.
    ContentHasher& append(std::span<const uint8_t> bytes);
    ContentHasher& append(std::string_view text);

    // Streams a file's bytes into the hash. Returns false when the file cannot
    // be opened or a read fails; the state is then partially updated.
    bool appendFile(const std::filesystem::path& path);

    uint64_t value() const {
        return m_state;
    }

    // Sixteen lowercase hexadecimal digits.
    std::string hex() const;

  private:
    uint64_t m_state{offsetBasis};
};

uint64_t contentHash(std::span<const uint8_t> bytes);
uint64_t contentHash(std::string_view text);

// Formats a 64-bit hash as sixteen zero-padded lowercase hexadecimal digits.
std::string formatContentHash(uint64_t hash);
