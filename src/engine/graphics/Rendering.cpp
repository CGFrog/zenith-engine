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
    // Never reallocates, so viewDebugDraw pointers survive createView.
    m_views.reserve(maximumViews);
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
    m_picks.cancelView(view);
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

bool RenderRuntime::setViewVisibleEntities(RenderViewId view, std::optional<std::vector<RenderObjectId>> objects) {
    ViewSlot* slot = findView(view);
    if (slot == nullptr) {
        return false;
    }
    if (objects) {
        std::ranges::sort(*objects);
        const auto duplicates = std::ranges::unique(*objects);
        objects->erase(duplicates.begin(), duplicates.end());
    }
    slot->visibleObjects = std::move(objects);
    return true;
}

std::optional<std::vector<RenderObjectId>> RenderRuntime::viewVisibleEntities(RenderViewId view) const {
    const ViewSlot* slot = findView(view);
    return slot != nullptr ? slot->visibleObjects : std::nullopt;
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

DebugDraw* RenderRuntime::viewDebugDraw(RenderViewId view) {
    ViewSlot* slot = findView(view);
    return slot != nullptr ? &slot->debugDraw : nullptr;
}

const DebugDraw* RenderRuntime::viewDebugDraw(RenderViewId view) const {
    const ViewSlot* slot = findView(view);
    return slot != nullptr ? &slot->debugDraw : nullptr;
}

std::span<const ProjectedDebugLabel> RenderRuntime::viewDebugLabels(RenderViewId view) const {
    const ViewSlot* slot = findView(view);
    return slot != nullptr ? std::span<const ProjectedDebugLabel>(slot->debugLabels)
                           : std::span<const ProjectedDebugLabel>{};
}

PickTicket RenderRuntime::requestPick(RenderViewId view, PickRect rect) {
    if (findView(view) == nullptr) {
        return {};
    }
    return m_picks.request(view, rect);
}

PickStatus RenderRuntime::pickStatus(PickTicket ticket) const {
    return m_picks.status(ticket);
}

PickResult RenderRuntime::pickResult(PickTicket ticket) {
    return m_picks.take(ticket);
}

bool RenderRuntime::cancelPick(PickTicket ticket) {
    return m_picks.cancel(ticket);
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

UiImageId RenderRuntime::createUiImage(UiImage image) {
    if (!image.valid()) {
        return {};
    }
    const auto free = std::ranges::find_if(m_uiImages, [](const UiImageSlot& slot) { return !slot.alive; });
    const auto index = static_cast<uint32_t>(free - m_uiImages.begin());
    if (free == m_uiImages.end()) {
        m_uiImages.emplace_back();
    }
    UiImageSlot& slot = m_uiImages[index];
    const uint32_t generation = slot.generation + 1;
    slot = {};
    slot.generation = generation;
    slot.alive = true;
    slot.pending = std::move(image);
    return {.index = index, .generation = generation};
}

bool RenderRuntime::updateUiImage(UiImageId id, UiImage image) {
    UiImageSlot* slot = findUiImage(id);
    if (slot == nullptr || !image.valid()) {
        return false;
    }
    slot->pending = std::move(image);
    return true;
}

bool RenderRuntime::destroyUiImage(UiImageId id) {
    UiImageSlot* slot = findUiImage(id);
    if (slot == nullptr) {
        return false;
    }
    // Keep the generation so createUiImage issues a newer one for this slot.
    const uint32_t generation = slot->generation;
    *slot = {};
    slot->generation = generation;
    return true;
}

bool RenderRuntime::uiImageAlive(UiImageId id) const {
    return findUiImage(id) != nullptr;
}

UiTextureHandle RenderRuntime::uiImageTexture(UiImageId id) const {
    const UiImageSlot* slot = findUiImage(id);
    return slot != nullptr ? slot->texture : UiTextureHandle{};
}

RenderRuntime::UiImageSlot* RenderRuntime::findUiImage(UiImageId id) {
    if (id.index >= m_uiImages.size() || !m_uiImages[id.index].alive ||
        m_uiImages[id.index].generation != id.generation) {
        return nullptr;
    }
    return &m_uiImages[id.index];
}

const RenderRuntime::UiImageSlot* RenderRuntime::findUiImage(UiImageId id) const {
    if (id.index >= m_uiImages.size() || !m_uiImages[id.index].alive ||
        m_uiImages[id.index].generation != id.generation) {
        return nullptr;
    }
    return &m_uiImages[id.index];
}
