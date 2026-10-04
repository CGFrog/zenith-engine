#include "PreferencesWindow.hpp"

#include "editor/project/ExternalEditor.hpp"
#include "editor/project/ProjectFileOperations.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"
#include "editor/theme/PixelPainter.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <system_error>

namespace {
constexpr std::array<PreferenceSection, 5> sections{PreferenceSection::Interface, PreferenceSection::Keymap,
                                                    PreferenceSection::ExternalIde, PreferenceSection::Saving,
                                                    PreferenceSection::Build};
// The shortest autosave interval the Saving section offers; 0 is off.
constexpr float minimumAutosaveInterval = 5.0f;

bool blank(const std::string& text) {
    return text.find_first_not_of(" \t") == std::string::npos;
}

std::filesystem::path resolvedDirectory(const std::string& utf8, const std::filesystem::path& base) {
    const std::filesystem::path path = pathFromUtf8Text(utf8);
    std::filesystem::path normal = (path.is_absolute() ? path : base / path).lexically_normal();
    // "dir/." normalizes to "dir/"; compare folders without the separator.
    if (!normal.has_filename() && normal.has_relative_path()) {
        normal = normal.parent_path();
    }
    return normal;
}

// Copies one section's values from source into target.
void copySection(EditorPreferenceValues& target, const EditorPreferenceValues& source, PreferenceSection section) {
    switch (section) {
    case PreferenceSection::Interface:
        target.appearance = source.appearance;
        break;
    case PreferenceSection::Keymap:
        break;
    case PreferenceSection::ExternalIde:
        target.tools.ideCommand = source.tools.ideCommand;
        break;
    case PreferenceSection::Saving:
        target.autosave = source.autosave;
        break;
    case PreferenceSection::Build:
        target.tools.buildCommand = source.tools.buildCommand;
        target.tools.buildDirectory = source.tools.buildDirectory;
        target.tools.packageDirectory = source.tools.packageDirectory;
        target.tools.buildOnSave = source.tools.buildOnSave;
        break;
    }
}

const char* sectionId(PreferenceSection section) {
    switch (section) {
    case PreferenceSection::Interface:
        return "interface";
    case PreferenceSection::Keymap:
        return "keymap";
    case PreferenceSection::ExternalIde:
        return "ide";
    case PreferenceSection::Saving:
        return "saving";
    case PreferenceSection::Build:
        return "build";
    }
    return "section";
}

void drawProblems(std::span<const PreferenceProblem> problems, PreferenceSection section) {
    for (const PreferenceProblem& problem : problems) {
        if (problem.section != section) {
            continue;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, toImVec4(palette::error));
        ImGui::TextWrapped("%s", problem.message.c_str());
        ImGui::PopStyleColor();
    }
}

// A swatch per accent; the chosen one carries the pixel glow ring.
bool drawAccentSwatches(ThemeAccent& chosen) {
    bool changed = false;
    const float side = ImGui::GetFrameHeight();
    for (const ThemeAccentInfo& accent : themeAccents()) {
        ImGui::PushID(accent.key.data(), accent.key.data() + accent.key.size());
        if (ImGui::ColorButton("##accent", toImVec4(accent.color),
                               ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop |
                                   ImGuiColorEditFlags_NoAlpha,
                               {side, side})) {
            changed = changed || chosen != accent.accent;
            chosen = accent.accent;
        }
        ImGui::SetItemTooltip("%.*s", static_cast<int>(accent.label.size()), accent.label.data());
        if (chosen == accent.accent) {
            pixel::glowRing(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                            widgets::color(palette::captionInk), editorArtPixel());
        }
        ImGui::PopID();
        ImGui::SameLine();
    }
    ImGui::TextUnformatted(themeAccentInfo(chosen).label.data(),
                           themeAccentInfo(chosen).label.data() + themeAccentInfo(chosen).label.size());
    return changed;
}
} // namespace

EditorPreferenceValues preferenceValues(const EditorSettings& settings) {
    return {.appearance = settings.appearance(), .autosave = settings.autosave(), .tools = settings.tools()};
}

void storePreferenceValues(EditorSettings& settings, const EditorPreferenceValues& values) {
    settings.appearance() = values.appearance;
    settings.autosave() = values.autosave;
    settings.tools() = values.tools;
}

