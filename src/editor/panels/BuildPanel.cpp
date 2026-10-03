#include "BuildPanel.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/project/ExternalEditor.hpp"
#include "editor/project/PackageRunner.hpp"
#include "editor/project/ProjectFileOperations.hpp"
#include "editor/settings/EditorSettings.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"

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
        return toImVec4(palette::error);
    case CompilerDiagnosticSeverity::Warning:
        return toImVec4(palette::warning);
    case CompilerDiagnosticSeverity::Note:
        return toImVec4(palette::info);
    }
    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

std::string describe(const CompilerDiagnostic& diagnostic) {
    return diagnostic.code.empty() ? diagnostic.message : diagnostic.code + ": " + diagnostic.message;
}
} // namespace

BuildPanel::BuildPanel(EditorToolSettings& tools, PackageRunner* package)
    : m_tools(tools)
    , m_package(package) {}

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
    const ChildProcessResult started = m_runner.start(buildRequest());
    std::vector<EditorDiagnostic> replacements;
    if (!started) {
        replacements.push_back({.severity = EditorDiagnosticSeverity::Error,
                                .message = "Could not start the build: " + started.error,
                                .action = "Check build.command and build.directory in the Build panel."});
    }
    diagnostics.replaceSource(buildSource, std::move(replacements));
    m_view = View::Build;
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

void BuildPanel::setProjectBuild(std::optional<BuildRequest> projectBuild) {
    m_projectBuild = std::move(projectBuild);
}

bool BuildPanel::usesProjectBuild() const {
    const EditorToolSettings defaults;
    return m_projectBuild && m_tools.buildCommand == defaults.buildCommand &&
           m_tools.buildDirectory == defaults.buildDirectory;
}

BuildRequest BuildPanel::buildRequest() const {
    if (usesProjectBuild()) {
        return *m_projectBuild;
    }
    // Settings text is UTF-8; a narrow path would use the ANSI code page.
    return {.command = m_tools.buildCommand,
            .workingDirectory =
                std::filesystem::path(std::u8string(m_tools.buildDirectory.begin(), m_tools.buildDirectory.end()))};
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

void BuildPanel::followPackage() {
    const bool packageRunning = m_package != nullptr && m_package->running();
    if (packageRunning && !m_packageWasRunning) {
        m_view = View::Package;
        m_scrollToEnd = true;
    }
    m_packageWasRunning = packageRunning;
}

void BuildPanel::drawContents(EditorDiagnostics& diagnostics) {
    followPackage();
    drawToolbar(diagnostics);
    const bool showPackage = m_view == View::Package && m_package != nullptr;
    if (showPackage) {
        drawPackageStatus();
    } else {
        drawStatus();
    }
    ImGui::Separator();
    drawOutput(showPackage ? m_package->output() : m_runner.output(), diagnostics);
}

void BuildPanel::drawToolbar(EditorDiagnostics& diagnostics) {
    const bool running = m_runner.running();
    if (running) {
        if (widgets::Button("Cancel")) {
            m_runner.cancel();
        }
    } else if (widgets::Button("Build")) {
        startBuild(diagnostics);
    }
    if (m_package != nullptr && m_package->state() != PackageState::Idle) {
        ImGui::SameLine();
        if (widgets::Button("Build Output", {}, m_view == View::Build)) {
            m_view = View::Build;
        }
        ImGui::SameLine();
        if (widgets::Button("Package Output", {}, m_view == View::Package)) {
            m_view = View::Package;
        }
        if (m_package->running()) {
            ImGui::SameLine();
            if (widgets::Button("Cancel Package")) {
                m_package->cancel();
            }
        }
    }
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &m_followOutput);
    ImGui::SameLine();
    ImGui::BeginDisabled(running);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##command", "Build command, run by the shell", &m_tools.buildCommand);
    ImGui::EndDisabled();
    if (usesProjectBuild()) {
        const std::u8string directory = m_projectBuild->workingDirectory.u8string();
        ImGui::TextDisabled("Builds the project: %s (in %s). Change the command or directory to run them instead.",
                            m_projectBuild->command.c_str(), std::string(directory.begin(), directory.end()).c_str());
    }
    if (widgets::SectionHeader("Settings")) {
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
        ImGui::TextColored(toImVec4(palette::success), "%sBuild succeeded", widgets::IconLabelPadding());
        widgets::ItemIcon(EditorIcon::Success, 0.0f);
        break;
    case BuildState::Failed:
        ImGui::TextColored(severityColor(CompilerDiagnosticSeverity::Error), "%sBuild failed (exit code %d)",
                           widgets::IconLabelPadding(), m_runner.exitCode().value_or(-1));
        widgets::ItemIcon(EditorIcon::Error, 0.0f);
        break;
    case BuildState::Cancelled:
        ImGui::TextDisabled("Build cancelled");
        break;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu error lines, %zu warning lines", errors, warnings);
}

void BuildPanel::drawPackageStatus() const {
    const PackagePlan& plan = m_package->plan();
    switch (m_package->state()) {
    case PackageState::Idle:
        ImGui::TextDisabled("No package has been built.");
        return;
    case PackageState::Running:
        ImGui::Text("Packaging: step %zu of %zu, %s...", m_package->currentStep() + 1, plan.steps.size(),
                    plan.steps[m_package->currentStep()].label.c_str());
        return;
    case PackageState::Succeeded:
        ImGui::TextColored(toImVec4(palette::success), "%sPackage built in %s", widgets::IconLabelPadding(),
                           utf8PathText(plan.buildDirectory).c_str());
        widgets::ItemIcon(EditorIcon::Success, 0.0f);
        return;
    case PackageState::Failed:
        ImGui::TextColored(toImVec4(palette::error), "%sPackaging failed: %s", widgets::IconLabelPadding(),
                           m_package->failure().c_str());
        widgets::ItemIcon(EditorIcon::Error, 0.0f);
        return;
    case PackageState::Cancelled:
        ImGui::TextDisabled("Packaging cancelled");
        return;
    }
}

void BuildPanel::drawOutput(const std::deque<BuildOutputLine>& output, EditorDiagnostics& diagnostics) {
    if (!ImGui::BeginChild("Build output", {0.0f, 0.0f}, ImGuiChildFlags_Borders,
                           ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::EndChild();
        return;
    }
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
