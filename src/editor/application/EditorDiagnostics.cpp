#include "EditorDiagnostics.hpp"

#include <algorithm>
#include <utility>

uint64_t EditorDiagnostics::add(EditorDiagnosticSeverity severity, std::string source, std::string message,
                                std::string action) {
    const uint64_t id = m_nextId++;
    m_entries.push_back({id, severity, std::move(source), std::move(message), std::move(action)});
    return id;
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
