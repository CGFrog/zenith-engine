#include "BuildPanel.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/project/ExternalEditor.hpp"
#include "editor/settings/EditorSettings.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <cfloat>
#include <utility>
#include <vector>

namespace {
constexpr const char* buildSource = "Build";

EditorDiagnosticSeverity editorSeverity(CompilerDiagnosticSeverity severity) {
    switch (severity) {
    case CompilerDiagnosticSeverity::Error:
        return EditorDiagnosticSeverity::Error;
    case CompilerDiagnosticSeverity::Warning:
        return EditorDiagnosticSeverity::Warning;
    case CompilerDiagnosticSeverity::Note:
        return EditorDiagnosticSeverity::Info;
    }
    return EditorDiagnosticSeverity::Info;
}

ImVec4 severityColor(CompilerDiagnosticSeverity severity) {
    switch (severity) {
    case CompilerDiagnosticSeverity::Error:
        return {1.0f, 0.42f, 0.42f, 1.0f};
    case CompilerDiagnosticSeverity::Warning:
        return {1.0f, 0.82f, 0.36f, 1.0f};
    case CompilerDiagnosticSeverity::Note:
        return {0.55f, 0.75f, 1.0f, 1.0f};
    }
    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

std::string describe(const CompilerDiagnostic& diagnostic) {
    return diagnostic.code.empty() ? diagnostic.message : diagnostic.code + ": " + diagnostic.message;
}
} // namespace

BuildPanel::BuildPanel(EditorToolSettings& tools)
    : m_tools(tools) {}

const char* BuildPanel::title() const {
    return "Build";
}

void BuildPanel::update(EditorContext& context, const InputState&, float) {
    // Builds keep streaming while the panel is closed.
    poll(context.diagnostics);
}

void BuildPanel::draw(EditorContext& context) {
    drawContents(context.diagnostics);
}

void BuildPanel::startBuild(EditorDiagnostics& diagnostics) {
    const ChildProcessResult started =
        m_runner.start({.command = m_tools.buildCommand,
                        // Settings text is UTF-8; a narrow path would use the ANSI code page.
                        .workingDirectory = std::filesystem::path(
                            std::u8string(m_tools.buildDirectory.begin(), m_tools.buildDirectory.end()))});
    std::vector<EditorDiagnostic> replacements;
    if (!started) {
        replacements.push_back({.severity = EditorDiagnosticSeverity::Error,
                                .message = "Could not start the build: " + started.error,
                                .action = "Check build.command and build.directory in the Build panel."});
    }
    diagnostics.replaceSource(buildSource, std::move(replacements));
    m_scrollToEnd = true;
}

void BuildPanel::poll(EditorDiagnostics& diagnostics) {
    if (m_runner.poll().finished) {
        publishDiagnostics(diagnostics);
    }
}

const BuildRunner& BuildPanel::runner() const {
    return m_runner;
}

void BuildPanel::publishDiagnostics(EditorDiagnostics& diagnostics) const {
    std::vector<EditorDiagnostic> replacements;
    bool reportedError = false;
    for (const CompilerDiagnostic& diagnostic : m_runner.diagnostics()) {
        reportedError = reportedError || diagnostic.severity == CompilerDiagnosticSeverity::Error;
        replacements.push_back({.severity = editorSeverity(diagnostic.severity),
                                .message = describe(diagnostic),
                                .location = diagnostic.location});
    }
    if (m_runner.state() == BuildState::Failed && !reportedError) {
        const int code = m_runner.exitCode().value_or(-1);
        replacements.push_back({.severity = EditorDiagnosticSeverity::Error,
                                .message = "Build failed with exit code " + std::to_string(code),
                                .action = "Review the Build panel output."});
    } else if (m_runner.state() == BuildState::Cancelled) {
        replacements.push_back({.severity = EditorDiagnosticSeverity::Info, .message = "Build cancelled"});
    }
    diagnostics.replaceSource(buildSource, std::move(replacements));
}

void BuildPanel::drawContents(EditorDiagnostics& diagnostics) {
    drawToolbar(diagnostics);
    drawStatus();
    ImGui::Separator();
    drawOutput(diagnostics);
}

void BuildPanel::drawToolbar(EditorDiagnostics& diagnostics) {
    const bool running = m_runner.running();
    if (running) {
        if (ImGui::Button("Cancel")) {
            m_runner.cancel();
        }
    } else if (ImGui::Button("Build")) {
        startBuild(diagnostics);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &m_followOutput);
    ImGui::SameLine();
    ImGui::BeginDisabled(running);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##command", "Build command, run by the shell", &m_tools.buildCommand);
    ImGui::EndDisabled();
    if (ImGui::CollapsingHeader("Settings")) {
        ImGui::BeginDisabled(running);
        ImGui::InputTextWithHint("Directory", "Editor working directory", &m_tools.buildDirectory);
        ImGui::EndDisabled();
        ImGui::InputTextWithHint("IDE command", R"(code -g "{file}:{line}:{column}")", &m_tools.ideCommand);
        ImGui::TextDisabled("Placeholders: {file} {line} {column}");
    }
}

void BuildPanel::drawStatus() const {
    size_t errors = 0;
    size_t warnings = 0;
    for (const BuildOutputLine& line : m_runner.output()) {
        errors += line.severity == CompilerDiagnosticSeverity::Error ? 1 : 0;
        warnings += line.severity == CompilerDiagnosticSeverity::Warning ? 1 : 0;
    }
    switch (m_runner.state()) {
    case BuildState::Idle:
        ImGui::TextDisabled("No build has run.");
        return;
    case BuildState::Running:
        ImGui::Text("Building...");
        break;
    case BuildState::Succeeded:
        ImGui::TextColored({0.45f, 0.85f, 0.45f, 1.0f}, "Build succeeded");
        break;
    case BuildState::Failed:
        ImGui::TextColored(severityColor(CompilerDiagnosticSeverity::Error), "Build failed (exit code %d)",
                           m_runner.exitCode().value_or(-1));
        break;
    case BuildState::Cancelled:
        ImGui::TextDisabled("Build cancelled");
        break;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu error lines, %zu warning lines", errors, warnings);
}

void BuildPanel::drawOutput(EditorDiagnostics& diagnostics) {
    if (!ImGui::BeginChild("Build output", {0.0f, 0.0f}, ImGuiChildFlags_Borders,
                           ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::EndChild();
        return;
    }
    const std::deque<BuildOutputLine>& output = m_runner.output();
    std::optional<EditorSourceLocation> clicked;
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(output.size()));
    while (clipper.Step()) {
        for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
            const BuildOutputLine& line = output[static_cast<size_t>(index)];
            if (!line.severity) {
                ImGui::TextUnformatted(line.text.c_str());
                continue;
            }
            // Drawn as plain text, never a widget label, so `##` in compiler
            // output is shown rather than read as an ImGui id separator.
            ImGui::PushStyleColor(ImGuiCol_Text, severityColor(*line.severity));
            ImGui::TextUnformatted(line.text.c_str());
            ImGui::PopStyleColor();
            if (line.location && ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("Open in IDE");
                if (ImGui::IsItemClicked()) {
                    clicked = line.location;
                }
            }
        }
    }
    clipper.End();
    // Follow new output while the view rests at the end; scrolling up pauses
    // following until the user returns to the end.
    if (std::exchange(m_scrollToEnd, false) || (m_followOutput && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    if (clicked) {
        openSourceLocation(m_tools.ideCommand, *clicked, diagnostics);
    }
}
