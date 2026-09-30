#include "engine/ui/Localization.hpp"

#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>

LocalizationResult LocalizationCatalog::load(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        return {.error = "could not open localization catalog: " + path.string()};
    }
    try {
        nlohmann::json document;
        input >> document;
        if (document.value("version", 0) != 1 || !document.contains("locales")) {
            return {.error = "unsupported localization catalog: " + path.string()};
        }
        decltype(m_locales) parsed;
        for (const auto& [localeName, entries] : document.at("locales").items()) {
            for (const auto& [id, value] : entries.items()) {
                if (id.empty()) {
                    return {.error = "localization message IDs must not be empty"};
                }
                LocalizedMessage message;
                if (value.is_string()) {
                    message.text = value.get<std::string>();
                } else {
                    message.zero = value.value("zero", "");
                    message.one = value.value("one", "");
                    message.two = value.value("two", "");
                    message.few = value.value("few", "");
                    message.many = value.value("many", "");
                    message.other = value.value("other", "");
                    if (message.other.empty()) {
                        return {.error = "plural message '" + id + "' requires an other form"};
                    }
                }
                parsed[localeName].emplace(id, std::move(message));
            }
        }
        m_locales = std::move(parsed);
        if (!m_locales.contains(m_locale) && !m_locales.empty()) {
            m_locale = m_locales.begin()->first;
        }
        return {.success = true};
    } catch (const nlohmann::json::exception& error) {
        return {.error = "could not parse localization catalog '" + path.string() + "': " + error.what()};
    }
}

LocalizationResult LocalizationCatalog::addLocale(std::string localeName,
                                                  std::unordered_map<MessageId, LocalizedMessage> messages) {
    if (localeName.empty()) {
        return {.error = "locale name must not be empty"};
    }
    for (const auto& [id, message] : messages) {
        if (id.empty() || (message.text.empty() && message.other.empty())) {
            return {.error = "locale contains an invalid message"};
        }
    }
    m_locales.insert_or_assign(std::move(localeName), std::move(messages));
    return {.success = true};
}

bool LocalizationCatalog::selectLocale(std::string_view localeName) {
    const auto exact = m_locales.find(std::string(localeName));
    if (exact != m_locales.end()) {
        m_locale = exact->first;
        return true;
    }
    const size_t separator = localeName.find_first_of("-_");
    if (separator != std::string_view::npos) {
        const auto language = m_locales.find(std::string(localeName.substr(0, separator)));
        if (language != m_locales.end()) {
            m_locale = language->first;
            return true;
        }
    }
    return false;
}

std::string_view LocalizationCatalog::locale() const {
    return m_locale;
}

std::string LocalizationCatalog::text(std::string_view id, int64_t count) const {
    const auto localeIt = m_locales.find(m_locale);
    if (localeIt == m_locales.end()) {
        return "[" + std::string(id) + "]";
    }
    const auto messageIt = localeIt->second.find(std::string(id));
    if (messageIt == localeIt->second.end()) {
        return "[" + std::string(id) + "]";
    }
    const LocalizedMessage& message = messageIt->second;
    std::string result = message.text;
    if (result.empty()) {
        const int64_t absolute = std::abs(count);
        const int64_t modulo10 = absolute % 10;
        const int64_t modulo100 = absolute % 100;
        const bool russian = m_locale.starts_with("ru") || m_locale.starts_with("uk");
        const bool arabic = m_locale.starts_with("ar");
        if (arabic && count == 0 && !message.zero.empty()) {
            result = message.zero;
        } else if (count == 1 && !message.one.empty()) {
            result = message.one;
        } else if (arabic && count == 2 && !message.two.empty()) {
            result = message.two;
        } else if (russian && modulo10 >= 2 && modulo10 <= 4 && !(modulo100 >= 12 && modulo100 <= 14) &&
                   !message.few.empty()) {
            result = message.few;
        } else if (russian && !message.many.empty()) {
            result = message.many;
        } else if (arabic && modulo100 >= 3 && modulo100 <= 10 && !message.few.empty()) {
            result = message.few;
        } else if (arabic && modulo100 >= 11 && modulo100 <= 99 && !message.many.empty()) {
            result = message.many;
        } else {
            result = message.other;
        }
    }
    const std::string marker = "{count}";
    for (size_t position = result.find(marker); position != std::string::npos;
         position = result.find(marker, position)) {
        const std::string replacement = std::to_string(count);
        result.replace(position, marker.size(), replacement);
        position += replacement.size();
    }
    return result;
}

bool LocalizationCatalog::contains(std::string_view id) const {
    const auto localeIt = m_locales.find(m_locale);
    return localeIt != m_locales.end() && localeIt->second.contains(std::string(id));
}

LocalizationResult shapeUtf8(std::string_view text, std::span<const FontHandle> fallbackFonts,
                             std::vector<ShapedGlyph>& glyphs) {
    glyphs.clear();
    if (fallbackFonts.empty()) {
        return {.error = "text shaping requires at least one fallback font"};
    }
    for (size_t offset = 0; offset < text.size();) {
        const uint8_t lead = static_cast<uint8_t>(text[offset]);
        uint32_t length = 1;
        char32_t codepoint = lead;
        if ((lead & 0xE0u) == 0xC0u) {
            length = 2;
            codepoint = lead & 0x1Fu;
        } else if ((lead & 0xF0u) == 0xE0u) {
            length = 3;
            codepoint = lead & 0x0Fu;
        } else if ((lead & 0xF8u) == 0xF0u) {
            length = 4;
            codepoint = lead & 0x07u;
        } else if ((lead & 0x80u) != 0) {
            return {.error = "invalid UTF-8 lead byte"};
        }
        if (offset + length > text.size()) {
            return {.error = "truncated UTF-8 sequence"};
        }
        for (uint32_t index = 1; index < length; ++index) {
            const uint8_t continuation = static_cast<uint8_t>(text[offset + index]);
            if ((continuation & 0xC0u) != 0x80u) {
                return {.error = "invalid UTF-8 continuation byte"};
            }
            codepoint = (codepoint << 6u) | (continuation & 0x3Fu);
        }
        if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
            return {.error = "invalid Unicode codepoint"};
        }
        const size_t fallback = codepoint < 128 ? 0 : std::min<size_t>(1, fallbackFonts.size() - 1);
        glyphs.push_back({codepoint, fallbackFonts[fallback], codepoint == U' ' ? 0.5f : 1.0f});
        offset += length;
    }
    return {.success = true};
}
