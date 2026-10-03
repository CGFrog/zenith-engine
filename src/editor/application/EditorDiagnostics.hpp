#pragma once

#include <chrono>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class EditorDiagnosticSeverity : uint8_t {
    Info,
    Warning,
    Error
};

// A place in a source file. Lines and columns are 1-based; 0 means unknown.
struct EditorSourceLocation {
    std::filesystem::path file;
    uint32_t line{0};
    uint32_t column{0};

    auto operator<=>(const EditorSourceLocation&) const = default;
};

struct EditorDiagnostic {
    uint64_t id{0};
    EditorDiagnosticSeverity severity{EditorDiagnosticSeverity::Error};
    std::string source;
    std::string message;
    std::string action;
    // Set when the diagnostic points into a file the user can open.
    std::optional<EditorSourceLocation> location;
    // When the entry was added: wall-clock time for display, steady time for
    // ordering against other logs. Set by EditorDiagnostics.
    std::chrono::system_clock::time_point wallTime;
    std::chrono::steady_clock::time_point steadyTime;
};

class EditorDiagnostics {
  public:
    uint64_t add(EditorDiagnosticSeverity severity, std::string source, std::string message, std::string action = {});
    // Records a failed editor operation and flags it for the status bar until
    // the next successful save, open, or Play transition acknowledges it.
    void reportError(std::string message);
    bool hasUnacknowledgedError() const;
    void acknowledgeErrors();
    // Removes every entry from source and appends the replacements with new
    // ids and that source, so a repeated producer (a build) never accumulates.
    void replaceSource(std::string_view source, std::vector<EditorDiagnostic> replacements);
    void dismiss(uint64_t id);
    void clear();
    const std::vector<EditorDiagnostic>& entries() const;

    // Changes whenever entries() changes, so a reader can skip re-reading.
    uint64_t revision() const {
        return m_revision;
    }

  private:
    uint64_t m_nextId{1};
    uint64_t m_revision{0};
    std::vector<EditorDiagnostic> m_entries;
    bool m_unacknowledgedError{false};
};
