#pragma once

// {{NAME}} token substitution in template files.

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

// One {{NAME}} placeholder of a template file and its replacement. Names are
// upper-case ASCII letters and underscores.
struct TemplateToken {
    std::string_view name;
    std::string value;
};

// Replaces each {{NAME}} in text with its token's value. An unknown
// {{NAME}} is an error; braces not around an upper-case name (C++
// initializers, JSON) are kept. On failure output is unspecified and error
// names the token.
bool substituteTemplateTokens(std::string_view text, std::span<const TemplateToken> tokens, std::string& output,
                              std::string& error);

// Reads a whole file as bytes; false when it cannot be read.
bool readTextFile(const std::filesystem::path& path, std::string& contents);
// Writes contents as bytes, creating the parent directories; on failure
// error says why.
bool writeTextFile(const std::filesystem::path& path, std::string_view contents, std::string& error);
