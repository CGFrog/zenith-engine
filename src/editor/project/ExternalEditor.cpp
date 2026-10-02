#include "ExternalEditor.hpp"

#include <optional>

namespace {
struct TokenizeResult {
    std::vector<std::string> tokens;
    std::string error;
};

TokenizeResult tokenize(std::string_view commandTemplate) {
    TokenizeResult result;
    std::string current;
    bool inToken = false;
    bool quoted = false;
    for (const char character : commandTemplate) {
        if (character == '"') {
            quoted = !quoted;
            inToken = true;
            continue;
        }
        if (!quoted && (character == ' ' || character == '\t')) {
            if (inToken) {
                result.tokens.push_back(std::move(current));
                current.clear();
                inToken = false;
            }
            continue;
        }
        if (character == '\r' || character == '\n') {
            return {.error = "The IDE command must be a single line"};
        }
        current.push_back(character);
        inToken = true;
    }
    if (quoted) {
        return {.error = "The IDE command has an unmatched quote"};
    }
    if (inToken) {
        result.tokens.push_back(std::move(current));
    }
    if (result.tokens.empty()) {
        return {.error = "No IDE command is configured"};
    }
    return result;
}

std::string pathArgument(const std::filesystem::path& path) {
    const std::u8string utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

// Replaces placeholders in one argument. Returns nothing and sets error when
// a placeholder is unknown or unterminated.
std::optional<std::string> substitute(std::string_view token, const EditorSourceLocation& location,
                                      std::string& error) {
    std::string expanded;
    for (size_t index = 0; index < token.size(); ++index) {
        const char character = token[index];
        if ((character == '{' || character == '}') && index + 1 < token.size() && token[index + 1] == character) {
            expanded.push_back(character);
            ++index;
            continue;
        }
        if (character == '}') {
            error = "The IDE command has an unmatched '}'";
            return std::nullopt;
        }
        if (character != '{') {
            expanded.push_back(character);
            continue;
        }
        const size_t close = token.find('}', index);
        if (close == std::string_view::npos) {
            error = "The IDE command has an unterminated placeholder";
            return std::nullopt;
        }
        const std::string_view name = token.substr(index + 1, close - index - 1);
        if (name == "file") {
            expanded += pathArgument(location.file);
        } else if (name == "line") {
            expanded += std::to_string(location.line == 0 ? 1 : location.line);
        } else if (name == "column") {
            expanded += std::to_string(location.column == 0 ? 1 : location.column);
        } else {
            error = "Unknown IDE command placeholder {" + std::string(name) + "}; use {file}, {line}, or {column}";
            return std::nullopt;
        }
        index = close;
    }
    return expanded;
}
} // namespace

ExternalEditorCommandResult expandExternalEditorCommand(std::string_view commandTemplate,
                                                        const EditorSourceLocation& location) {
    TokenizeResult tokens = tokenize(commandTemplate);
    if (!tokens.error.empty()) {
        return {.error = std::move(tokens.error)};
    }
    ExternalEditorCommandResult result;
    for (const std::string& token : tokens.tokens) {
        std::string error;
        std::optional<std::string> argument = substitute(token, location, error);
        if (!argument) {
            return {.error = std::move(error)};
        }
        result.arguments.push_back(std::move(*argument));
    }
    if (result.arguments.front().empty()) {
        return {.error = "The IDE command does not name a program"};
    }
    return result;
}

ChildProcessResult openInExternalEditor(std::string_view commandTemplate, const EditorSourceLocation& location) {
    if (location.file.empty()) {
        return {.error = "The diagnostic has no file to open"};
    }
    ExternalEditorCommandResult command = expandExternalEditorCommand(commandTemplate, location);
    if (!command) {
        return {.error = std::move(command.error)};
    }
    return ChildProcess::launchDetached({.command = ChildProcessCommand::program(std::move(command.arguments))});
}

void openSourceLocation(std::string_view ideCommand, const EditorSourceLocation& location,
                        EditorDiagnostics& diagnostics) {
    if (const ChildProcessResult opened = openInExternalEditor(ideCommand, location); !opened) {
        diagnostics.add(EditorDiagnosticSeverity::Error, "IDE",
                        "Could not open " + location.file.string() + ": " + opened.error,
                        "Set ide.command in the editor settings.ini, for example: code -g \"{file}:{line}:{column}\"");
    }
}
