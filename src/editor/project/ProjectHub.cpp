#include "ProjectHub.hpp"

#include "editor/project/ExternalEditor.hpp"
#include "editor/project/ProjectTemplate.hpp"
#include "editor/settings/EditorSettings.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"
#include "editor/theme/ThemeChrome.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <array>
#include <cfloat>
#include <ctime>
#include <system_error>
#include <utility>

namespace {
constexpr const char* hubWindowName = "Project Hub";
constexpr const char* projectFileExtension = ".zenith-project";

std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

std::filesystem::path fromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string formatLastOpened(std::chrono::system_clock::time_point time) {
    if (time.time_since_epoch().count() == 0) {
        return "never";
    }
    const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
    std::tm local{};
#if defined(_WIN32)
    if (localtime_s(&local, &seconds) != 0) {
        return "unknown";
    }
#else
    if (localtime_r(&seconds, &local) == nullptr) {
        return "unknown";
    }
#endif
    std::array<char, 32> text{};
    if (std::strftime(text.data(), text.size(), "%Y-%m-%d %H:%M", &local) == 0) {
        return "unknown";
    }
    return text.data();
}

// Native dialogs need an existing absolute folder to start in; a trailing
// separator marks it as a folder with no suggested file name.
std::filesystem::path dialogFolder(const std::filesystem::path& folder) {
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(folder, error);
    if (folder.empty() || error || !std::filesystem::is_directory(absolute, error)) {
        return {};
    }
    return (absolute / "").make_preferred();
}
} // namespace

EditorSourceLocation projectIdeLocation(const std::filesystem::path& projectFile) {
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(projectFile, error);
    return {.file = (error ? projectFile : absolute).lexically_normal().parent_path().make_preferred(),
            .line = 1,
            .column = 1};
}

ChildProcessResult openProjectInIde(std::string_view ideCommand, const std::filesystem::path& projectFile) {
    return openInExternalEditor(ideCommand, projectIdeLocation(projectFile));
}

ProjectHub::ProjectHub(RecentProjects& recentProjects, ProjectHubConfig config)
    : m_recentProjects(recentProjects)
    , m_config(std::move(config))
    , m_openDialog("Zenith project", projectFileExtension)
    , m_newProjectLocation(utf8(m_config.defaultProjectLocation)) {}