std::vector<PreferenceProblem> validatePreferences(const EditorPreferenceValues& values,
                                                   const std::filesystem::path& workingDirectory) {
    std::vector<PreferenceProblem> problems;
    const auto report = [&](PreferenceSection section, std::string message) {
        problems.push_back({.section = section, .message = std::move(message)});
    };

    if (blank(values.tools.ideCommand)) {
        report(PreferenceSection::ExternalIde, "The IDE command is empty; compiler errors could not be opened.");
    } else if (const ExternalEditorCommandResult expanded =
                   expandExternalEditorCommand(values.tools.ideCommand, {"file.cpp", 1, 1});
               !expanded) {
        report(PreferenceSection::ExternalIde, "The IDE command is invalid: " + expanded.error);
    }

    if (blank(values.tools.buildCommand)) {
        report(PreferenceSection::Build, "The build command is empty; the Build panel would have nothing to run.");
    }
    std::error_code error;
    const std::filesystem::path buildDirectory = resolvedDirectory(values.tools.buildDirectory, workingDirectory);
    if (!values.tools.buildDirectory.empty() && !std::filesystem::is_directory(buildDirectory, error)) {
        report(PreferenceSection::Build, "The build directory does not exist: " + utf8PathText(buildDirectory));
    }
    if (blank(values.tools.packageDirectory)) {
        report(PreferenceSection::Build, "The package build directory is empty; packaging needs a release tree of "
                                         "its own, such as build-release.");
    } else if (resolvedDirectory(values.tools.packageDirectory, buildDirectory) == buildDirectory) {
        report(PreferenceSection::Build,
               "The package build directory cannot be the build directory itself; use a folder inside it, such as "
               "build-release.");
    }

    const float interval = values.autosave.intervalSeconds;
    if (!std::isfinite(interval) || interval < 0.0f || interval > EditorAutosaveSettings::maximumIntervalSeconds) {
        report(PreferenceSection::Saving, "The autosave interval must be from 0 (off) to 3600 seconds.");
    }
    if (values.appearance.theme != clampedThemeParameters(values.appearance.theme) ||
        values.appearance.uiScale > maximumUiScale) {
        report(PreferenceSection::Interface, "A theme value is outside its supported range.");
    }
    return problems;
}

EditorPreferenceValues resetPreferenceSection(EditorPreferenceValues values, PreferenceSection section) {
    copySection(values, EditorPreferenceValues{}, section);
    return values;
}

void PreferencesWindow::open() {
    m_open = true;
    // Editing restarts from the stored settings.
    m_synced = false;
}

bool PreferencesWindow::isOpen() const {
    return m_open;
}

bool PreferencesWindow::draw(EditorSettings& settings, const PreferencesContext& context) {
    if (!m_open) {
        return false;
    }
    const float artPixel = editorArtPixel();
    ImGui::SetNextWindowSize({300.0f * artPixel, 330.0f * artPixel}, ImGuiCond_FirstUseEver);
    bool changed = false;
    if (ImGui::Begin("Preferences", &m_open, ImGuiWindowFlags_NoDocking)) {
        changed = drawContents(settings, context);
    }
    ImGui::End();
    return changed;
}

bool PreferencesWindow::drawContents(EditorSettings& settings, const PreferencesContext& context) {
    syncWithSettings(settings, context.workingDirectory);
    bool edited = false;
    if (widgets::SectionHeader("Interface", ImGuiTreeNodeFlags_DefaultOpen)) {
        edited = drawInterface(context) || edited;
    }
    if (widgets::SectionHeader("Keymap", ImGuiTreeNodeFlags_DefaultOpen)) {
        drawKeymap(context);
    }
    if (widgets::SectionHeader("External IDE", ImGuiTreeNodeFlags_DefaultOpen)) {
        edited = drawExternalIde(context) || edited;
    }
    if (widgets::SectionHeader("Saving", ImGuiTreeNodeFlags_DefaultOpen)) {
        edited = drawSaving() || edited;
    }
    if (widgets::SectionHeader("Build", ImGuiTreeNodeFlags_DefaultOpen)) {
        edited = drawBuild() || edited;
    }
    if (!edited) {
        return false;
    }
    m_problems = validatePreferences(m_draft, context.workingDirectory);
    return commitValidSections(settings);
}

