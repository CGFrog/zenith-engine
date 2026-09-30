#pragma once

#include "engine/assets/Assets.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using MessageId = std::string;

struct LocalizedMessage {
    std::string text;
    std::string zero;
    std::string one;
    std::string two;
    std::string few;
    std::string many;
    std::string other;
};

struct LocalizationResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

struct ShapedGlyph {
    char32_t codepoint{0};
    FontHandle font{};
    float advance{0.0f};
};

class LocalizationCatalog {
  public:
    LocalizationResult load(const std::filesystem::path& path);
    LocalizationResult addLocale(std::string locale, std::unordered_map<MessageId, LocalizedMessage> messages);
    bool selectLocale(std::string_view locale);
    std::string_view locale() const;
    std::string text(std::string_view id, int64_t count = 1) const;
    bool contains(std::string_view id) const;

  private:
    std::unordered_map<std::string, std::unordered_map<MessageId, LocalizedMessage>> m_locales;
    std::string m_locale{"en"};
};

LocalizationResult shapeUtf8(std::string_view text, std::span<const FontHandle> fallbackFonts,
                             std::vector<ShapedGlyph>& glyphs);
