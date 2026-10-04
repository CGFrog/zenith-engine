// EditorSession's preferences and packaging workflows: the Preferences window
// and its live application of the look, and Build and Package.

#include "EditorSession.hpp"

#include "editor/operators/PackageOperators.hpp"
#include "editor/operators/PreferencesOperators.hpp"
#include "editor/project/ProjectFileOperations.hpp"
#include "editor/theme/EditorTheme.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <system_error>

namespace {
constexpr const char* packageDiagnosticSource = "Package";

std::string_view platformKey(PackagePlatform platform) {
    switch (platform) {
    case PackagePlatform::Windows:
        return "windows";
    case PackagePlatform::Linux:
        return "linux";
    case PackagePlatform::MacOS:
        return "macos";
    }
    return "windows";
}
} // namespace

void EditorSession::registerPreferenceOperators() {
    if (const OperatorRegistrationResult result = registerPreferencesOperators(m_operators, m_preferences); !result) {
        m_diagnostics.reportError(result.error);
    }
    if (const OperatorRegistrationResult result =
            registerPackageOperators(m_operators, {.runner = m_packageRunner,
                                                   .tools = m_settings.tools(),
                                                   .projectFile = m_renderSettings.settings().projectFile()});
        !result) {
        m_diagnostics.reportError(result.error);
    }
}

void EditorSession::applyAppearance() {
    const EditorAppearanceResult applied = m_appearance.apply(m_settings.appearance());
    if (!applied) {
        m_diagnostics.reportError(applied.error);
    }
    for (const std::string& warning : applied.warnings) {
        m_diagnostics.add(EditorDiagnosticSeverity::Warning, "Theme", warning);
    }
    m_themeChrome.setParameters(m_appearance.theme());
}

PreferencesContext EditorSession::preferencesContext(ApplicationContext& context) {
    std::error_code error;
    return {
        .keymapPath = m_keymapPath,
        .displayContentScale = m_appearance.displayContentScale(),
        .openKeymapEditor = [this, &context] { invokeOperator(context, "wm.keymap_editor", {}, KeymapRegion::Global); },
        .ideTestFile = m_settings.path(),
        .workingDirectory = std::filesystem::current_path(error)};
}

void EditorSession::updatePackaging() {
    if (!m_packageRunner.poll().finished) {
        return;
    }
    std::optional<std::filesystem::path> archive;
    if (m_packageRunner.state() == PackageState::Succeeded) {
        const std::filesystem::path& output = m_packageRunner.plan().buildDirectory;
        archive = newestPackageArchive(output);
        if (const ProjectFileResult revealed = revealInFileBrowser(archive ? *archive : output); !revealed) {
            m_diagnostics.add(EditorDiagnosticSeverity::Warning, packageDiagnosticSource, revealed.error);
        }
    }
    m_diagnostics.replaceSource(packageDiagnosticSource, packageDiagnostics(m_packageRunner, archive));
}

void EditorSession::drawBuildPackageMenu(ApplicationContext& context) {
    if (!ImGui::BeginMenu("Build and Package")) {
        return;
    }
    for (const PackagePlatform platform : {PackagePlatform::Windows, PackagePlatform::Linux, PackagePlatform::MacOS}) {
        const std::string label(packagePlatformLabel(platform));
        const PackagePlatformSupport support = packagePlatformSupport(platform);
        if (!support.available) {
            ImGui::MenuItem(label.c_str(), nullptr, false, false);
            ImGui::SetItemTooltip("%s", support.reason.c_str());
            continue;
        }
        for (const PackageConfiguration configuration :
             {PackageConfiguration::Release, PackageConfiguration::RelWithDebInfo}) {
            const std::string itemLabel =
                label + (configuration == PackageConfiguration::Release ? "" : " (Release with Debug Info)");
            const std::string properties =
                nlohmann::json{
                    {"platform", platformKey(platform)},
                    {"configuration", configuration == PackageConfiguration::Release ? "release" : "relwithdebinfo"}}
                    .dump();
            operatorMenuItem(context, "file.build_package", itemLabel.c_str(), properties);
        }
    }
    ImGui::EndMenu();
}
