#include "Rendering.hpp"

#include <algorithm>

RenderSettings composeViewSettings(const RenderSettings& frameSettings,
                                   const std::optional<RenderSettings>& viewSettings) {
    if (!viewSettings) {
        return frameSettings;
    }
    RenderSettings composed = *viewSettings;
    // Work recorded once per frame for every view.
    composed.shadows = frameSettings.shadows;
    composed.cascadeSplitLambda = frameSettings.cascadeSplitLambda;
    composed.environment = frameSettings.environment;
    composed.reflectionProbes = frameSettings.reflectionProbes;
    composed.reflectionProbeBlending = frameSettings.reflectionProbeBlending;
    composed.indirectDiffuse = frameSettings.indirectDiffuse;
    composed.localLights = frameSettings.localLights;
    composed.maximumLocalLights = frameSettings.maximumLocalLights;
    composed.localShadows = frameSettings.localShadows;
    composed.particles = frameSettings.particles;
    composed.preferGpuDrivenSubmission = frameSettings.preferGpuDrivenSubmission;
    composed.textureStreamingBytesPerFrame = frameSettings.textureStreamingBytesPerFrame;
    composed.deferredUploadWarningFrames = frameSettings.deferredUploadWarningFrames;
    composed.budgets = frameSettings.budgets;
    return composed;
}

RenderRuntime::RenderRuntime()
    : m_views(1)
    , m_activeView{.index = 0, .generation = 1} {
    m_views.front().generation = 1;
    m_views.front().alive = true;
}

RenderViewId RenderRuntime::createView() {
    uint32_t index = 1;
    while (index < m_views.size() && m_views[index].alive) {
        ++index;
    }
    if (index >= maximumViews) {
        return {};
    }
    if (index == m_views.size()) {
        m_views.emplace_back();
    }
    ViewSlot& slot = m_views[index];
    const uint32_t generation = slot.generation + 1;
    slot = {};
    slot.generation = generation;
    slot.alive = true;
    return {.index = index, .generation = generation};
}

bool RenderRuntime::destroyView(RenderViewId view) {
    ViewSlot* slot = findView(view);
    if (slot == nullptr || view.index == 0) {
        return false;
    }
    // Keep the generation so createView issues a newer one for this slot.
    const uint32_t generation = slot->generation;
    *slot = {};
    slot->generation = generation;
    if (m_activeView == view) {
        m_activeView = defaultView();
    }
    return true;
}

bool RenderRuntime::viewAlive(RenderViewId view) const {
    return findView(view) != nullptr;
}

uint32_t RenderRuntime::viewCount() const {
    return static_cast<uint32_t>(std::ranges::count_if(m_views, [](const ViewSlot& slot) { return slot.alive; }));
}

bool RenderRuntime::setActiveView(RenderViewId view) {
    if (findView(view) == nullptr) {
        return false;
    }
    m_activeView = view;
    return true;
}

bool RenderRuntime::requestViewSize(RenderViewId view, uint32_t width, uint32_t height) {
    ViewSlot* slot = findView(view);
    if (slot == nullptr) {
        return false;
    }
    slot->sizeRequested = true;
    slot->renderRequested = true;
    slot->requestedWidth = width;
    slot->requestedHeight = height;
    return true;
}

bool RenderRuntime::setViewInputActive(RenderViewId view, bool active) {
    ViewSlot* slot = findView(view);
    if (slot == nullptr) {
        return false;
    }
    slot->inputActive = active;
    return true;
}

bool RenderRuntime::setViewOverride(RenderViewId view, RenderView camera) {
    ViewSlot* slot = findView(view);
    if (slot == nullptr) {
        return false;
    }
    slot->cameraOverride = camera;
    return true;
}

bool RenderRuntime::clearViewOverride(RenderViewId view) {
    ViewSlot* slot = findView(view);
    if (slot == nullptr) {
        return false;
    }
    slot->cameraOverride.reset();
    return true;
}

bool RenderRuntime::setViewGridOverlay(RenderViewId view, std::optional<GridOverlay> grid) {
    ViewSlot* slot = findView(view);
    if (slot == nullptr) {
        return false;
    }
    slot->grid = grid;
    return true;
}

bool RenderRuntime::setViewOutlinedObjects(RenderViewId view, std::span<const RenderObjectId> objects) {
    ViewSlot* slot = findView(view);
    if (slot == nullptr) {
        return false;
    }
    slot->outlinedObjects.assign(objects.begin(), objects.end());
    std::ranges::sort(slot->outlinedObjects);
    const auto duplicates = std::ranges::unique(slot->outlinedObjects);
    slot->outlinedObjects.erase(duplicates.begin(), duplicates.end());
    return true;
}

bool RenderRuntime::setViewSettings(RenderViewId view, std::optional<RenderSettings> settings) {
    ViewSlot* slot = findView(view);
    if (slot == nullptr) {
        return false;
    }
    slot->settings = std::move(settings);
    return true;
}

UiTextureHandle RenderRuntime::viewTexture(RenderViewId view) const {
    const ViewSlot* slot = findView(view);
    return slot != nullptr ? slot->texture : UiTextureHandle{};
}

uint32_t RenderRuntime::viewWidth(RenderViewId view) const {
    const ViewSlot* slot = findView(view);
    return slot != nullptr ? slot->width : 0u;
}

uint32_t RenderRuntime::viewHeight(RenderViewId view) const {
    const ViewSlot* slot = findView(view);
    return slot != nullptr ? slot->height : 0u;
}

RenderViewStats RenderRuntime::viewStats(RenderViewId view) const {
    const ViewSlot* slot = findView(view);
    return slot != nullptr ? slot->stats : RenderViewStats{};
}

RenderRuntime::ViewSlot* RenderRuntime::findView(RenderViewId view) {
    if (view.index >= m_views.size() || !m_views[view.index].alive ||
        m_views[view.index].generation != view.generation) {
        return nullptr;
    }
    return &m_views[view.index];
}

const RenderRuntime::ViewSlot* RenderRuntime::findView(RenderViewId view) const {
    if (view.index >= m_views.size() || !m_views[view.index].alive ||
        m_views[view.index].generation != view.generation) {
        return nullptr;
    }
    return &m_views[view.index];
}

bool RenderRuntime::anyViewInputActive() const {
    return std::ranges::any_of(m_views, [](const ViewSlot& slot) { return slot.alive && slot.inputActive; });
}
