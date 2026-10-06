// EditorSession's side of the editor smoke (ZENITH_EDITOR_SMOKE): the hooks
// the smoke steps use, and running the scenario once per frame. The steps and
// their order live in editor/smoke/.

#include "EditorSession.hpp"

#include "editor/areas/AreaWindows.hpp"
#include "editor/smoke/EditorSmokeHooks.hpp"
#include "editor/smoke/EditorSmokeScenario.hpp"

#include <iostream>
#include <limits>

class EditorSession::SmokeHooks final : public EditorSmokeHooks {
  public:
    SmokeHooks(EditorSession& session, ApplicationContext& context)
        : m_session(session)
        , m_context(context) {}

    OperatorResult invokeOperator(std::string_view operatorId, std::string_view properties,
                                  KeymapRegion region) override {
        return m_session.invokeOperator(m_context, operatorId, properties, region);
    }

    OperatorContext operatorContext(KeymapRegion region) override {
        return m_session.levelOperatorContext(m_context, region);
    }

    SceneEditing editing() override {
        return m_session.editing(m_context);
    }

    void undo() override {
        OperatorContext level = m_session.levelOperatorContext(m_context, KeymapRegion::Global);
        m_session.undo(level);
    }

    void undoSince(CommandHistoryMarker marker) override {
        m_session.undoSince(m_context, marker);
    }

    void enterPlayMode() override {
        m_session.m_playback.enter(m_context);
    }

    void stopPlayMode() override {
        m_session.m_playback.stop(m_context);
    }

    const CommandHistory& history() const override {
        return m_session.m_history;
    }

    Selection& selection() override {
        return m_session.m_selection;
    }

    EditorPlayMode& playMode() override {
        return m_session.m_playMode;
    }

    EditorDocument& document() override {
        return m_session.m_document;
    }

    const PrefabWindowSet& prefabWindows() const override {
        return m_session.m_prefabWindows.windows();
    }

    ViewportPanel* prefabWindowViewport(uint64_t window) override {
        return m_session.m_prefabWindows.viewport(window);
    }

    std::optional<SceneEditing> prefabWindowEditing(uint64_t window) override {
        PrefabWindowDocument* document = m_session.m_prefabWindows.windows().find(window);
        if (document == nullptr) {
            return std::nullopt;
        }
        return SceneEditing(document->scene, m_context.assets, document->document, document->history,
                            m_session.m_diagnostics);
    }

    bool savePrefabWindow(uint64_t window) override {
        PrefabWindowDocument* document = m_session.m_prefabWindows.windows().find(window);
        return document != nullptr && m_session.m_prefabWindows.save(m_context, *document);
    }

    void closePrefabWindow(uint64_t window) override {
        m_session.m_prefabWindows.requestClose(window);
    }

    EditorMenus& menus() override {
        return m_session.m_menus;
    }

    ViewportRegistry& viewports() override {
        return m_session.m_viewports;
    }

    Workspace& activeWorkspace() override {
        return m_session.m_workspaces.active();
    }

    void restoreWorkspaceLayout(const std::string& layout) override {
        restoreImGuiLayout(layout);
        m_session.m_workspaces.active().setLayout(layout);
        m_session.m_workspacesDirty = true;
    }

    const InfoLog& infoLog() const override {
        return m_session.m_infoPanel.log();
    }

    const RenderSettings& requestedRenderSettings() const override {
        return m_session.m_renderSettings.settings().requested();
    }

    const LastOperator* lastOperator() const override {
        return m_session.m_operatorRunner.lastOperator();
    }

    const GameModuleHost& moduleHost() const override {
        return m_session.m_moduleHost;
    }

    void requestModuleReload() override {
        m_session.m_moduleReloadPending = true;
    }

    bool moduleReloadPending() const override {
        return m_session.m_moduleReloadPending;
    }

    ViewportPanel* gameViewport() override {
        return m_session.m_playback.gameViewport();
    }

    bool gameReceivesInput() const override {
        const InputState frameInput;
        return &m_session.m_playback.gameInput(frameInput) == &frameInput;
    }

    MouseCapturePolicy mouseCapturePolicy() const override {
        return m_session.m_playback.mouseCapturePolicy();
    }

    bool playTargetActive() const override {
        return m_session.m_playback.playActive() || m_session.m_playback.activeTarget().has_value();
    }

    const StandalonePlayer& standalone() const override {
        return m_session.m_playback.standalone();
    }

    const std::filesystem::path& gameExecutable() const override {
        return m_session.m_playback.gameExecutable();
    }

  private:
    EditorSession& m_session;
    ApplicationContext& m_context;
};

SceneResult EditorSession::prepareSmokeScenario(ApplicationContext& context) {
    EditorSmokeScenario scenario = buildEditorSmokeScenario(context, m_document.options(), m_smokeScratchDirectory);
    if (!scenario.runner) {
        return {.error = std::move(scenario.error)};
    }
    m_smokeRunner = std::move(scenario.runner);
    return {.success = true};
}

void EditorSession::updateSmokeScenario(ApplicationContext& context) {
    SmokeHooks hooks(*this, context);
    SmokeStepContext stepContext{.app = context, .editor = hooks};
    const SmokeRunResult result = m_smokeRunner->update(stepContext);
    if (result.status == SmokeRunStatus::Running) {
        return;
    }
    // The run is over either way; its steps (and the files they wrote) go.
    const size_t steps = m_smokeRunner->stepCount();
    m_smokeRunner.reset();
    if (result.status == SmokeRunStatus::Passed) {
        std::cout << "Editor smoke passed: " << steps << " steps\n";
        context.control.requestQuit();
        return;
    }
    const std::string error = "Smoke step '" + result.step + "' failed: " + result.error;
    // The log reaches the console too.
    context.logger.log(LogLevel::Error, "editor", error);
    m_diagnostics.reportError(error);
    // No scene has this id, so the engine stops with an error.
    context.control.requestSceneChange({std::numeric_limits<uint32_t>::max()});
}
