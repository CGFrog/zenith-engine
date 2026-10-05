#include "DebugLabelOverlay.hpp"

#include "engine/ui/GameUi.hpp"

#include <algorithm>
#include <cmath>

namespace {
// Above the game's own UI, which draws at small orders.
constexpr int32_t labelDrawOrder = 10000;
// Text height at the UI's design resolution (GameUiContext::scale).
constexpr float labelDesignSize = 18.0f;
} // namespace

void drawDebugLabels(const RenderRuntime& rendering, RenderViewId view, GameUiContext& ui) {
    const std::span<const ProjectedDebugLabel> labels = rendering.viewDebugLabels(view);
    if (labels.empty()) {
        return;
    }
    // A view rendered straight to the window has the UI's size (and reports
    // none of its own); an offscreen one is stretched over the window.
    const glm::vec2 uiSize(ui.viewportSize());
    const glm::vec2 viewSize(static_cast<float>(rendering.viewWidth(view)),
                             static_cast<float>(rendering.viewHeight(view)));
    const glm::vec2 scale = viewSize.x > 0.0f && viewSize.y > 0.0f ? uiSize / viewSize : glm::vec2(1.0f);
    const float size = std::max(labelDesignSize * ui.scale(), 10.0f);
    const float shadowOffset = std::max(std::round(size / 12.0f), 1.0f);
    for (const ProjectedDebugLabel& label : labels) {
        // Beside the anchor and vertically centred on it, as in the editor.
        const glm::vec2 anchor = label.position * scale;
        const glm::vec2 position{std::floor(anchor.x + size / 6.0f), std::floor(anchor.y - size * 0.5f)};
        ui.pixelText(position + glm::vec2(shadowOffset), label.text, size, {0.0f, 0.0f, 0.0f, label.color.a * 0.85f},
                     labelDrawOrder);
        ui.pixelText(position, label.text, size, label.color, labelDrawOrder + 1);
    }
}