const EditorPreferenceValues& PreferencesWindow::draft() const {
    return m_draft;
}

std::span<const PreferenceProblem> PreferencesWindow::problems() const {
    return m_problems;
}

const std::string& PreferencesWindow::ideTestResult() const {
    return m_ideTestResult;
}

void PreferencesWindow::syncWithSettings(const EditorSettings& settings,
                                         const std::filesystem::path& workingDirectory) {
    const EditorPreferenceValues current = preferenceValues(settings);
    if (m_synced && current == m_committed) {
        return;
    }
    m_committed = current;
    m_draft = current;
    m_problems = validatePreferences(m_draft, workingDirectory);
    if (current.autosave.intervalSeconds > 0.0f) {
        m_enabledAutosaveInterval = current.autosave.intervalSeconds;
    }
    m_synced = true;
}

bool PreferencesWindow::commitValidSections(EditorSettings& settings) {
    EditorPreferenceValues next = m_committed;
    for (const PreferenceSection section : sections) {
        const bool valid = std::ranges::none_of(
            m_problems, [section](const PreferenceProblem& problem) { return problem.section == section; });
        if (valid) {
            copySection(next, m_draft, section);
        }
    }
    if (next == m_committed) {
        return false;
    }
    storePreferenceValues(settings, next);
    m_committed = next;
    return true;
}

bool PreferencesWindow::drawInterface(const PreferencesContext& context) {
    ImGui::PushID(sectionId(PreferenceSection::Interface));
    EditorAppearanceSettings& appearance = m_draft.appearance;
    ThemeParameters& theme = appearance.theme;
    bool edited = ImGui::Checkbox("Wallpaper", &theme.wallpaper);
    ImGui::BeginDisabled(!theme.wallpaper);
    int frost = static_cast<int>(theme.frostRadius);
    if (ImGui::SliderInt("Frost", &frost, 0, static_cast<int>(ThemeParameters::maximumFrostRadius), "%d cells",
                         ImGuiSliderFlags_AlwaysClamp)) {
        theme.frostRadius = static_cast<uint32_t>(frost);
        edited = true;
    }
    ImGui::SetItemTooltip("How much the glass blurs the wallpaper behind it.");
    edited = ImGui::SliderFloat("Glass opacity", &theme.glassOpacity, ThemeParameters::minimumGlassOpacity,
                                ThemeParameters::maximumGlassOpacity, "%.2f", ImGuiSliderFlags_AlwaysClamp) ||
             edited;
    ImGui::EndDisabled();
    edited = ImGui::SliderFloat("Panel opacity", &theme.paneOpacity, ThemeParameters::minimumPaneOpacity, 1.0f, "%.2f",
                                ImGuiSliderFlags_AlwaysClamp) ||
             edited;
    ImGui::SetItemTooltip("Panels stay opaque enough for every text colour to read over the wallpaper.");
    ImGui::TextUnformatted("Accent");
    ImGui::SameLine();
    edited = drawAccentSwatches(theme.accent) || edited;

    ImGui::TextUnformatted("UI scale");
    const std::string automatic =
        "Automatic (" + std::to_string(static_cast<int>(artPixelForScale(context.displayContentScale))) + "x)";
    if (ImGui::RadioButton(automatic.c_str(), appearance.uiScale == 0)) {
        appearance.uiScale = 0;
        edited = true;
    }
    for (uint32_t scale = 1; scale <= maximumUiScale; ++scale) {
        ImGui::SameLine();
        const std::string label = std::to_string(scale) + "x";
        if (ImGui::RadioButton(label.c_str(), appearance.uiScale == scale)) {
            appearance.uiScale = scale;
            edited = true;
        }
    }
    ImGui::TextDisabled("Whole multipliers keep the pixel grid crisp.");
    edited = ImGui::Checkbox("Pie menus", &appearance.pieMenus) || edited;
    ImGui::SetItemTooltip("Z, ` and . open shading, view, and pivot pies around the mouse instead of menus.");
    edited = drawSectionFooter(PreferenceSection::Interface) || edited;
    ImGui::PopID();
    return edited;
}

