#include "InfoPanel.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/operators/OperatorHistory.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"
#include "engine/diagnostics/Logger.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <ctime>

namespace {
constexpr std::string_view buildDiagnosticSource = "Build";
constexpr std::string_view gameModuleDiagnosticSource = "Game Module";

InfoSeverity infoSeverity(LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
        return InfoSeverity::Debug;
    case LogLevel::Info:
        return InfoSeverity::Info;
    case LogLevel::Warning:
        return InfoSeverity::Warning;
    case LogLevel::Error:
        return InfoSeverity::Error;
    }
    return InfoSeverity::Info;
}

InfoSeverity infoSeverity(EditorDiagnosticSeverity severity) {
    switch (severity) {
    case EditorDiagnosticSeverity::Info:
        return InfoSeverity::Info;
    case EditorDiagnosticSeverity::Warning:
        return InfoSeverity::Warning;
    case EditorDiagnosticSeverity::Error:
        return InfoSeverity::Error;
    }
    return InfoSeverity::Info;
}

InfoSource diagnosticSource(std::string_view source) {
    if (source == buildDiagnosticSource) {
        return InfoSource::Build;
    }
    if (source == gameModuleDiagnosticSource) {
        return InfoSource::GameModule;
    }
    return InfoSource::Editor;
}

std::string_view operatorStatusName(OperatorStatus status) {
    switch (status) {
    case OperatorStatus::Finished:
        return "Finished";
    case OperatorStatus::Cancelled:
        return "Cancelled";
    case OperatorStatus::Failed:
        return "Failed";
    }
    return "Unknown";
}

InfoRecord engineRecord(const LogEntry& entry) {
    return {.source = InfoSource::Engine,
            .severity = infoSeverity(entry.level),
            .category = entry.category,
            .text = entry.message,
            .originId = entry.sequence,
            .wallTime = entry.wallTime,
            .steadyTime = entry.steadyTime};
}

InfoRecord operatorRecord(const OperatorHistoryEntry& entry) {
    std::string text = entry.label.empty() ? entry.id : entry.label;
    text += ": ";
    text += operatorStatusName(entry.status);
    if (!entry.message.empty()) {
        text += " - " + entry.message;
    }
    if (!entry.propertiesJson.empty() && entry.propertiesJson != "{}") {
        text += " " + entry.propertiesJson;
    }
    return {.source = InfoSource::Operator,
            .severity = entry.status == OperatorStatus::Failed ? InfoSeverity::Error : InfoSeverity::Info,
            .category = entry.id,
            .text = std::move(text),
            .originId = entry.sequence,
            .wallTime = entry.wallTime,
            .steadyTime = entry.steadyTime};
}

InfoRecord diagnosticRecord(const EditorDiagnostic& diagnostic) {
    std::string text = diagnostic.message;
    if (diagnostic.location) {
        text += " (" + diagnostic.location->file.generic_string();
        if (diagnostic.location->line != 0) {
            text += ":" + std::to_string(diagnostic.location->line);
        }
        text += ")";
    }
    return {.source = diagnosticSource(diagnostic.source),
            .severity = infoSeverity(diagnostic.severity),
            .category = diagnostic.source,
            .text = std::move(text),
            .originId = diagnostic.id,
            .wallTime = diagnostic.wallTime,
            .steadyTime = diagnostic.steadyTime};
}

std::string lowercase(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return result;
}

// Local wall-clock time with milliseconds, "HH:MM:SS.mmm".
std::string clockText(std::chrono::system_clock::time_point time) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count() % 1000;
    std::array<char, 16> text{};
    std::snprintf(text.data(), text.size(), "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min, local.tm_sec,
                  static_cast<int>(milliseconds < 0 ? milliseconds + 1000 : milliseconds));
    return text.data();
}

std::string sourceLabel(const InfoRecord& record) {
    std::string label(infoSourceName(record.source));
    // Diagnostic sources other than Build and Game Module name the producer.
    if (!record.category.empty() && record.source != InfoSource::Build && record.source != InfoSource::GameModule) {
        label += "/" + record.category;
    }
    return label;
}

EditorIcon severityIcon(InfoSeverity severity) {
    switch (severity) {
    case InfoSeverity::Debug:
    case InfoSeverity::Info:
        return EditorIcon::Info;
    case InfoSeverity::Warning:
        return EditorIcon::Warning;
    case InfoSeverity::Error:
        return EditorIcon::Error;
    }
    return EditorIcon::Info;
}

ImVec4 severityColor(InfoSeverity severity) {
    switch (severity) {
    case InfoSeverity::Debug:
        return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    case InfoSeverity::Info:
        return toImVec4(palette::info);
    case InfoSeverity::Warning:
        return toImVec4(palette::warning);
    case InfoSeverity::Error:
        return toImVec4(palette::error);
    }
    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}
} // namespace

