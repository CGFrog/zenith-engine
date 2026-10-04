#include "HeadlessImGui.hpp"
#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/operators/ObjectOperators.hpp"
#include "editor/operators/OperatorRunner.hpp"
#include "editor/panels/InfoPanel.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/diagnostics/Logger.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

// The objects the editor session owns that feed the Info panel, without a
// window: a real operator runner editing a real scene, diagnostics, and an
// engine logger.
struct InfoFixture {
    Logger logger{{.minimumLevel = LogLevel::Debug, .echoToStderr = false}};
    AssetManager assets{&logger};
    Scene scene;
    EditorDocument document{std::filesystem::temp_directory_path()};
    CommandHistory history;
    EditorDiagnostics diagnostics;
    Selection selection;
    OperatorRegistry registry;
    OperatorRunner runner{registry, history};

    InfoFixture() {
        expect(registerObjectOperators(registry).success, "the object operators should register");
    }

    OperatorResult invoke(std::string_view id, std::string_view properties = {}) {
        OperatorContext context{.editing = SceneEditing(scene, assets, document, history, diagnostics),
                                .selection = selection,
                                .mode = EditorMode::Edit};
        return runner.invokeWithJson(id, context, properties);
    }
};

void testOperatorHistory() {
    InfoFixture fixture;
    expect(fixture.invoke("object.add_empty").status == OperatorStatus::Finished, "adding an empty should finish");
    // Deletes the new empty, so the next delete has nothing selected.
    fixture.invoke("object.delete");
    expect(fixture.invoke("object.delete").status == OperatorStatus::Cancelled,
           "deleting with nothing selected should be cancelled");
    expect(fixture.invoke("object.missing").status == OperatorStatus::Failed, "an unknown operator should fail");
    expect(fixture.invoke("object.select_all", R"({"action":"sideways"})").status == OperatorStatus::Failed,
           "invalid properties should fail");

    const std::deque<OperatorHistoryEntry>& entries = fixture.runner.history().entries();
    expect(entries.size() == 5, "every invocation should be recorded, whatever its outcome");
    if (entries.size() == 5) {
        expect(entries[0].id == "object.add_empty" && !entries[0].label.empty() &&
                   entries[0].status == OperatorStatus::Finished && entries[0].propertiesJson.starts_with("{"),
               "a finished invocation should record its id, label, and properties");
        expect(entries[2].status == OperatorStatus::Cancelled && !entries[2].message.empty(),
               "a cancelled invocation should record why");
        expect(entries[3].id == "object.missing" && entries[3].label.empty() &&
                   entries[3].status == OperatorStatus::Failed,
               "an unknown operator should be recorded as failed");
        expect(entries[4].status == OperatorStatus::Failed &&
                   entries[4].propertiesJson.find("sideways") != std::string::npos,
               "a properties error should record the given JSON");
        expect(entries[0].sequence == 1 && entries[4].sequence == 5 && entries[4].steadyTime >= entries[0].steadyTime,
               "entries should be sequenced and timestamped in order");
    }

    OperatorHistory bounded(2);
    for (int index = 0; index < 3; ++index) {
        bounded.record({.id = "test." + std::to_string(index), .status = OperatorStatus::Finished});
    }
    expect(bounded.entries().size() == 2 && bounded.entries().front().id == "test.1" && bounded.latestSequence() == 3,
           "the history should keep only its newest entries");
}