std::optional<std::filesystem::path> ProjectHub::draw(AssetManager& assets, const ThemeChrome* chrome) {
    pollOpenDialog();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float artPixel = editorArtPixel();
    const ImVec2 margin{24.0f * artPixel, 16.0f * artPixel};
    ImGui::SetNextWindowPos({viewport->WorkPos.x + margin.x, viewport->WorkPos.y + margin.y});
    ImGui::SetNextWindowSize({viewport->WorkSize.x - 2.0f * margin.x, viewport->WorkSize.y - 2.0f * margin.y});
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (chrome != nullptr) {
        ImGui::PushStyleColor(ImGuiCol_WindowBg, toImVec4(palette::transparent));
    }
    const bool open = ImGui::Begin(hubWindowName, nullptr, flags);
    if (chrome != nullptr) {
        ImGui::PopStyleColor();
        chrome->paintBarWindow();
    }
    if (open) {
        drawHeader();
        if (ImGui::BeginTable("##hub_columns", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Recent", ImGuiTableColumnFlags_WidthStretch, 3.0f);
            ImGui::TableSetupColumn("Start", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            drawRecentProjects();
            ImGui::TableSetColumnIndex(1);
            drawNewProject(assets);
            drawOpenExisting();
            drawMessage();
            ImGui::EndTable();
        }
    }
    ImGui::End();
    return std::exchange(m_chosen, std::nullopt);
}

void ProjectHub::drawHeader() const {
    widgets::CaptionText("ZENITH", palette::captionInk);
    ImGui::SameLine();
    ImGui::TextColored(toImVec4(palette::inkMuted), "Project Hub");
    ImGui::Separator();
}

void ProjectHub::drawRecentProjects() {
    widgets::SectionHeader("Recent Projects", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Leaf);
    if (m_recentProjects.entries().empty()) {
        ImGui::TextDisabled("No recent projects. Create one from the template or open an existing project.");
        return;
    }
    constexpr ImGuiTableFlags tableFlags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##recent_projects", 3, tableFlags)) {
        return;
    }
    ImGui::TableSetupColumn("Project", ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn("Last opened", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    for (const RecentProject& project : m_recentProjects.entries()) {
        drawRecentProjectRow(project);
    }
    ImGui::EndTable();

    if (m_pendingRemoval) {
        m_recentProjects.remove(*m_pendingRemoval);
        m_pendingRemoval.reset();
        if (const RecentProjectsResult saved = m_recentProjects.save(); !saved) {
            setMessage(saved.error, true);
        }
    }
}

void ProjectHub::drawRecentProjectRow(const RecentProject& project) {
    const bool missing = recentProjectMissing(project);
    const std::string path = utf8(project.projectFile);
    ImGui::PushID(path.c_str());
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    const std::string name = project.displayName.empty() ? utf8(project.projectFile.stem()) : project.displayName;
    ImGui::TextColored(toImVec4(missing ? palette::inkDisabled : palette::ink), "%s", name.c_str());
    if (missing) {
        ImGui::SameLine();
        ImGui::TextColored(toImVec4(palette::warning), "(missing)");
    }
    ImGui::TextDisabled("%s", path.c_str());

    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(formatLastOpened(project.lastOpened).c_str());

    ImGui::TableSetColumnIndex(2);
    ImGui::BeginDisabled(missing);
    if (widgets::Button("Open")) {
        m_chosen = project.projectFile;
    }
    ImGui::SameLine();
    if (widgets::Button("Open in IDE")) {
        EditorSettings settings(m_config.editorSettingsFile);
        if (!m_config.editorSettingsFile.empty()) {
            // A file that fails to load leaves the default command.
            static_cast<void>(settings.load());
        }
        if (const ChildProcessResult opened = openProjectInIde(settings.tools().ideCommand, project.projectFile);
            !opened) {
            setMessage("Could not open the project in the IDE: " + opened.error +
                           ". Set ide.command in the Build panel's settings.",
                       true);
        } else {
            setMessage("Opened " + utf8(projectIdeLocation(project.projectFile).file) + " in the IDE.", false);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (widgets::Button("Remove")) {
        m_pendingRemoval = project.projectFile;
    }
    ImGui::SetItemTooltip("Remove from this list; the project's files are kept");
    ImGui::PopID();
}

void ProjectHub::drawNewProject(AssetManager& assets) {
    widgets::SectionHeader("New from Template", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Leaf);
    if (m_config.templateDirectory.empty()) {
        ImGui::TextColored(toImVec4(palette::warning), "The C++ game template was not found.");
        return;
    }
    ImGui::TextDisabled("C++ game: a game module with an example component and system, built with CMake.");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##name", "Project name", &m_newProjectName);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##location", "Folder to create the project in", &m_newProjectLocation);
    const std::string identifier = projectIdentifier(m_newProjectName);
    if (!identifier.empty()) {
        ImGui::TextDisabled(
            "Creates %s",
            utf8(fromUtf8(m_newProjectLocation) / identifier / (identifier + projectFileExtension)).c_str());
    }
    if (widgets::Button("Create Project")) {
        const ProjectTemplateResult created =
            generateProjectFromTemplate({.displayName = m_newProjectName,
                                         .parentDirectory = fromUtf8(m_newProjectLocation),
                                         .templateDirectory = m_config.templateDirectory,
                                         .engineRoot = m_config.engineRoot,
                                         .engineAssetRoot = m_config.engineAssetRoot,
                                         .buildConfiguration = m_config.buildConfiguration},
                                        assets);
        if (!created) {
            setMessage(created.error, true);
        } else {
            setMessage("Created " + utf8(created.projectFile), false);
            m_chosen = created.projectFile;
        }
    }
}

void ProjectHub::drawOpenExisting() {
    widgets::SectionHeader("Open Existing", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Leaf);
    ImGui::BeginDisabled(m_openDialog.pending());
    if (widgets::Button("Open Project...")) {
        browseForProject();
    }
    ImGui::EndDisabled();
}

void ProjectHub::drawMessage() const {
    if (m_message.empty()) {
        return;
    }
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, toImVec4(m_messageIsError ? palette::error : palette::success));
    ImGui::TextWrapped("%s", m_message.c_str());
    ImGui::PopStyleColor();
}

void ProjectHub::browseForProject() {
    std::filesystem::path start = dialogFolder(m_config.defaultProjectLocation);
    if (!m_recentProjects.entries().empty()) {
        if (std::filesystem::path recent = dialogFolder(m_recentProjects.entries().front().projectFile.parent_path());
            !recent.empty()) {
            start = std::move(recent);
        }
    }
    if (!m_openDialog.request(FileDialogKind::Open, start)) {
        setMessage("A file dialog is already open.", true);
    }
}

void ProjectHub::pollOpenDialog() {
    const std::optional<FileDialogOutcome> outcome = m_openDialog.poll();
    if (!outcome) {
        return;
    }
    if (outcome->status == FileDialogStatus::Failed) {
        setMessage("Could not show the open dialog: " + outcome->error, true);
    } else if (outcome->status == FileDialogStatus::Chosen) {
        m_chosen = outcome->path;
    }
}

void ProjectHub::reportError(std::string message) {
    setMessage(std::move(message), true);
}

void ProjectHub::setMessage(std::string message, bool isError) {
    m_message = std::move(message);
    m_messageIsError = isError;
}

const std::string& ProjectHub::message() const {
    return m_message;
}

bool ProjectHub::messageIsError() const {
    return m_messageIsError;
}

const ProjectHubConfig& ProjectHub::config() const {
    return m_config;
}