std::string_view infoSeverityName(InfoSeverity severity) {
    switch (severity) {
    case InfoSeverity::Debug:
        return "Debug";
    case InfoSeverity::Info:
        return "Info";
    case InfoSeverity::Warning:
        return "Warning";
    case InfoSeverity::Error:
        return "Error";
    }
    return "Unknown";
}

std::string_view infoSourceName(InfoSource source) {
    switch (source) {
    case InfoSource::Engine:
        return "Engine";
    case InfoSource::Editor:
        return "Editor";
    case InfoSource::Build:
        return "Build";
    case InfoSource::GameModule:
        return "Game Module";
    case InfoSource::Operator:
        return "Operator";
    }
    return "Unknown";
}

std::string formatInfoRecord(const InfoRecord& record) {
    return clockText(record.wallTime) + " [" + std::string(infoSeverityName(record.severity)) + "] " +
           sourceLabel(record) + ": " + record.text;
}

bool InfoFilter::accepts(const InfoRecord& record) const {
    if (!severities[static_cast<size_t>(record.severity)] || !sources[static_cast<size_t>(record.source)]) {
        return false;
    }
    if (text.empty()) {
        return true;
    }
    const std::string needle = lowercase(text);
    return lowercase(record.text).find(needle) != std::string::npos ||
           lowercase(record.category).find(needle) != std::string::npos;
}

InfoLog::InfoLog(size_t streamCapacity)
    : m_streamCapacity(std::max<size_t>(streamCapacity, 1)) {}

bool InfoLog::sync(const Logger* engineLog, const EditorDiagnostics* diagnostics, const OperatorHistory* operators) {
    bool changed = false;
    if (engineLog != nullptr && engineLog->latestSequence() != m_engineSequence) {
        // One snapshot, so entries logged by other threads meanwhile are
        // picked up next time; entries dropped from the ring are skipped.
        const LogSnapshot snapshot = engineLog->snapshotAfter(m_engineSequence);
        for (const LogEntry& entry : snapshot.entries) {
            appendStreamed(engineRecord(entry));
        }
        m_engineSequence = snapshot.latestSequence;
        changed = !snapshot.entries.empty();
    }
    if (operators != nullptr && operators->latestSequence() != m_operatorSequence) {
        for (const OperatorHistoryEntry& entry : operators->entries()) {
            if (entry.sequence > m_operatorSequence) {
                appendStreamed(operatorRecord(entry));
            }
        }
        m_operatorSequence = operators->latestSequence();
        changed = true;
    }
    if (diagnostics != nullptr && (!m_diagnosticsSeen || diagnostics->revision() != m_diagnosticsRevision)) {
        m_diagnosticRecords.clear();
        for (const EditorDiagnostic& diagnostic : diagnostics->entries()) {
            m_diagnosticRecords.push_back(diagnosticRecord(diagnostic));
        }
        m_diagnosticsSeen = true;
        m_diagnosticsRevision = diagnostics->revision();
        changed = true;
    }
    if (changed) {
        rebuild();
    }
    return changed;
}

size_t InfoLog::count(InfoSource source) const {
    return static_cast<size_t>(std::count_if(m_records.begin(), m_records.end(),
                                             [source](const InfoRecord& record) { return record.source == source; }));
}

void InfoLog::clearStreams() {
    m_streamed.clear();
    rebuild();
}

void InfoLog::appendStreamed(InfoRecord record) {
    if (m_streamed.size() == m_streamCapacity) {
        m_streamed.pop_front();
    }
    m_streamed.push_back(std::move(record));
}

void InfoLog::rebuild() {
    m_records.assign(m_streamed.begin(), m_streamed.end());
    m_records.insert(m_records.end(), m_diagnosticRecords.begin(), m_diagnosticRecords.end());
    // Stable, so records from one source keep their arrival order on ties.
    std::stable_sort(m_records.begin(), m_records.end(), [](const InfoRecord& first, const InfoRecord& second) {
        return first.steadyTime < second.steadyTime;
    });
    ++m_revision;
}

InfoPanel::InfoPanel(const OperatorHistory& operators)
    : m_operators(operators) {}

const char* InfoPanel::title() const {
    return "Info";
}

void InfoPanel::update(EditorContext& context, const InputState&, float) {
    // Kept current while closed so reopening shows everything since.
    sync(&context.app.logger, context.diagnostics);
}

void InfoPanel::draw(EditorContext& context) {
    drawContents(context.diagnostics);
}

void InfoPanel::sync(const Logger* engineLog, const EditorDiagnostics& diagnostics) {
    m_log.sync(engineLog, &diagnostics, &m_operators);
}

