// Prefab windows in the running editor: a prefab written to a hidden folder of
// the project's asset root (which the step removes again) is placed in the
// level and opened with prefab.edit in a window of its own beside it. The
// window's viewport must draw the window's own scene through the renderer
// (its render view is bound to the window's render scene and renders the
// prefab's objects) while the level keeps rendering. The prefab's root is then
// edited in the window and saved: the file changes, the level's instance takes
// the edit as a step of the level's own, and undo in the level puts the
// instance back. The window then closes again.

#include "editor/application/PrefabWindowSet.hpp"
#include "editor/document/DefaultScene.hpp"
#include "editor/document/PrefabAuthoring.hpp"
#include "editor/document/PrefabWriteCommand.hpp"
#include "editor/interaction/Selection.hpp"
#include "editor/panels/ViewportPanel.hpp"
#include "editor/smoke/EditorSmokeHooks.hpp"
#include "editor/smoke/EditorSmokeSteps.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <system_error>

namespace {

constexpr const char* smokeFolder = ".zenith-smoke-prefab-window";
// Frames the window's viewport may take to render its first frame (the view
// is acquired on its first draw and renders on the frame after).
constexpr uint32_t renderDeadline = 30;
constexpr float authoredIntensity = 3.0f;
constexpr float editedIntensity = 7.0f;

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
        if (!m_saved) {
            return editAndSave(context);
        }
        return closed(context);
    }

  private:
    // Writes the prefab, places it in the level, and opens it in a window.
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
        staging.registry().emplace<PointLightComponent>(lantern, glm::vec3(1.0f), authoredIntensity, 2.0f);
        staging.setParent(staging.createEntity("Smoke Glass"), lantern);
        if (PrefabResult created = authoring.create(staging, lantern, reference); !created) {
            return SmokeStepResult::failed("the smoke prefab was not written: " + created.error);
        }
        if (std::string readError; !readPrefabFileBytes(m_folder / "lantern.prefab", m_original, readError)) {
            return SmokeStepResult::failed(readError);
        }
        m_marker = context.editor.history().marker();
        const std::string asset = nlohmann::json{{"asset", reference}}.dump();
        if (context.editor.invokeOperator("prefab.instantiate", asset, KeymapRegion::Global).status !=
            OperatorStatus::Finished) {
            return SmokeStepResult::failed("prefab.instantiate did not place the smoke prefab");
        }
        const std::optional<SceneEntityId> placed = context.editor.selection().active();
        if (!placed) {
            return SmokeStepResult::failed("prefab.instantiate did not select the instance");
        }
        m_instance = *placed;
        if (context.editor.invokeOperator("prefab.edit", asset, KeymapRegion::Global).status !=
            OperatorStatus::Finished) {
            return SmokeStepResult::failed("prefab.edit did not open a prefab window");
        }
        const PrefabWindowDocument* window = context.editor.prefabWindows().find(reference);
        if (window == nullptr || context.app.scene.findEntity(m_instance) == entt::null) {
            return SmokeStepResult::failed("the prefab should open in a window beside the level");
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
                return SmokeStepResult::running();
            }
        }
        if (frame >= renderDeadline) {
            return SmokeStepResult::failed("the prefab window's viewport did not render the prefab within " +
                                           std::to_string(renderDeadline) + " frames");
        }
        return SmokeStepResult::running();
    }

    // Brightens the prefab's root in the window and saves the window.
    SmokeStepResult editAndSave(SmokeStepContext& context) {
        PrefabWindowDocument* window = context.editor.prefabWindows().find(m_window);
        std::optional<SceneEditing> editing = context.editor.prefabWindowEditing(m_window);
        if (window == nullptr || !editing) {
            return SmokeStepResult::failed("the prefab window closed on its own");
        }
        const SceneEntityId root = window->target.root;
        const size_t levelSteps = context.editor.history().stepsSince(m_marker);
        editing->perform("Brighten Lantern", {}, [window, root] {
            Scene& scene = window->scene;
            scene.registry().get<PointLightComponent>(scene.findEntity(root)).intensity = editedIntensity;
            return SceneResult{.success = true};
        });
        if (!window->document.dirty() || intensity(context) != authoredIntensity) {
            return SmokeStepResult::failed("an edit in the window should dirty the prefab and leave the level alone");
        }
        if (!context.editor.savePrefabWindow(m_window) || window->document.dirty()) {
            return SmokeStepResult::failed("saving the window did not save the prefab");
        }
        std::optional<std::string> saved;
        if (std::string readError;
            !readPrefabFileBytes(m_folder / "lantern.prefab", saved, readError) || saved == m_original) {
            return SmokeStepResult::failed("saving did not change the prefab file");
        }
        if (intensity(context) != editedIntensity || context.editor.history().stepsSince(m_marker) != levelSteps + 1) {
            return SmokeStepResult::failed("the level's instance should take the saved edit as one step of its own");
        }
        context.editor.undo();
        if (intensity(context) != authoredIntensity) {
            return SmokeStepResult::failed("undo in the level did not put its instance back");
        }
        m_saved = true;
        context.editor.closePrefabWindow(m_window);
        return SmokeStepResult::running();
    }

    SmokeStepResult closed(SmokeStepContext& context) {
        if (context.editor.prefabWindows().find(m_window) != nullptr) {
            return SmokeStepResult::failed("the prefab window did not close");
        }
        context.editor.undoSince(m_marker);
        return SmokeStepResult::done();
    }

    float intensity(SmokeStepContext& context) const {
        const Scene& scene = context.app.scene;
        const Entity instance = scene.findEntity(m_instance);
        const auto* light = instance == entt::null ? nullptr : scene.registry().try_get<PointLightComponent>(instance);
        return light == nullptr ? -1.0f : light->intensity;
    }

    std::filesystem::path m_folder;
    std::optional<std::string> m_original;
    SceneEntityId m_instance;
    CommandHistoryMarker m_marker;
    uint64_t m_window{0};
    bool m_rendered{false};
    bool m_saved{false};
};

} // namespace

std::unique_ptr<SmokeStep> makePrefabWindowSmokeStep() {
    return std::make_unique<PrefabWindowSmokeStep>();
}
