#include "ViewportDrop.hpp"

#include "editor/document/EditorDocument.hpp"
#include "editor/panels/AssetDragPayloads.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/viewport/Cursor3D.hpp"

#include <imgui.h>

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
