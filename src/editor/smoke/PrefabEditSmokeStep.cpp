// Prefab mode in the running editor: a prefab written to a hidden folder of
// the project's asset root (which the asset catalog and database skip, and
// the step removes again) is placed in the scene and opened with prefab.edit;
// its root is edited and saved with file.save, prefab.exit_edit returns to
// the scene, whose instance must show the edit; undo must put back both the
// file and the instance.

#include "editor/application/DocumentStack.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/document/PrefabAuthoring.hpp"
#include "editor/document/PrefabWriteCommand.hpp"
#include "editor/interaction/Selection.hpp"
#include "editor/smoke/EditorSmokeHooks.hpp"
#include "editor/smoke/EditorSmokeSteps.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <system_error>

namespace {

constexpr const char* smokeFolder = ".zenith-smoke-prefab-edit";
constexpr float authoredIntensity = 3.0f;
constexpr float editedIntensity = 7.0f;

class PrefabEditSmokeStep final : public SmokeStep {
  public:
    PrefabEditSmokeStep() = default;
    PrefabEditSmokeStep(const PrefabEditSmokeStep&) = delete;
    PrefabEditSmokeStep& operator=(const PrefabEditSmokeStep&) = delete;

    ~PrefabEditSmokeStep() override {
        if (!m_folder.empty()) {
            std::error_code error;
            std::filesystem::remove_all(m_folder, error);
        }
    }

    std::string_view name() const override {
        return "prefab edit mode";
    }

    SmokeStepResult update(SmokeStepContext& context, uint32_t frame) override {
        switch (frame) {
        case 0:
            return open(context);
        case 1:
            return editAndSave(context);
        case 2:
            return goBack(context);
        default:
            return undo(context);
        }
    }

  private:
    // Writes the prefab, places it, and opens it as its own document.
    SmokeStepResult open(SmokeStepContext& context) {
        const SceneDocumentOptions& options = context.editor.document().options();
        m_folder = options.assetRoot / smokeFolder;
        std::error_code error;
        std::filesystem::remove_all(m_folder, error);
        m_reference = std::string(smokeFolder) + "/crate.prefab";
        PrefabAuthoring authoring(context.app.assets, options);
        Scene staging;
        const Entity crate = staging.createEntity("Smoke Crate");
        staging.registry().emplace<PointLightComponent>(crate, glm::vec3(1.0f), authoredIntensity, 2.0f);
        staging.setParent(staging.createEntity("Smoke Lid"), crate);
        if (PrefabResult created = authoring.create(staging, crate, m_reference); !created) {
            return SmokeStepResult::failed("the smoke prefab was not written: " + created.error);
        }
        if (std::string readError; !readPrefabFileBytes(m_folder / "crate.prefab", m_original, readError)) {
            return SmokeStepResult::failed(readError);
        }
        m_marker = context.editor.history().marker();
        const std::string asset = nlohmann::json{{"asset", m_reference}}.dump();
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
                OperatorStatus::Finished ||
            context.editor.documents().depth() != 1) {
            return SmokeStepResult::failed("prefab.edit did not open the prefab");
        }
        if (context.app.scene.findEntity(context.editor.documents().activePrefab()->root) == entt::null ||
            context.app.scene.registry().view<const SceneEntityIdComponent>().size() != 2) {
            return SmokeStepResult::failed("the active scene should hold only the opened prefab");
        }
        return SmokeStepResult::running();
    }

    SmokeStepResult editAndSave(SmokeStepContext& context) {
        const PrefabEditTarget* prefab = context.editor.documents().activePrefab();
        if (prefab == nullptr) {
            return SmokeStepResult::failed("prefab mode ended on its own");
        }
        const SceneEntityId root = prefab->root;
        context.editor.editing().perform("Brighten Crate", {}, [&context, root] {
            Scene& scene = context.app.scene;
            scene.registry().get<PointLightComponent>(scene.findEntity(root)).intensity = editedIntensity;
            return SceneResult{.success = true};
        });
        if (!context.editor.document().dirty()) {
            return SmokeStepResult::failed("an edit in prefab mode did not make the prefab dirty");
        }
        if (context.editor.invokeOperator("file.save", {}, KeymapRegion::Global).status != OperatorStatus::Finished ||
            context.editor.document().dirty()) {
            return SmokeStepResult::failed("file.save did not save the prefab");
        }
        std::optional<std::string> saved;
        if (std::string readError;
            !readPrefabFileBytes(m_folder / "crate.prefab", saved, readError) || saved == m_original) {
            return SmokeStepResult::failed("saving did not change the prefab file");
        }
        return SmokeStepResult::running();
    }

    SmokeStepResult goBack(SmokeStepContext& context) {
        if (context.editor.invokeOperator("prefab.exit_edit", {}, KeymapRegion::Global).status !=
                OperatorStatus::Finished ||
            !context.editor.documents().empty()) {
            return SmokeStepResult::failed("prefab.exit_edit did not return to the scene");
        }
        if (intensity(context) != editedIntensity) {
            return SmokeStepResult::failed("the scene's instance did not take the saved edit");
        }
        if (context.editor.history().stepsSince(m_marker) != 2) {
            return SmokeStepResult::failed("the scene should hold the instantiate and one Edit step");
        }
        return SmokeStepResult::running();
    }

    SmokeStepResult undo(SmokeStepContext& context) {
        context.editor.undo();
        std::optional<std::string> restored;
        std::string readError;
        if (!readPrefabFileBytes(m_folder / "crate.prefab", restored, readError) || restored != m_original) {
            return SmokeStepResult::failed("undo did not restore the prefab file");
        }
        if (intensity(context) != authoredIntensity) {
            return SmokeStepResult::failed("undo did not restore the instance");
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
    std::string m_reference;
    std::optional<std::string> m_original;
    SceneEntityId m_instance;
    CommandHistoryMarker m_marker;
};

} // namespace

std::unique_ptr<SmokeStep> makePrefabEditSmokeStep() {
    return std::make_unique<PrefabEditSmokeStep>();
}
