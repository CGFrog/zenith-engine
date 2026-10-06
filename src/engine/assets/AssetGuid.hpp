#pragma once

// A 128-bit identifier that names an asset independently of its file path, so references survive renames and moves.

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <string_view>

// A 128-bit identifier that names an asset independently of its file path, so
// references survive renames and moves. The all-zero value is the null GUID;
// generators never produce it. The text form is 32 lowercase hexadecimal
// digits, high word first.
class AssetGuid {
  public:
    constexpr AssetGuid() = default;

    constexpr AssetGuid(uint64_t high, uint64_t low)
        : m_high(high)
        , m_low(low) {}

    // Accepts exactly 32 hexadecimal digits in either case.
    static std::optional<AssetGuid> parse(std::string_view text);

    std::string toString() const;

    constexpr bool isNull() const {
        return m_high == 0 && m_low == 0;
    }

    constexpr explicit operator bool() const {
        return !isNull();
    }

    constexpr uint64_t high() const {
        return m_high;
    }

    constexpr uint64_t low() const {
        return m_low;
    }

    auto operator<=>(const AssetGuid&) const = default;

  private:
    uint64_t m_high{0};
    uint64_t m_low{0};
};

namespace std {
template <> struct hash<AssetGuid> {
    size_t operator()(const AssetGuid& guid) const noexcept {
        // The words are already uniformly random; fold them together.
        return static_cast<size_t>(guid.high() ^ (guid.low() * 0x9e3779b97f4a7c15ull));
    }
};
} // namespace std

// Produces random non-null GUIDs. Each owner keeps its own generator; there is
// no process-wide state.
class AssetGuidGenerator {
  public:
    // Seeds from std::random_device for globally unique identifiers.
    AssetGuidGenerator();

    // A deterministic sequence for tests and reproducible tooling. GUIDs from a
    // fixed seed are not unique across projects; do not use it for real content.
    explicit AssetGuidGenerator(uint64_t seed);

    AssetGuid next();

  private:
    std::mt19937_64 m_engine;
};