void PreferencesWindow::drawKeymap(const PreferencesContext& context) {
    ImGui::PushID(sectionId(PreferenceSection::Keymap));
    ImGui::TextDisabled("Shortcuts are saved in");
    ImGui::TextWrapped("%s", utf8PathText(context.keymapPath).c_str());
    if (widgets::Button("Edit Keymap...") && context.openKeymapEditor) {
        context.openKeymapEditor();
    }
    ImGui::PopID();
}

bool PreferencesWindow::drawExternalIde(const PreferencesContext& context) {
    ImGui::PushID(sectionId(PreferenceSection::ExternalIde));
    ImGui::SetNextItemWidth(-FLT_MIN);
    bool edited =
        ImGui::InputTextWithHint("##ide_command", R"(code -g "{file}:{line}:{column}")", &m_draft.tools.ideCommand);
    ImGui::TextDisabled("Placeholders: {file} {line} {column}");
    if (widgets::Button("Test")) {
        const ChildProcessResult opened = openInExternalEditor(m_draft.tools.ideCommand, {context.ideTestFile, 1, 1});
        m_ideTestFailed = !opened;
        m_ideTestResult = opened ? "Started the IDE on " + utf8PathText(context.ideTestFile)
                                 : "The IDE did not start: " + opened.error;
    }
    if (!m_ideTestResult.empty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, toImVec4(m_ideTestFailed ? palette::error : palette::success));
        ImGui::TextWrapped("%s", m_ideTestResult.c_str());
        ImGui::PopStyleColor();
    }
    edited = drawSectionFooter(PreferenceSection::ExternalIde) || edited;
    ImGui::PopID();
    return edited;
}

bool PreferencesWindow::drawSaving() {
    ImGui::PushID(sectionId(PreferenceSection::Saving));
    float& interval = m_draft.autosave.intervalSeconds;
    bool enabled = interval > 0.0f;
    bool edited = false;
    if (ImGui::Checkbox("Autosave a recovery copy", &enabled)) {
        interval = enabled ? m_enabledAutosaveInterval : 0.0f;
        edited = true;
    }
    ImGui::BeginDisabled(!enabled);
    float shown = enabled ? interval : m_enabledAutosaveInterval;
    if (ImGui::SliderFloat("Every", &shown, minimumAutosaveInterval, EditorAutosaveSettings::maximumIntervalSeconds,
                           "%.0f s", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic)) {
        m_enabledAutosaveInterval = shown;
        interval = shown;
        edited = true;
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Unsaved scenes are copied for recovery after a crash; saving is never automatic.");
    edited = drawSectionFooter(PreferenceSection::Saving) || edited;
    ImGui::PopID();
    return edited;
}

bool PreferencesWindow::drawBuild() {
    ImGui::PushID(sectionId(PreferenceSection::Build));
    bool edited = ImGui::InputTextWithHint("Build command", "Run by the shell", &m_draft.tools.buildCommand);
    edited = ImGui::InputTextWithHint("Build directory", "Editor working directory", &m_draft.tools.buildDirectory) ||
             edited;
    ImGui::SetItemTooltip("Where the build command runs. Build and Package also configures CMake from here.");
    edited = ImGui::InputTextWithHint("Package directory", "build-release", &m_draft.tools.packageDirectory) || edited;
    ImGui::SetItemTooltip("The release tree Build and Package uses, inside the build directory.");
    edited = ImGui::Checkbox("Build on save", &m_draft.tools.buildOnSave) || edited;
    ImGui::SetItemTooltip("Saving a source file in the project's src folder builds the project, as Ctrl B does.");
    edited = drawSectionFooter(PreferenceSection::Build) || edited;
    ImGui::PopID();
    return edited;
}

bool PreferencesWindow::drawSectionFooter(PreferenceSection section) {
    drawProblems(m_problems, section);
    if (!widgets::Button("Reset to Defaults")) {
        return false;
    }
    m_draft = resetPreferenceSection(m_draft, section);
    if (section == PreferenceSection::Saving) {
        m_enabledAutosaveInterval = m_draft.autosave.intervalSeconds;
    }
    return true;
}
