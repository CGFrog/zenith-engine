#include "ExternalEditor.hpp"

#include <algorithm>
#include <array>
#include <optional>

namespace {
struct TokenizeResult {
    std::vector<std::string> tokens;
    std::string error;
};

TokenizeResult tokenize(std::string_view commandTemplate, std::string_view commandName) {
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
            return {.error = "The " + std::string(commandName) + " must be a single line"};
        }
        current.push_back(character);
        inToken = true;
    }
    if (quoted) {
        return {.error = "The " + std::string(commandName) + " has an unmatched quote"};
    }
    if (inToken) {
        result.tokens.push_back(std::move(current));
    }
    if (result.tokens.empty()) {
        return {.error = "No " + std::string(commandName) + " is configured"};
    }
    return result;
}

std::string pathArgument(const std::filesystem::path& path) {
    const std::u8string utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

// "{file}, {line}, or {column}".
std::string placeholderList(std::span<const ToolCommandPlaceholder> placeholders) {
    std::string list;
    for (size_t index = 0; index < placeholders.size(); ++index) {
        if (index > 0) {
            list += index + 1 == placeholders.size() ? (placeholders.size() > 2 ? ", or " : " or ") : ", ";
        }
        list += "{" + std::string(placeholders[index].name) + "}";
    }
    return list;
}

// Replaces placeholders in one argument. Returns nothing and sets error when
// a placeholder is unknown or unterminated.
std::optional<std::string> substitute(std::string_view token, std::span<const ToolCommandPlaceholder> placeholders,
                                      std::string_view commandName, std::string& error) {
    std::string expanded;
    for (size_t index = 0; index < token.size(); ++index) {
        const char character = token[index];
        if ((character == '{' || character == '}') && index + 1 < token.size() && token[index + 1] == character) {
            expanded.push_back(character);
            ++index;
            continue;
        }
        if (character == '}') {
            error = "The " + std::string(commandName) + " has an unmatched '}'";
            return std::nullopt;
        }
        if (character != '{') {
            expanded.push_back(character);
            continue;
        }
        const size_t close = token.find('}', index);
        if (close == std::string_view::npos) {
            error = "The " + std::string(commandName) + " has an unterminated placeholder";
            return std::nullopt;
        }
        const std::string_view name = token.substr(index + 1, close - index - 1);
        const auto placeholder = std::ranges::find(placeholders, name, &ToolCommandPlaceholder::name);
        if (placeholder == placeholders.end()) {
            error = "Unknown " + std::string(commandName) + " placeholder {" + std::string(name) + "}; use " +
                    placeholderList(placeholders);
            return std::nullopt;
        }
        expanded += placeholder->value;
        index = close;
    }
    return expanded;
}
} // namespace

ExternalEditorCommandResult expandToolCommand(std::string_view commandTemplate,
                                              std::span<const ToolCommandPlaceholder> placeholders,
                                              std::string_view commandName) {
    TokenizeResult tokens = tokenize(commandTemplate, commandName);
    if (!tokens.error.empty()) {
        return {.error = std::move(tokens.error)};
    }
    ExternalEditorCommandResult result;
    for (const std::string& token : tokens.tokens) {
        std::string error;
        std::optional<std::string> argument = substitute(token, placeholders, commandName, error);
        if (!argument) {
            return {.error = std::move(error)};
        }
        result.arguments.push_back(std::move(*argument));
    }
    if (result.arguments.front().empty()) {
        return {.error = "The " + std::string(commandName) + " does not name a program"};
    }
    return result;
}

ExternalEditorCommandResult expandExternalEditorCommand(std::string_view commandTemplate,
                                                        const EditorSourceLocation& location) {
    const std::array placeholders{
        ToolCommandPlaceholder{.name = "file", .value = pathArgument(location.file)},
        ToolCommandPlaceholder{.name = "line", .value = std::to_string(location.line == 0 ? 1 : location.line)},
        ToolCommandPlaceholder{.name = "column", .value = std::to_string(location.column == 0 ? 1 : location.column)},
    };
    return expandToolCommand(commandTemplate, placeholders, "IDE command");
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

ExternalEditorCommandResult expandDebuggerCommand(std::string_view commandTemplate, uint32_t processId) {
    const std::array placeholders{ToolCommandPlaceholder{.name = "pid", .value = std::to_string(processId)}};
    return expandToolCommand(commandTemplate, placeholders, "debugger command");
}

ChildProcessResult attachDebugger(std::string_view commandTemplate, uint32_t processId) {
    ExternalEditorCommandResult command = expandDebuggerCommand(commandTemplate, processId);
    if (!command) {
        return {.error = std::move(command.error)};
    }
    return ChildProcess::launchDetached({.command = ChildProcessCommand::program(std::move(command.arguments))});
}
