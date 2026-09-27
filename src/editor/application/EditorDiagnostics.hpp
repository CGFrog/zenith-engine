#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class EditorDiagnosticSeverity : uint8_t {
    Info,
    Warning,
    Error
};

struct EditorDiagnostic {
    uint64_t id{0};
    EditorDiagnosticSeverity severity{EditorDiagnosticSeverity::Error};
    std::string source;
    std::string message;
    std::string action;
};

class EditorDiagnostics {
  public:
    uint64_t add(EditorDiagnosticSeverity severity, std::string source, std::string message, std::string action = {});
    void dismiss(uint64_t id);
    void clear();
    const std::vector<EditorDiagnostic>& entries() const;

  private:
    uint64_t m_nextId{1};
    std::vector<EditorDiagnostic> m_entries;
};
