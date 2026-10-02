#include "EditorDiagnostics.hpp"

#include <algorithm>
#include <utility>

uint64_t EditorDiagnostics::add(EditorDiagnosticSeverity severity, std::string source, std::string message,
                                std::string action) {
    const uint64_t id = m_nextId++;
    m_entries.push_back({id, severity, std::move(source), std::move(message), std::move(action), std::nullopt});
    return id;
}

void EditorDiagnostics::reportError(std::string message) {
    add(EditorDiagnosticSeverity::Error, "Editor", std::move(message),
        "Review the operation, correct the path or data, then retry.");
    m_unacknowledgedError = true;
}

bool EditorDiagnostics::hasUnacknowledgedError() const {
    return m_unacknowledgedError;
}

void EditorDiagnostics::acknowledgeErrors() {
    m_unacknowledgedError = false;
}

void EditorDiagnostics::replaceSource(std::string_view source, std::vector<EditorDiagnostic> replacements) {
    std::erase_if(m_entries, [source](const EditorDiagnostic& entry) { return entry.source == source; });
    for (EditorDiagnostic& replacement : replacements) {
        replacement.id = m_nextId++;
        replacement.source = std::string(source);
        m_entries.push_back(std::move(replacement));
    }
}

void EditorDiagnostics::dismiss(uint64_t id) {
    std::erase_if(m_entries, [id](const EditorDiagnostic& entry) { return entry.id == id; });
}

void EditorDiagnostics::clear() {
    m_entries.clear();
}

const std::vector<EditorDiagnostic>& EditorDiagnostics::entries() const {
    return m_entries;
}
