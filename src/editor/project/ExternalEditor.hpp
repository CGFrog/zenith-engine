#pragma once

#include "editor/application/EditorDiagnostics.hpp"
#include "engine/platform/ChildProcess.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct ExternalEditorCommandResult {
    std::vector<std::string> arguments;
    std::string error;

    explicit operator bool() const {
        return error.empty();
    }
};

// One {name} a tool command template may use, and its value.
struct ToolCommandPlaceholder {
    std::string_view name;
    std::string value;
};

// Turns a tool command template into program arguments, as described for
// expandExternalEditorCommand, with the given placeholders. commandName
// ("IDE command") names the setting in errors.
ExternalEditorCommandResult expandToolCommand(std::string_view commandTemplate,
                                              std::span<const ToolCommandPlaceholder> placeholders,
                                              std::string_view commandName);

// Turns an IDE command template into program arguments for one location.
//
// The template is split into arguments at unquoted whitespace; double quotes
// group text containing spaces and are removed. Placeholders are then
// substituted inside each argument, so a path with spaces or shell
// metacharacters always stays one argument and is never interpreted by a
// shell. Placeholders: {file} (absolute path when the location's file is
// absolute), {line} and {column} (1 when unknown), and {{ / }} for literal
// braces. Any other {name} is an error, as is an empty or unbalanced template.
//
// Examples: `code -g "{file}:{line}:{column}"`, `clion --line {line} {file}`,
//           `devenv /edit "{file}"`.
ExternalEditorCommandResult expandExternalEditorCommand(std::string_view commandTemplate,
                                                        const EditorSourceLocation& location);

// Expands the template and launches the IDE detached from the editor.
ChildProcessResult openInExternalEditor(std::string_view commandTemplate, const EditorSourceLocation& location);

// Opens a location with the IDE command, reporting a failure as an editor
// diagnostic (source "IDE") instead of returning it.
void openSourceLocation(std::string_view ideCommand, const EditorSourceLocation& location,
                        EditorDiagnostics& diagnostics);

// The debugger command (ide.debug_command) for a process: its only
// placeholder is {pid}, for example `vsjitdebugger.exe -p {pid}`.
ExternalEditorCommandResult expandDebuggerCommand(std::string_view commandTemplate, uint32_t processId);
// Expands the debugger command and launches it detached, to attach to
// processId.
ChildProcessResult attachDebugger(std::string_view commandTemplate, uint32_t processId);
