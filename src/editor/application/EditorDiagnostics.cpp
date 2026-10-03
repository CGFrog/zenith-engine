#include "EditorDiagnostics.hpp"

#include <algorithm>
#include <utility>

namespace {
void stamp(EditorDiagnostic& diagnostic) {
    diagnostic.wallTime = std::chrono::system_clock::now();
    diagnostic.steadyTime = std::chrono::steady_clock::now();
}
} // namespace

uint64_t EditorDiagnostics::add(EditorDiagnosticSeverity severity, std::string source, std::string message,
                                std::string action) {
    const uint64_t id = m_nextId++;
    EditorDiagnostic& entry = m_entries.emplace_back();
    entry.id = id;
    entry.severity = severity;
    entry.source = std::move(source);
    entry.message = std::move(message);
    entry.action = std::move(action);
    stamp(entry);
    ++m_revision;
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
        stamp(replacement);
        m_entries.push_back(std::move(replacement));
    }
    ++m_revision;
}

void EditorDiagnostics::dismiss(uint64_t id) {
    if (std::erase_if(m_entries, [id](const EditorDiagnostic& entry) { return entry.id == id; }) > 0) {
        ++m_revision;
    }
}

void EditorDiagnostics::clear() {
    m_entries.clear();
    ++m_revision;
}

const std::vector<EditorDiagnostic>& EditorDiagnostics::entries() const {
    return m_entries;
}
