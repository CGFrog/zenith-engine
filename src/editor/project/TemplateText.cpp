#include "TemplateText.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>

namespace {
bool isTokenCharacter(char character) {
    return (character >= 'A' && character <= 'Z') || character == '_';
}
} // namespace

bool substituteTemplateTokens(std::string_view text, std::span<const TemplateToken> tokens, std::string& output,
                              std::string& error) {
    output.clear();
    output.reserve(text.size());
    size_t position = 0;
    while (position < text.size()) {
        const size_t open = text.find("{{", position);
        if (open == std::string_view::npos) {
            output.append(text.substr(position));
            break;
        }
        size_t nameEnd = open + 2;
        while (nameEnd < text.size() && isTokenCharacter(text[nameEnd])) {
            ++nameEnd;
        }
        if (nameEnd == open + 2 || text.substr(nameEnd, 2) != "}}") {
            output.append(text.substr(position, open + 2 - position));
            position = open + 2;
            continue;
        }
        const std::string_view name = text.substr(open + 2, nameEnd - open - 2);
        const auto token = std::ranges::find(tokens, name, &TemplateToken::name);
        if (token == tokens.end()) {
            error = "Unknown template token {{" + std::string(name) + "}}";
            return false;
        }
        output.append(text.substr(position, open - position));
        output.append(token->value);
        position = nameEnd + 2;
    }
    return true;
}

bool readTextFile(const std::filesystem::path& path, std::string& contents) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    contents.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return !input.bad();
}

bool writeTextFile(const std::filesystem::path& path, std::string_view contents, std::string& error) {
    std::error_code directoryError;
    std::filesystem::create_directories(path.parent_path(), directoryError);
    if (directoryError) {
        error = "Could not create " + path.parent_path().string() + ": " + directoryError.message();
        return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output) {
        error = "Could not write " + path.string();
        return false;
    }
    return true;
}
