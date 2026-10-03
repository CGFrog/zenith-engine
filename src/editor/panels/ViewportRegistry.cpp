#include "ViewportRegistry.hpp"

#include <algorithm>
#include <optional>
#include <utility>

ViewportId ViewportRegistry::add(ViewportPanel& panel) {
    const ViewportId id{m_nextId++};
    m_entries.push_back({.id = id, .panel = &panel});
    return id;
}

void ViewportRegistry::remove(ViewportId id) {
    std::erase_if(m_entries, [id](const Entry& candidate) { return candidate.id == id; });
    if (m_active == id) {
        m_active = {};
    }
}

const ViewportRegistry::Entry* ViewportRegistry::entry(ViewportId id) const {
    const auto found = std::ranges::find(m_entries, id, &Entry::id);
    return found == m_entries.end() ? nullptr : &*found;
}

ViewportRegistry::Entry* ViewportRegistry::entry(ViewportId id) {
    const auto found = std::ranges::find(m_entries, id, &Entry::id);
    return found == m_entries.end() ? nullptr : &*found;
}

ViewportPanel* ViewportRegistry::find(ViewportId id) const {
    const Entry* found = entry(id);
    return found != nullptr ? found->panel : nullptr;
}

size_t ViewportRegistry::size() const {
    return m_entries.size();
}

void ViewportRegistry::beginFrame() {
    for (Entry& candidate : m_entries) {
        candidate.open = candidate.drawnThisFrame;
        candidate.drawnThisFrame = false;
    }
}

void ViewportRegistry::markDrawn(ViewportId id) {
    if (Entry* found = entry(id)) {
        // Drawn now, so open from now on rather than from the next frame.
        found->drawnThisFrame = true;
        found->open = true;
    }
}

bool ViewportRegistry::open(ViewportId id) const {
    const Entry* found = entry(id);
    return found != nullptr && found->open;
}

std::vector<ViewportPanel*> ViewportRegistry::openViewports() const {
    std::vector<ViewportPanel*> panels;
    for (const Entry& candidate : m_entries) {
        if (candidate.open) {
            panels.push_back(candidate.panel);
        }
    }
    return panels;
}

void ViewportRegistry::activate(ViewportId id) {
    if (entry(id) != nullptr) {
        m_active = id;
    }
}

ViewportPanel* ViewportRegistry::active() const {
    if (const Entry* activated = entry(m_active); activated != nullptr && activated->open) {
        return activated->panel;
    }
    const auto firstOpen = std::ranges::find_if(m_entries, &Entry::open);
    return firstOpen == m_entries.end() ? nullptr : firstOpen->panel;
}

RenderViewId ViewportRegistry::acquireView(RenderRuntime& rendering, ViewportId requester) {
    if (defaultViewFree()) {
        m_defaultViewHolder = requester;
        return rendering.defaultView();
    }
    return rendering.createView();
}

void ViewportRegistry::releaseView(RenderRuntime& rendering, RenderViewId view) {
    if (!view) {
        return;
    }
    if (view != rendering.defaultView()) {
        rendering.destroyView(view);
        return;
    }
    // The default view outlives every viewport and renders every frame once
    // sized, so it is left as a fresh view at the smallest size.
    constexpr uint32_t idleSize = 16;
    rendering.requestViewSize(view, idleSize, idleSize);
    rendering.clearViewOverride(view);
    rendering.setViewGridOverlay(view, std::nullopt);
    rendering.setViewOutlinedObjects(view, {});
    rendering.setViewSettings(view, std::nullopt);
    rendering.setViewInputActive(view, false);
    m_defaultViewHolder = {};
}

bool ViewportRegistry::defaultViewFree() const {
    return entry(m_defaultViewHolder) == nullptr;
}

ViewportPanel* ViewportRegistry::defaultViewHolder() const {
    return find(m_defaultViewHolder);
}

ViewportSidebarTabResult ViewportRegistry::addSidebarTab(ViewportSidebarTab tab) {
    if (tab.title.empty()) {
        return {.error = "A viewport sidebar tab needs a title"};
    }
    if (!tab.draw) {
        return {.error = "Viewport sidebar tab '" + tab.title + "' needs a draw function"};
    }
    const bool builtIn = tab.title == BuiltInViewportSidebarTab::item || tab.title == BuiltInViewportSidebarTab::tool ||
                         tab.title == BuiltInViewportSidebarTab::view;
    const bool duplicate =
        std::ranges::find(m_sidebarTabs, tab.title, &ViewportSidebarTab::title) != m_sidebarTabs.end();
    if (builtIn || duplicate) {
        return {.error = "The viewport sidebar already has a tab named '" + tab.title + "'"};
    }
    m_sidebarTabs.push_back(std::move(tab));
    return {.success = true};
}

bool ViewportRegistry::removeSidebarTab(std::string_view title) {
    return std::erase_if(m_sidebarTabs, [&](const ViewportSidebarTab& tab) { return tab.title == title; }) != 0;
}

std::span<const ViewportSidebarTab> ViewportRegistry::sidebarTabs() const {
    return m_sidebarTabs;
}
