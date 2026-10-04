// A project with a game module hot reloads it once. Every editor extension
// the module registered is in the editor's registries, and the reload
// releases and registers the same ones again (a release that missed one would
// make its re-registration fail). Without a module there is nothing to do.

#include "editor/modules/GameModuleHost.hpp"
#include "editor/smoke/EditorSmokeHooks.hpp"
#include "editor/smoke/EditorSmokeSteps.hpp"

namespace {

std::string extensionsProblem(const GameModuleHost& host) {
    std::string problem;
    for (const std::string& missing : host.missingExtensions()) {
        problem += (problem.empty() ? "missing " : ", ") + missing;
    }
    return problem;
}

class ModuleReloadSmokeStep final : public SmokeStep {
  public:
    std::string_view name() const override {
        return "game module reload";
    }

    SmokeStepResult update(SmokeStepContext& context, uint32_t stepFrame) override {
        EditorSmokeHooks& editor = context.editor;
        const GameModuleHost& host = editor.moduleHost();
        if (host.sourcePath().empty()) {
            return SmokeStepResult::done();
        }
        if (stepFrame == 0) {
            m_extensions = host.extensionCounts();
            if (const std::string problem = extensionsProblem(host); !problem.empty()) {
                return SmokeStepResult::failed("before the reload, " + problem);
            }
            editor.requestModuleReload();
            return SmokeStepResult::running();
        }
        if (editor.moduleReloadPending()) {
            return SmokeStepResult::running();
        }
        if (host.status() != GameModuleStatus::Loaded || host.loadCount() < 2) {
            return SmokeStepResult::failed("the reload failed: " + host.error());
        }
        if (host.extensionCounts() != m_extensions) {
            return SmokeStepResult::failed(
                "the reloaded module's editor extensions differ from those before the reload");
        }
        if (const std::string problem = extensionsProblem(host); !problem.empty()) {
            return SmokeStepResult::failed("after the reload, " + problem);
        }
        context.app.logger.log(LogLevel::Info, "editor", "Game module smoke: reloaded with the same editor extensions");
        return SmokeStepResult::done();
    }

  private:
    // The module's editor extensions before the reload.
    GameModuleHost::ExtensionCounts m_extensions;
};

} // namespace

std::unique_ptr<SmokeStep> makeModuleReloadSmokeStep() {
    return std::make_unique<ModuleReloadSmokeStep>();
}
