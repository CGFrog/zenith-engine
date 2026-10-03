#pragma once

#include "editor/panels/EditorPanel.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class EditorDiagnostics;
class Logger;
class OperatorHistory;

enum class InfoSeverity : uint8_t {
    Debug,
    Info,
    Warning,
    Error
};

// Where an Info record came from. Editor diagnostics from the build and the
// game module host get their own sources; every other diagnostic is Editor.
enum class InfoSource : uint8_t {
    Engine,
    Editor,
    Build,
    GameModule,
    Operator
};

inline constexpr size_t infoSeverityCount = 4;
inline constexpr size_t infoSourceCount = 5;

std::string_view infoSeverityName(InfoSeverity severity);
std::string_view infoSourceName(InfoSource source);

struct InfoRecord {
    InfoSource source{InfoSource::Engine};
    InfoSeverity severity{InfoSeverity::Info};
    // The engine log category, the diagnostic's source, or the operator id.
    std::string category;
    std::string text;
    // The engine log sequence, diagnostic id, or operator history sequence:
    // unique within the source.
    uint64_t originId{0};
    std::chrono::system_clock::time_point wallTime;
    std::chrono::steady_clock::time_point steadyTime;
};

// "12:34:56.789 [Warning] Engine/graphics: message", as copied to the clipboard.
std::string formatInfoRecord(const InfoRecord& record);

struct InfoFilter {
    std::array<bool, infoSeverityCount> severities{true, true, true, true};
    std::array<bool, infoSourceCount> sources{true, true, true, true, true};
    // Case-insensitive substring of the text or category; empty accepts all.
    std::string text;

    bool accepts(const InfoRecord& record) const;
    bool operator==(const InfoFilter&) const = default;
};

// The merged, time-ordered model behind the Info panel: engine log entries and
// operator invocations are copied in as they arrive (bounded), and editor
// diagnostics are mirrored as they currently are. CPU-only.
class InfoLog {
  public:
    explicit InfoLog(size_t streamCapacity = 4096);

    // Copies engine entries and operator invocations newer than the last sync
    // and re-reads diagnostics when they changed. Any input may be null.
    // Returns whether records() changed.
    bool sync(const Logger* engineLog, const EditorDiagnostics* diagnostics, const OperatorHistory* operators);

    // Oldest first, by steady time.
    const std::vector<InfoRecord>& records() const {
        return m_records;
    }

    size_t count(InfoSource source) const;
    // Hides every engine and operator record received so far. The engine log
    // and operator history keep theirs; diagnostics are cleared at their owner.
    void clearStreams();

    // Increases whenever records() changes.
    uint64_t revision() const {
        return m_revision;
    }

  private:
    void appendStreamed(InfoRecord record);
    void rebuild();

    size_t m_streamCapacity;
    std::deque<InfoRecord> m_streamed;
    std::vector<InfoRecord> m_diagnosticRecords;
    std::vector<InfoRecord> m_records;
    uint64_t m_engineSequence{0};
    uint64_t m_operatorSequence{0};
    bool m_diagnosticsSeen{false};
    uint64_t m_diagnosticsRevision{0};
    uint64_t m_revision{0};
};

// A unified, filterable view of the engine log, editor diagnostics (including
// build and game module output), and operator history, with auto-scroll,
// copy, and clear.
class InfoPanel final : public EditorPanel {
  public:
    // operators is the runner's history; it must outlive the panel.
    explicit InfoPanel(const OperatorHistory& operators);

    const char* title() const override;
    void update(EditorContext& context, const InputState& input, float deltaSeconds) override;
    void draw(EditorContext& context) override;

    // The session-independent halves of update() and draw(), for tests.
    void sync(const Logger* engineLog, const EditorDiagnostics& diagnostics);
    void drawContents(EditorDiagnostics& diagnostics);

    const InfoLog& log() const {
        return m_log;
    }

    InfoFilter& filter() {
        return m_filter;
    }

    // Indices into log().records() that pass the filter, oldest first.
    const std::vector<size_t>& visibleRecords();
    // The selected visible records, or every visible record when none is
    // selected, one formatted line each.
    std::string copyText();
    void clear(EditorDiagnostics& diagnostics);

  private:
    using RecordKey = std::pair<InfoSource, uint64_t>;

    void drawToolbar(EditorDiagnostics& diagnostics);
    void drawRecords();
    void selectRecord(const InfoRecord& record, bool toggle);

    const OperatorHistory& m_operators;
    InfoLog m_log;
    InfoFilter m_filter;
    std::array<char, 256> m_searchBuffer{};
    std::vector<size_t> m_visible;
    uint64_t m_visibleRevision{0};
    InfoFilter m_visibleFilter;
    bool m_visibleValid{false};
    std::set<RecordKey> m_selected;
    bool m_autoScroll{true};
    uint64_t m_drawnRevision{0};
};
