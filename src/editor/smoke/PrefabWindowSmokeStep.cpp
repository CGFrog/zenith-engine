// Prefab windows in the running editor: a prefab written to a hidden folder of
// the project's asset root (which the step removes again) opens with
// prefab.edit in a window of its own beside the level. The window's viewport
// must draw the window's own scene through the renderer (its render view is
// bound to the window's render scene and renders the prefab's objects) while
// the level keeps rendering, and the window then closes again.

#include "editor/application/DocumentStack.hpp"
#include "editor/application/PrefabWindowSet.hpp"
#include "editor/document/DefaultScene.hpp"
#include "editor/document/PrefabAuthoring.hpp"
#include "editor/panels/ViewportPanel.hpp"
#include "editor/smoke/EditorSmokeHooks.hpp"
#include "editor/smoke/EditorSmokeSteps.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include <nlohmann/json.hpp>

#include <system_error>

namespace {

constexpr const char* smokeFolder = ".zenith-smoke-prefab-window";
// Frames the window's viewport may take to render its first frame (the view
// is acquired on its first draw and renders on the frame after).
constexpr uint32_t renderDeadline = 30;

class PrefabWindowSmokeStep final : public SmokeStep {
  public:
    PrefabWindowSmokeStep() = default;
    PrefabWindowSmokeStep(const PrefabWindowSmokeStep&) = delete;
    PrefabWindowSmokeStep& operator=(const PrefabWindowSmokeStep&) = delete;

    ~PrefabWindowSmokeStep() override {
        if (!m_folder.empty()) {
            std::error_code error;
            std::filesystem::remove_all(m_folder, error);
        }
    }

    std::string_view name() const override {
        return "prefab windows";
    }

    SmokeStepResult update(SmokeStepContext& context, uint32_t frame) override {
        if (frame == 0) {
            return open(context);
        }
        if (!m_rendered) {
            return waitForRender(context, frame);
        }
        return closed(context);
    }

  private:
    SmokeStepResult open(SmokeStepContext& context) {
        const SceneDocumentOptions& options = context.editor.document().options();
        m_folder = options.assetRoot / smokeFolder;
        std::error_code error;
        std::filesystem::remove_all(m_folder, error);
        const std::string reference = std::string(smokeFolder) + "/lantern.prefab";
        PrefabAuthoring authoring(context.app.assets, options);
        Scene staging;
        // A mesh, so the window's viewport has something to draw.
        Entity lantern = entt::null;
        if (SceneResult cube = addCube(staging, context.app.assets, options.assetRoot, lantern); !cube) {
            return SmokeStepResult::failed(cube.error);
        }
        staging.setParent(staging.createEntity("Smoke Glass"), lantern);
        if (PrefabResult created = authoring.create(staging, lantern, reference); !created) {
            return SmokeStepResult::failed("the smoke prefab was not written: " + created.error);
        }
        m_windowsWereEnabled = context.editor.prefabWindowsEnabled();
        context.editor.setPrefabWindowsEnabled(true);
        const std::string asset = nlohmann::json{{"asset", reference}}.dump();
        if (context.editor.invokeOperator("prefab.edit", asset, KeymapRegion::Global).status !=
            OperatorStatus::Finished) {
            return SmokeStepResult::failed("prefab.edit did not open a prefab window");
        }
        const PrefabWindowDocument* window = context.editor.prefabWindows().find(reference);
        if (window == nullptr || !context.editor.documents().empty()) {
            return SmokeStepResult::failed("the prefab should open in a window, not in prefab mode");
        }
        m_window = window->id;
        return SmokeStepResult::running();
    }

    SmokeStepResult waitForRender(SmokeStepContext& context, uint32_t frame) {
        const PrefabWindowDocument* window = context.editor.prefabWindows().find(m_window);
        const ViewportPanel* viewport = context.editor.prefabWindowViewport(m_window);
        if (window == nullptr || viewport == nullptr) {
            return SmokeStepResult::failed("the prefab window or its viewport closed on its own");
        }
        const RenderRuntime& rendering = context.app.rendering;
        const RenderViewId view = viewport->renderView();
        if (view && view != rendering.defaultView() && rendering.viewScene(view) == window->renderScene) {
            const RenderViewStats stats = rendering.viewStats(view);
            if (stats.rendered && stats.visibleCount > 0 && rendering.viewTexture(view)) {
                m_rendered = true;
                context.editor.closePrefabWindow(m_window);
                return SmokeStepResult::running();
            }
        }
        if (frame >= renderDeadline) {
            return SmokeStepResult::failed("the prefab window's viewport did not render the prefab within " +
                                           std::to_string(renderDeadline) + " frames");
        }
        return SmokeStepResult::running();
    }

    SmokeStepResult closed(SmokeStepContext& context) {
        if (context.editor.prefabWindows().find(m_window) != nullptr) {
            return SmokeStepResult::failed("the prefab window did not close");
        }
        context.editor.setPrefabWindowsEnabled(m_windowsWereEnabled);
        return SmokeStepResult::done();
    }

    std::filesystem::path m_folder;
    uint64_t m_window{0};
    bool m_rendered{false};
    bool m_windowsWereEnabled{true};
};

} // namespace

std::unique_ptr<SmokeStep> makePrefabWindowSmokeStep() {
    return std::make_unique<PrefabWindowSmokeStep>();
}