const std::vector<size_t>& InfoPanel::visibleRecords() {
    if (!m_visibleValid || m_visibleRevision != m_log.revision() || !(m_visibleFilter == m_filter)) {
        m_visible.clear();
        const std::vector<InfoRecord>& records = m_log.records();
        for (size_t index = 0; index < records.size(); ++index) {
            if (m_filter.accepts(records[index])) {
                m_visible.push_back(index);
            }
        }
        m_visibleRevision = m_log.revision();
        m_visibleFilter = m_filter;
        m_visibleValid = true;
    }
    return m_visible;
}

std::string InfoPanel::copyText() {
    const std::vector<InfoRecord>& records = m_log.records();
    const std::vector<size_t>& visible = visibleRecords();
    const bool selectionOnly = std::any_of(visible.begin(), visible.end(), [this, &records](size_t index) {
        return m_selected.contains({records[index].source, records[index].originId});
    });
    std::string text;
    for (const size_t index : visible) {
        const InfoRecord& record = records[index];
        if (selectionOnly && !m_selected.contains({record.source, record.originId})) {
            continue;
        }
        text += formatInfoRecord(record);
        text += '\n';
    }
    return text;
}

void InfoPanel::clear(EditorDiagnostics& diagnostics) {
    m_log.clearStreams();
    diagnostics.clear();
    m_log.sync(nullptr, &diagnostics, nullptr);
    m_selected.clear();
}

void InfoPanel::drawContents(EditorDiagnostics& diagnostics) {
    drawToolbar(diagnostics);
    ImGui::Separator();
    drawRecords();
}

void InfoPanel::drawToolbar(EditorDiagnostics& diagnostics) {
    for (size_t index = 0; index < infoSeverityCount; ++index) {
        const std::string label(infoSeverityName(static_cast<InfoSeverity>(index)));
        ImGui::Checkbox(label.c_str(), &m_filter.severities[index]);
        ImGui::SameLine();
    }
    ImGui::TextDisabled("|");
    for (size_t index = 0; index < infoSourceCount; ++index) {
        const InfoSource source = static_cast<InfoSource>(index);
        const std::string label = std::string(infoSourceName(source)) + " (" + std::to_string(m_log.count(source)) +
                                  ")##source" + std::to_string(index);
        ImGui::SameLine();
        ImGui::Checkbox(label.c_str(), &m_filter.sources[index]);
    }

    ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x * 0.4f, 120.0f));
    if (ImGui::InputTextWithHint("##search", "Search text or category", m_searchBuffer.data(), m_searchBuffer.size())) {
        m_filter.text = m_searchBuffer.data();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &m_autoScroll);
    ImGui::SameLine();
    if (widgets::Button("Copy")) {
        ImGui::SetClipboardText(copyText().c_str());
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Copies the selected rows, or every shown row when none is selected");
    }
    ImGui::SameLine();
    if (widgets::Button("Clear")) {
        clear(diagnostics);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Hides the engine log and operator history so far and clears editor diagnostics");
    }
}

void InfoPanel::selectRecord(const InfoRecord& record, bool toggle) {
    const RecordKey key{record.source, record.originId};
    if (!toggle) {
        m_selected.clear();
        m_selected.insert(key);
        return;
    }
    if (!m_selected.erase(key)) {
        m_selected.insert(key);
    }
}

void InfoPanel::drawRecords() {
    constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                           ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                                           ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##info records", 4, tableFlags)) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time");
    ImGui::TableSetupColumn("Severity");
    ImGui::TableSetupColumn("Source");
    ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    const std::vector<InfoRecord>& records = m_log.records();
    const std::vector<size_t>& visible = visibleRecords();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(visible.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const InfoRecord& record = records[visible[static_cast<size_t>(row)]];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(row);
            const bool selected = m_selected.contains({record.source, record.originId});
            const std::string time = clockText(record.wallTime);
            if (ImGui::Selectable(time.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
                selectRecord(record, ImGui::GetIO().KeyCtrl);
            }
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, severityColor(record.severity));
            ImGui::Text("%s%s", widgets::IconLabelPadding(), infoSeverityName(record.severity).data());
            ImGui::PopStyleColor();
            widgets::ItemIcon(severityIcon(record.severity), 0.0f,
                              {.disabled = record.severity == InfoSeverity::Debug});
            ImGui::TableNextColumn();
            const std::string source = sourceLabel(record);
            ImGui::TextUnformatted(source.c_str());
            ImGui::TableNextColumn();
            // Plain text, never a widget label, so `##` in messages is shown.
            ImGui::TextUnformatted(record.text.c_str());
        }
    }
    clipper.End();
    // New records scroll into view while auto-scroll is on and the view rests
    // at the end; scrolling up pauses it until the user returns to the end.
    const bool arrived = m_drawnRevision != m_log.revision();
    m_drawnRevision = m_log.revision();
    if (m_autoScroll && arrived && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
        ImGui::SetScrollHereY(1.0f);
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::GetIO().KeyCtrl &&
        ImGui::IsKeyPressed(ImGuiKey_C, false)) {
        ImGui::SetClipboardText(copyText().c_str());
    }
    ImGui::EndTable();
}
