#include "ColorLut.hpp"

#include <charconv>
#include <cmath>
#include <fstream>
#include <iterator>
#include <optional>

namespace {
bool isSpace(char character) {
    return character == ' ' || character == '\t' || character == '\r' || character == '\f' || character == '\v';
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && isSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isSpace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

// Splits on whitespace; a line holds at most a keyword and three values, so
// anything beyond the expected count is reported as an extra token.
std::vector<std::string_view> tokens(std::string_view line) {
    std::vector<std::string_view> result;
    size_t position = 0;
    while (position < line.size()) {
        while (position < line.size() && isSpace(line[position])) {
            ++position;
        }
        const size_t begin = position;
        while (position < line.size() && !isSpace(line[position])) {
            ++position;
        }
        if (position > begin) {
            result.push_back(line.substr(begin, position - begin));
        }
    }
    return result;
}

std::optional<float> parseFloat(std::string_view token) {
    // from_chars rejects a leading '+', which some exporters write.
    if (!token.empty() && token.front() == '+') {
        token.remove_prefix(1);
    }
    float value = 0.0f;
    const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
    if (error != std::errc{} || end != token.data() + token.size() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<glm::vec3> parseTriple(const std::vector<std::string_view>& parts, size_t first) {
    if (parts.size() != first + 3) {
        return std::nullopt;
    }
    glm::vec3 value{0.0f};
    for (int component = 0; component < 3; ++component) {
        const std::optional<float> parsed = parseFloat(parts[first + component]);
        if (!parsed) {
            return std::nullopt;
        }
        value[component] = *parsed;
    }
    return value;
}

ColorLutResult lineError(size_t line, const std::string& message) {
    return {.error = "Line " + std::to_string(line) + ": " + message};
}

std::string parseTitle(std::string_view line) {
    std::string_view title = trim(line.substr(std::string_view("TITLE").size()));
    if (title.size() >= 2 && title.front() == '"' && title.back() == '"') {
        title = title.substr(1, title.size() - 2);
    }
    return std::string(title);
}
} // namespace

ColorLutResult parseColorLut(std::string_view text, ColorLut& output) {
    ColorLut parsed;
    size_t lineNumber = 0;
    bool dataStarted = false;
    while (!text.empty()) {
        const size_t newline = text.find('\n');
        const std::string_view rawLine = text.substr(0, newline);
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
        ++lineNumber;
        const std::string_view line = trim(rawLine);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::vector<std::string_view> parts = tokens(line);
        const std::string_view keyword = parts.front();
        const bool isData = keyword.front() == '-' || keyword.front() == '+' || keyword.front() == '.' ||
                            (keyword.front() >= '0' && keyword.front() <= '9');
        if (isData) {
            if (parsed.size == 0) {
                return lineError(lineNumber, "data appears before LUT_3D_SIZE");
            }
            const std::optional<glm::vec3> value = parseTriple(parts, 0);
            if (!value) {
                return lineError(lineNumber, "expected three finite numbers");
            }
            if (parsed.texels.size() == static_cast<size_t>(parsed.size) * parsed.size * parsed.size) {
                return lineError(lineNumber, "more data lines than LUT_3D_SIZE^3");
            }
            parsed.texels.push_back(*value);
            dataStarted = true;
            continue;
        }
        if (dataStarted) {
            return lineError(lineNumber, "keyword '" + std::string(keyword) + "' after the data lines");
        }
        if (keyword == "TITLE") {
            parsed.title = parseTitle(line);
        } else if (keyword == "LUT_3D_SIZE") {
            const std::optional<float> size = parts.size() == 2 ? parseFloat(parts[1]) : std::nullopt;
            if (!size || *size != std::floor(*size) || *size < static_cast<float>(minimumColorLutSize) ||
                *size > static_cast<float>(maximumColorLutSize)) {
                return lineError(lineNumber, "LUT_3D_SIZE must be an integer from " +
                                                 std::to_string(minimumColorLutSize) + " to " +
                                                 std::to_string(maximumColorLutSize));
            }
            parsed.size = static_cast<uint32_t>(*size);
            parsed.texels.reserve(static_cast<size_t>(parsed.size) * parsed.size * parsed.size);
        } else if (keyword == "LUT_1D_SIZE") {
            return lineError(lineNumber, "1D LUTs are not supported; expected a 3D LUT");
        } else if (keyword == "DOMAIN_MIN" || keyword == "DOMAIN_MAX") {
            const std::optional<glm::vec3> value = parseTriple(parts, 1);
            if (!value) {
                return lineError(lineNumber, std::string(keyword) + " expects three finite numbers");
            }
            (keyword == "DOMAIN_MIN" ? parsed.domainMinimum : parsed.domainMaximum) = *value;
        } else if (keyword == "LUT_3D_INPUT_RANGE") {
            const std::optional<float> minimum = parts.size() == 3 ? parseFloat(parts[1]) : std::nullopt;
            const std::optional<float> maximum = parts.size() == 3 ? parseFloat(parts[2]) : std::nullopt;
            if (!minimum || !maximum) {
                return lineError(lineNumber, "LUT_3D_INPUT_RANGE expects two finite numbers");
            }
            parsed.domainMinimum = glm::vec3(*minimum);
            parsed.domainMaximum = glm::vec3(*maximum);
        } else {
            return lineError(lineNumber, "unknown keyword '" + std::string(keyword) + "'");
        }
    }
    if (parsed.size == 0) {
        return {.error = "Missing LUT_3D_SIZE"};
    }
    const size_t expected = static_cast<size_t>(parsed.size) * parsed.size * parsed.size;
    if (parsed.texels.size() != expected) {
        return {.error = "Expected " + std::to_string(expected) + " data lines for LUT_3D_SIZE " +
                         std::to_string(parsed.size) + " but found " + std::to_string(parsed.texels.size())};
    }
    if (glm::any(glm::greaterThanEqual(parsed.domainMinimum, parsed.domainMaximum))) {
        return {.error = "DOMAIN_MIN must be below DOMAIN_MAX in every channel"};
    }
    output = std::move(parsed);
    return {.success = true};
}

ColorLutResult loadColorLut(const std::filesystem::path& path, ColorLut& output) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {.error = "Could not open colour LUT: " + path.string()};
    }
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad()) {
        return {.error = "Could not read colour LUT: " + path.string()};
    }
    ColorLutResult result = parseColorLut(text, output);
    if (!result) {
        result.error = path.string() + ": " + result.error;
    }
    return result;
}

ColorLut identityColorLut(uint32_t size) {
    ColorLut lut{.title = "Identity", .size = size};
    lut.texels.reserve(static_cast<size_t>(size) * size * size);
    const float scale = 1.0f / static_cast<float>(size - 1);
    for (uint32_t blue = 0; blue < size; ++blue) {
        for (uint32_t green = 0; green < size; ++green) {
            for (uint32_t red = 0; red < size; ++red) {
                lut.texels.emplace_back(red * scale, green * scale, blue * scale);
            }
        }
    }
    return lut;
}
