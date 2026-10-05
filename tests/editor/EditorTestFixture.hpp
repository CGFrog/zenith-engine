#pragma once

// The objects an editor session owns, without a window or GPU, shared by the
// editor tests that need an ApplicationContext or EditorContext. A new
// required member of either context is added here once, not in every test.

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/application/EditorPlayMode.hpp"
#include "editor/application/SceneEditing.hpp"
#include "editor/document/CommandHistory.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/interaction/ComponentEditorOverrides.hpp"
#include "editor/interaction/EntityEyedropper.hpp"
#include "editor/interaction/Selection.hpp"
#include "editor/menus/EditorMenus.hpp"
#include "editor/operators/ComponentOperators.hpp"
#include "editor/operators/DefaultKeymap.hpp"
#include "editor/operators/Keymap.hpp"
#include "editor/operators/KeymapDispatch.hpp"
#include "editor/operators/OperatorRegistry.hpp"
#include "editor/operators/OperatorRunner.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/panels/ViewportRegistry.hpp"
#include "editor/settings/ComponentInspectorLayout.hpp"
#include "engine/application/Application.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/audio/AudioSystem.hpp"
#include "engine/diagnostics/Logger.hpp"
#include "engine/physics/PhysicsWorld.hpp"
#include "engine/scene/Scene.hpp"
#include "engine/ui/GameUi.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// The session workflows a panel can trigger. Default-constructed, they are
// no-ops: no operator runs or is available. Connected to a registry and
// runner, operators run and poll as in the session, in the context the
// source builds for the invoking region. Tests needing other behaviour
// derive and override only what they need.
class TestSessionActions : public EditorSessionActions {
  public:
    // The context an operator invoked from region runs in.
    using ContextSource = std::function<OperatorContext(ApplicationContext&, KeymapRegion)>;

    TestSessionActions() = default;

    TestSessionActions(const OperatorRegistry& registry, OperatorRunner& runner, ContextSource context)
        : m_registry(&registry)
        , m_runner(&runner)
        , m_context(std::move(context)) {}

    virtual ~TestSessionActions() = default;

    TestSessionActions(const TestSessionActions&) = delete;
    TestSessionActions& operator=(const TestSessionActions&) = delete;

    OperatorResult invokeOperator(ApplicationContext& app, std::string_view operatorId, std::string_view properties,
                                  KeymapRegion region) override {
        if (m_runner == nullptr) {
            return OperatorResult::cancelled();
        }
        OperatorContext context = m_context(app, region);
        return m_runner->invokeWithJson(operatorId, context, properties);
    }

    bool operatorAvailable(ApplicationContext& app, std::string_view operatorId) override {
        const Operator* op = m_registry != nullptr ? m_registry->find(operatorId) : nullptr;
        return op != nullptr && op->canRun(m_context(app, KeymapRegion::Global));
    }

    std::string operatorShortcut(std::string_view, std::string_view) const override {
        return {};
    }

    void requestOpenScene(ApplicationContext&, std::filesystem::path) override {}

    void projectFilesMoved(ApplicationContext&, std::span<const ProjectFileMove>) override {}

    void projectFilesRemoved(ApplicationContext&, std::span<const std::filesystem::path>) override {}

    void handleObjectKeys(ApplicationContext&, KeymapRegion) override {}

    bool modalTransformActive() const override {
        return false;
    }

    void updateModalTransform(ApplicationContext&, ViewportId) override {}

  private:
    const OperatorRegistry* m_registry{nullptr};
    OperatorRunner* m_runner{nullptr};
    ContextSource m_context;
};

// Connected actions with the session's default keymap: menu shortcut labels
// and viewport mouse chords come from it (region keys stay unbound).
class KeymapSessionActions : public TestSessionActions {
  public:
    using TestSessionActions::TestSessionActions;

    std::string operatorShortcut(std::string_view operatorId, std::string_view properties) const override {
        return keymap.shortcutLabel(operatorId, properties);
    }

    uint32_t dispatchMouseChords(ApplicationContext& app, KeymapRegion region, uint32_t allowedButtons) override {
        return dispatcher.dispatchMouse(keymap, region, allowedButtons, [&](const KeymapInvocation& invocation) {
            invokeOperator(app, invocation.operatorId, invocation.properties, invocation.region);
        });
    }

    bool mouseButtonBound(KeymapRegion region, int button) const override {
        return dispatcher.mouseButtonBound(keymap, region, button);
    }

    Keymap keymap{defaultEditorKeymap()};
    KeymapDispatcher dispatcher;
};

