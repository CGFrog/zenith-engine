#pragma once

#include "editor/panels/EditorPanel.hpp"
#include "engine/graphics/Rendering.hpp"
#include "engine/scene/Scene.hpp"

#include <glm/vec2.hpp>

#include <string>
#include <vector>

enum class GizmoMode {
    Select,
    Translate,
    Rotate,
    Scale,
};

enum class TransformSpace {
    Global,
    Local,
};

// Transform tool choices shared by the viewport gizmo and modal G/R/S.
struct TransformToolSettings {
    // Blender starts with the selection tool, which shows no gizmo.
    GizmoMode gizmo{GizmoMode::Select};
    TransformSpace space{TransformSpace::Global};
    bool snapping{false};
    float translationSnap{1.0f};
    float rotationSnapDegrees{5.0f};
    float scaleSnap{0.1f};

    // The increment for the current gizmo mode; Select uses the scale value.
    float& activeSnap();
};

// The 3D viewport: shows the renderer's offscreen image, drives the editor
// camera, and hosts the transform toolbar, gizmo, picking, and drop target.
class ViewportPanel final : public EditorPanel {
  public:
    const char* title() const override;
    int windowFlags() const override;
    void update(EditorContext& context, const InputState& input, float deltaSeconds) override;
    void draw(EditorContext& context) override;
    void hidden(EditorContext& context) override;

    TransformToolSettings& tools();
    const TransformToolSettings& tools() const;
    // Screen-space rectangle of the viewport image from the latest draw.
    glm::vec2 position() const;
    glm::vec2 size() const;
    // A right-click that cancels a modal transform must not also start fly
    // navigation, so camera input waits until the button is released.
    void suppressCameraUntilRightRelease();

  private:
    void updateSelectionOutline(EditorContext& context);
    // Returns whether the toolbar is under the mouse.
    bool drawToolbar();
    // Returns whether the gizmo is in use or under the mouse.
    bool drawGizmo(EditorContext& context);
    void pickAtMouse(EditorContext& context);
    void drawActiveName(EditorContext& context) const;
    void setInputActive(EditorContext& context, bool active);

    TransformToolSettings m_tools;
    bool m_showGrid{true};
    bool m_inputActive{false};
    bool m_navigating{false};
    bool m_suppressCameraUntilRelease{false};
    glm::vec2 m_position{0.0f};
    glm::vec2 m_size{1.0f};
    bool m_gizmoWasUsing{false};
    // Scene snapshot from before the current gizmo drag, for undo.
    std::string m_gizmoBefore;
    std::vector<RenderObjectId> m_outlinedObjects;
    std::vector<Entity> m_outlineTraversal;
};
