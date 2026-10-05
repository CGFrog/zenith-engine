#include "ViewportDrop.hpp"

#include "editor/application/FileDropQueue.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/panels/AssetDragPayloads.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/project/ProjectAssets.hpp"
#include "editor/viewport/Cursor3D.hpp"

#include <imgui.h>

#include <array>
#include <optional>
#include <vector>

namespace {
// The project file a dropped path stands for: itself when it is already a
// project asset, else its imported copy (nullopt when importing failed).
std::optional<std::filesystem::path> projectFileFor(ProjectAssets& project, const std::filesystem::path& dropped,
                                                    EditorDiagnostics& diagnostics) {
    const std::filesystem::path normalized = normalizedCatalogPath(dropped);
    if (project.catalog().findAsset(normalized) != nullptr) {
        return normalized;
    }
    const std::array<std::filesystem::path, 1> sources{dropped};
    std::vector<std::filesystem::path> imported =
        project.importFiles(sources, project.defaultImportFolder(), diagnostics);
    return imported.empty() ? std::nullopt : std::optional<std::filesystem::path>(imported.front());
}
} // namespace

void acceptViewportAssetDrops(EditorContext& context, const SnapView& view, glm::vec2 viewPoint) {
    if (!ImGui::BeginDragDropTarget()) {
        return;
    }
    const auto placementHere = [&] {
        const glm::vec3& cursor = context.editing.document().editorData().cursorLocation;
        return placementUnderMouse(context.app.scene, context.app.assets, view, viewPoint,
                                   placementDepth(view.lens, cursor));
    };
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(modelAssetPayload)) {
        const Placement placement = placementHere();
        context.placeModel(static_cast<const char*>(payload->Data), entt::null, &placement);
    }
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(prefabAssetPayload)) {
        const Placement placement = placementHere();
        context.placePrefab(static_cast<const char*>(payload->Data), entt::null, &placement);
    }
    ImGui::EndDragDropTarget();
}

void acceptViewportFileDrops(EditorContext& context, const SnapView& view, glm::vec2 imageMin, glm::vec2 imageSize) {
    if (context.fileDrops == nullptr || context.projectAssets == nullptr || context.fileDrops->empty()) {
        return;
    }
    const glm::vec2 imageMax = imageMin + imageSize;
    std::vector<DroppedFile> drops;
    for (const DroppedFile& drop : context.fileDrops->pending()) {
        if (drop.position.x >= imageMin.x && drop.position.y >= imageMin.y && drop.position.x < imageMax.x &&
            drop.position.y < imageMax.y) {
            drops.push_back(drop);
        }
    }
    context.fileDrops->claimWithin(imageMin, imageMax);
    const glm::vec3& cursor = context.editing.document().editorData().cursorLocation;
    for (const DroppedFile& drop : drops) {
        const std::optional<std::filesystem::path> file =
            projectFileFor(*context.projectAssets, drop.path, context.diagnostics);
        const std::optional<EditorAssetKind> kind = file ? editorAssetKind(*file) : std::nullopt;
        if (kind != EditorAssetKind::Model && kind != EditorAssetKind::Prefab) {
            continue;
        }
        const Placement placement = placementUnderMouse(context.app.scene, context.app.assets, view,
                                                        drop.position - imageMin, placementDepth(view.lens, cursor));
        if (kind == EditorAssetKind::Model) {
            context.placeModel(*file, entt::null, &placement);
        } else {
            context.placePrefab(*file, entt::null, &placement);
        }
    }
}