void testMergedModel() {
    InfoFixture fixture;
    InfoLog log;
    fixture.logger.log(LogLevel::Warning, "graphics", "First engine message");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    fixture.diagnostics.add(EditorDiagnosticSeverity::Error, "Build", "main.cpp failed to compile");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    fixture.invoke("object.add_empty");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    fixture.diagnostics.add(EditorDiagnosticSeverity::Warning, "Game Module", "Module reloaded with warnings");
    fixture.logger.log(LogLevel::Info, "assets", "Second engine message");

    expect(log.sync(&fixture.logger, &fixture.diagnostics, &fixture.runner.history()), "a first sync should change");
    const std::vector<InfoRecord>& records = log.records();
    expect(records.size() == 5, "every source should be merged");
    expect(log.count(InfoSource::Engine) == 2 && log.count(InfoSource::Build) == 1 &&
               log.count(InfoSource::GameModule) == 1 && log.count(InfoSource::Operator) == 1,
           "records should be attributed to their sources");
    expect(records.size() == 5 && records[0].text == "First engine message" && records[1].source == InfoSource::Build &&
               records[2].source == InfoSource::Operator,
           "records should be ordered by time across sources");
    expect(!log.sync(&fixture.logger, &fixture.diagnostics, &fixture.runner.history()),
           "a sync without new input should change nothing");

    fixture.logger.log(LogLevel::Error, "graphics", "Third engine message");
    expect(log.sync(&fixture.logger, &fixture.diagnostics, &fixture.runner.history()) && log.records().size() == 6,
           "new engine entries should be appended");

    InfoFilter filter;
    filter.severities[static_cast<size_t>(InfoSeverity::Info)] = false;
    expect(std::ranges::count_if(log.records(),
                                 [&filter](const InfoRecord& record) { return filter.accepts(record); }) == 4,
           "severity filters should hide records");
    filter = {};
    filter.sources[static_cast<size_t>(InfoSource::Engine)] = false;
    expect(std::ranges::count_if(log.records(),
                                 [&filter](const InfoRecord& record) { return filter.accepts(record); }) == 3,
           "source filters should hide records");
    filter = {};
    filter.text = "ENGINE MESSAGE";
    expect(std::ranges::count_if(log.records(),
                                 [&filter](const InfoRecord& record) { return filter.accepts(record); }) == 3,
           "text filters should ignore case");
    filter.text = "graphics";
    expect(std::ranges::count_if(log.records(),
                                 [&filter](const InfoRecord& record) { return filter.accepts(record); }) == 2,
           "text filters should match the category");

    const std::string formatted = formatInfoRecord(log.records().front());
    expect(formatted.find("[Warning] Engine/graphics: First engine message") != std::string::npos,
           "a formatted record should show severity, source, and text");

    log.clearStreams();
    expect(log.count(InfoSource::Engine) == 0 && log.count(InfoSource::Operator) == 0 &&
               log.count(InfoSource::Build) == 1,
           "clearing streams should hide engine and operator records only");
    expect(fixture.logger.entries().size() == 3, "clearing the view should keep the engine log");
    fixture.logger.log(LogLevel::Info, "graphics", "After clear");
    log.sync(&fixture.logger, &fixture.diagnostics, &fixture.runner.history());
    expect(log.count(InfoSource::Engine) == 1, "only entries after a clear should reappear");
}

void testHeadlessPanel() {
    InfoFixture fixture;
    InfoPanel panel(fixture.runner.history());
    for (int index = 0; index < 2'000; ++index) {
        fixture.logger.log(index % 2 == 0 ? LogLevel::Info : LogLevel::Warning, "stress",
                           "Line " + std::to_string(index) + " with ## and %s in it");
    }
    fixture.invoke("object.add_empty");
    fixture.diagnostics.reportError("Editor failure");
    panel.sync(&fixture.logger, fixture.diagnostics);
    expect(panel.log().records().size() == 2'002, "the panel should merge every source");
    {
        HeadlessFrame frame("Info test");
        panel.drawContents(fixture.diagnostics);
    }
    {
        // A second frame after the first layout pass exercises auto-scroll.
        HeadlessFrame frame("Info test");
        panel.drawContents(fixture.diagnostics);
    }

    panel.filter().severities[static_cast<size_t>(InfoSeverity::Warning)] = false;
    expect(panel.visibleRecords().size() == 1'002, "the panel should apply its filter");
    const std::string copied = panel.copyText();
    expect(std::ranges::count(copied, '\n') == 1'002, "copy with nothing selected should copy every shown row");
    panel.filter() = {};
    panel.filter().text = "editor failure";
    expect(panel.visibleRecords().size() == 1, "the text filter should narrow the rows");
    {
        HeadlessFrame frame("Info test");
        panel.drawContents(fixture.diagnostics);
    }

    panel.clear(fixture.diagnostics);
    expect(fixture.diagnostics.entries().empty(), "clear should clear editor diagnostics");
    expect(panel.log().records().empty(), "clear should empty the view");
    expect(fixture.logger.entries().size() == 2'000, "clear should leave the engine log intact");
}
} // namespace

// A game's gameplay.<name> channels are categories the panel can hide.
void testCategoryFilter() {
    InfoFixture fixture;
    LogChannel(&fixture.logger, "gameplay.weapons").info("fired");
    LogChannel(&fixture.logger, "gameplay.ai").info("thinking");
    InfoPanel panel(fixture.runner.history());
    panel.sync(&fixture.logger, fixture.diagnostics);
    expect(panel.log().categories().contains("gameplay.weapons") && panel.log().categories().contains("gameplay.ai"),
           "the log should collect the categories it received");
    const auto shown = [&panel](std::string_view text) {
        const std::vector<InfoRecord>& records = panel.log().records();
        return std::ranges::any_of(panel.visibleRecords(),
                                   [&records, text](size_t index) { return records[index].text == text; });
    };
    expect(shown("fired") && shown("thinking"), "every category should show by default");
    panel.filter().hiddenCategories.insert("gameplay.ai");
    expect(shown("fired") && !shown("thinking"), "a hidden category's records should be filtered out");
    panel.clear(fixture.diagnostics);
    expect(panel.log().categories().contains("gameplay.ai"), "Clear should keep the categories, to show them again");
    {
        HeadlessImGuiContext imgui;
        HeadlessImGuiFrame frame("Info");
        panel.drawContents(fixture.diagnostics);
    }
}

int main() {
    testOperatorHistory();
    testMergedModel();
    testHeadlessPanel();
    testCategoryFilter();
    if (failures == 0) {
        std::cout << "Info panel tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
