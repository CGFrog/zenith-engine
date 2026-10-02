#pragma once

#include "editor/application/EditorDiagnostics.hpp"

#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

enum class CompilerDiagnosticSeverity : uint8_t {
    Note,
    Warning,
    Error
};

// One error, warning, or note reported by a compiler, linker, or build tool.
struct CompilerDiagnostic {
    CompilerDiagnosticSeverity severity{CompilerDiagnosticSeverity::Error};
    // Absent for tool-level messages such as linker errors. Paths are as
    // printed and may be relative to the build's working directory.
    std::optional<EditorSourceLocation> location;
    // Tool-specific code such as C2065 or LNK1104; empty when none is printed.
    std::string code;
    std::string message;

    bool operator==(const CompilerDiagnostic&) const = default;
};

// Recognises one line of build output. Supported forms:
//   MSVC/MSBuild  `C:\src\a.cpp(12,5): error C2065: msg [proj.vcxproj]`
//                 `a.cpp(12): warning C4100: msg`, optionally prefixed `3>`
//   GCC/Clang     `/src/a.cpp:12:5: error: msg`, `a.cpp:12: note: msg`
//   CMake         `CMake Error at CMakeLists.txt:12 (add_executable):`
//   Tool-level    `LINK : fatal error LNK1104: msg`, `collect2: error: msg`,
//                 and any line reporting an `undefined reference to`.
// Anything else returns nothing.
std::optional<CompilerDiagnostic> parseCompilerOutputLine(std::string_view line);

// Accumulates the diagnostics of one build's output, keeping first-seen order
// and dropping exact duplicates (MSBuild repeats every error in its summary).
class CompilerOutputParser {
  public:
    // Parses one line and returns its diagnostic, including a duplicate that
    // was not recorded again.
    std::optional<CompilerDiagnostic> addLine(std::string_view line);
    const std::vector<CompilerDiagnostic>& diagnostics() const;
    void clear();

  private:
    using Key =
        std::tuple<CompilerDiagnosticSeverity, std::filesystem::path, uint32_t, uint32_t, std::string, std::string>;

    std::vector<CompilerDiagnostic> m_diagnostics;
    std::set<Key> m_seen;
};

std::vector<CompilerDiagnostic> parseCompilerOutput(std::span<const std::string> lines);