struct EditorTestFixtureOptions {
    // The document's asset root.
    std::filesystem::path documentRoot{std::filesystem::temp_directory_path()};
    LoggerConfig logger{};
};

// Owns everything ApplicationContext and EditorContext reference. Operators
// registered in `operators` run through `runner`; panels reach them through
// actions(), which are connected TestSessionActions unless a test installs
// its own with useActions. Members a test adds that reference these (panels,
// module hosts) are destroyed first, as members of a derived fixture.
struct EditorTestFixture {
    explicit EditorTestFixture(EditorTestFixtureOptions options = {})
        : logger(options.logger)
        , document(std::move(options.documentRoot)) {}

    EditorTestFixture(const EditorTestFixture&) = delete;
    EditorTestFixture& operator=(const EditorTestFixture&) = delete;

    Logger logger;
    Scene scene;
    AssetManager assets{&logger};
    PhysicsWorld physics;
    AudioSystem audio;
    ApplicationControl control;
    RenderRuntime rendering;
    GameUiContext gameUi;
    ApplicationContext app{scene, assets, physics, audio, control, rendering, gameUi, logger};
    EditorDocument document;
    CommandHistory history;
    EditorDiagnostics diagnostics;
    Selection selection;
    MemoryClipboard clipboard;
    ComponentInspectorLayout componentLayout;
    EntityEyedropper eyedropper;
    EditorMenus menus;
    OperatorRegistry operators;
    OperatorRunner runner{operators, history};
    ViewportRegistry viewports;
    ComponentEditorOverrides componentEditors;
    // The mode panels and operators see, unless followedPlayMode is set.
    EditorMode mode{EditorMode::Edit};
    bool simulating{false};
    // When set (to a play mode the test owns), the mode, Simulate flag, and
    // edited scene follow it, as in the session.
    EditorPlayMode* followedPlayMode{nullptr};
    // What panels learn about the game module's behaviours; none by default.
    const EditorScriptInfo* scripts{nullptr};
    // The Properties panel's watch list; none by default.
    const WatchList* watches{nullptr};
    // The project's names inspectors offer; none by default.
    const EditorProjectNames* projectNames{nullptr};
    // The project's assets asset fields offer; none by default.
    ProjectAssets* projectAssets{nullptr};
    TestSessionActions defaultActions{operators, runner, sessionContextSource()};

    // The context the session runs an operator in: operatorContext() with
    // the invoking region and the application. Custom actions connect with
    // it.
    TestSessionActions::ContextSource sessionContextSource() {
        return [this](ApplicationContext& context, KeymapRegion region) {
            OperatorContext invocation = operatorContext();
            invocation.region = region;
            invocation.app = &context;
            return invocation;
        };
    }

    // Panels see custom actions (which must outlive the fixture's use of
    // them) instead of defaultActions.
    void useActions(EditorSessionActions& custom) {
        m_actions = &custom;
    }

    EditorSessionActions& actions() {
        return *m_actions;
    }

    EditorMode currentMode() const {
        return followedPlayMode != nullptr ? followedPlayMode->mode() : mode;
    }

    // As EditorSession::editing: live on the runtime scene outside Edit mode.
    SceneEditing editing() {
        Scene* editScene = followedPlayMode != nullptr ? followedPlayMode->editScene() : nullptr;
        return {scene, assets, document, history, diagnostics, editScene};
    }

    // A headless operator context (no application, Global region).
    OperatorContext operatorContext() {
        return {.editing = editing(), .selection = selection, .mode = currentMode()};
    }

    // Runs an operator headless through the runner, its defaults overridden
    // by a JSON object of property values (empty for none).
    OperatorResult invoke(std::string_view operatorId, std::string_view properties = {}) {
        OperatorContext context = operatorContext();
        return runner.invokeWithJson(operatorId, context, properties);
    }

    EditorContext context() {
        return context(currentMode());
    }

    EditorContext context(EditorMode contextMode) {
        return {.app = app,
                .editing = editing(),
                .selection = selection,
                .diagnostics = diagnostics,
                .mode = contextMode,
                .actions = actions(),
                .eyedropper = &eyedropper,
                .menus = menus,
                .simulating = followedPlayMode != nullptr ? followedPlayMode->simulating() : simulating,
                .componentEditors = &componentEditors,
                .scripts = scripts,
                .watches = watches,
                .projectNames = projectNames,
                .projectAssets = projectAssets};
    }

  private:
    EditorSessionActions* m_actions{&defaultActions};
};
