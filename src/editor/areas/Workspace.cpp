#include "editor/areas/Workspace.hpp"

#include "editor/areas/AreaRegistry.hpp"

#include <algorithm>
#include <utility>

std::string areaId(std::string_view typeId, uint32_t number) {
    return std::string(typeId) + "." + std::to_string(number);
}

std::string AreaInstance::id() const {
    return areaId(typeId, number);
}

std::string AreaInstance::windowName(std::string_view title) const {
    return std::string(title) + "##" + id();
}

Workspace::Workspace(std::string name)
    : m_name(std::move(name)) {}

const std::string& Workspace::name() const {
    return m_name;
}

std::span<AreaInstance> Workspace::areas() {
    return m_areas;
}

std::span<const AreaInstance> Workspace::areas() const {
    return m_areas;
}

const std::string& Workspace::layout() const {
    return m_layout;
}

void Workspace::setLayout(std::string layout) {
    m_layout = std::move(layout);
}

AreaInstance* Workspace::find(std::string_view id) {
    const auto found = std::ranges::find_if(m_areas, [id](const AreaInstance& area) { return area.id() == id; });
    return found == m_areas.end() ? nullptr : &*found;
}

const AreaInstance* Workspace::find(std::string_view id) const {
    const auto found = std::ranges::find_if(m_areas, [id](const AreaInstance& area) { return area.id() == id; });
    return found == m_areas.end() ? nullptr : &*found;
}

bool Workspace::anyVisible(std::string_view typeId) const {
    return std::ranges::any_of(m_areas,
                               [typeId](const AreaInstance& area) { return area.typeId == typeId && area.visible; });
}

AreaResult Workspace::addStoredArea(std::string typeId, uint32_t number, bool visible) {
    if (typeId.empty() || typeId.find_first_of(".#") != std::string::npos || number == 0) {
        return {.error = "Invalid area \"" + areaId(typeId, number) + "\" in workspace \"" + m_name + "\""};
    }
    std::string id = areaId(typeId, number);
    if (find(id) != nullptr) {
        return {.error = "Workspace \"" + m_name + "\" lists area \"" + id + "\" twice"};
    }
    if (!visible && hiddenOfType(typeId) != nullptr) {
        // Keep at most one hidden area per type; a second carries no state.
        return {.success = true};
    }
    m_areas.push_back({.typeId = std::move(typeId), .number = number, .visible = visible});
    return {.success = true, .areaId = std::move(id)};
}

void Workspace::sync(const AreaRegistry& registry) {
    // A stored workspace may list several areas of a single-instance type;
    // keep the first visible one (or the first) so a shared panel is never
    // updated or drawn twice.
    for (const AreaType& type : registry.types()) {
        if (type.instancing != AreaInstancing::Single || countOfType(type.id) < 2) {
            continue;
        }
        const AreaInstance* keep = firstOfType(type.id, true);
        const std::string keepId = keep != nullptr ? keep->id() : firstOfType(type.id, false)->id();
        std::erase_if(m_areas, [&type, &keepId](const AreaInstance& area) {
            return area.typeId == type.id && area.id() != keepId;
        });
    }
    for (AreaInstance& area : m_areas) {
        const AreaType* type = registry.find(area.typeId);
        if (type == nullptr) {
            area.panel.reset();
        } else if (!area.panel) {
            area.panel = type->create();
        }
    }
    for (const AreaType& type : registry.types()) {
        if (countOfType(type.id) == 0) {
            AreaInstance& area = addArea(registry, type.id);
            area.visible = false;
        }
    }
}

AreaResult Workspace::open(const AreaRegistry& registry, std::string_view typeId) {
    if (registry.find(typeId) == nullptr) {
        return {.error = "There is no editor type \"" + std::string(typeId) + "\""};
    }
    if (AreaInstance* visible = firstOfType(typeId, true)) {
        return {.success = true, .areaId = visible->id()};
    }
    AreaInstance* area = hiddenOfType(typeId);
    if (area == nullptr) {
        area = &addArea(registry, typeId);
    }
    area->visible = true;
    return {.success = true, .areaId = area->id()};
}

AreaResult Workspace::openNew(const AreaRegistry& registry, std::string_view typeId) {
    const AreaType* type = registry.find(typeId);
    if (type == nullptr) {
        return {.error = "There is no editor type \"" + std::string(typeId) + "\""};
    }
    const AreaInstance* visible = firstOfType(typeId, true);
    if (type->instancing == AreaInstancing::Single || visible == nullptr) {
        return open(registry, typeId);
    }
    const uint32_t dockId = visible->dockId;
    AreaInstance& area = addArea(registry, typeId);
    area.dockRequest = dockId;
    return {.success = true, .areaId = area.id()};
}

AreaResult Workspace::close(std::string_view id) {
    AreaInstance* area = find(id);
    if (area == nullptr) {
        return {.error = "There is no area \"" + std::string(id) + "\" in workspace \"" + m_name + "\""};
    }
    // Copied: id may view the area's own storage, which erasing frees.
    std::string closedId = area->id();
    area->dockRequest = 0;
    if (countOfType(area->typeId) == 1) {
        area->visible = false;
        return {.success = true, .areaId = std::move(closedId)};
    }
    // Another area of the type remains, so drop this one; if a hidden one
    // remains, this was visible and the invariant still holds.
    std::erase_if(m_areas, [&closedId](const AreaInstance& candidate) { return candidate.id() == closedId; });
    return {.success = true, .areaId = std::move(closedId)};
}

AreaResult Workspace::toggle(const AreaRegistry& registry, std::string_view typeId) {
    if (!anyVisible(typeId)) {
        return open(registry, typeId);
    }
    std::vector<std::string> visible;
    for (const AreaInstance& area : m_areas) {
        if (area.typeId == typeId && area.visible) {
            visible.push_back(area.id());
        }
    }
    for (const std::string& id : visible) {
        close(id);
    }
    return {.success = true, .areaId = visible.front()};
}

AreaResult Workspace::changeType(const AreaRegistry& registry, std::string_view id, std::string_view typeId) {
    const AreaInstance* source = find(id);
    if (source == nullptr) {
        return {.error = "There is no area \"" + std::string(id) + "\" in workspace \"" + m_name + "\""};
    }
    const AreaType* type = registry.find(typeId);
    if (type == nullptr) {
        return {.error = "There is no editor type \"" + std::string(typeId) + "\""};
    }
    if (source->typeId == typeId) {
        return {.success = true, .areaId = std::string(id)};
    }
    const std::string sourceId(id);
    const uint32_t dockId = source->dockId;

    // A single-instance type moves its one area here; a multiple-instance
    // type reuses its hidden area so no stale hidden area is left behind.
    AreaInstance* target =
        type->instancing == AreaInstancing::Single ? firstOfType(typeId, false) : hiddenOfType(typeId);
    if (target == nullptr) {
        target = &addArea(registry, typeId);
    }
    target->visible = true;
    target->dockRequest = dockId;
    const std::string targetId = target->id();
    // May remove the source, so target is not used afterwards.
    close(sourceId);
    return {.success = true, .areaId = targetId};
}

AreaInstance* Workspace::hiddenOfType(std::string_view typeId) {
    const auto found = std::ranges::find_if(
        m_areas, [typeId](const AreaInstance& area) { return area.typeId == typeId && !area.visible; });
    return found == m_areas.end() ? nullptr : &*found;
}

AreaInstance* Workspace::firstOfType(std::string_view typeId, bool visibleOnly) {
    const auto found = std::ranges::find_if(m_areas, [typeId, visibleOnly](const AreaInstance& area) {
        return area.typeId == typeId && (area.visible || !visibleOnly);
    });
    return found == m_areas.end() ? nullptr : &*found;
}

size_t Workspace::countOfType(std::string_view typeId) const {
    return static_cast<size_t>(
        std::ranges::count_if(m_areas, [typeId](const AreaInstance& area) { return area.typeId == typeId; }));
}

uint32_t Workspace::nextNumber(std::string_view typeId) const {
    uint32_t number = 1;
    while (find(areaId(typeId, number)) != nullptr) {
        ++number;
    }
    return number;
}

AreaInstance& Workspace::addArea(const AreaRegistry& registry, std::string_view typeId) {
    const AreaType* type = registry.find(typeId);
    const uint32_t number = nextNumber(typeId);
    AreaInstance& area = m_areas.emplace_back();
    area.typeId = std::string(typeId);
    area.number = number;
    area.panel = type != nullptr ? type->create() : nullptr;
    return area;
}

void WorkspaceSet::assign(std::vector<Workspace> workspaces, std::string_view active) {
    m_workspaces = std::move(workspaces);
    m_requested.reset();
    m_active = 0;
    for (size_t index = 0; index < m_workspaces.size(); ++index) {
        if (m_workspaces[index].name() == active) {
            m_active = index;
        }
    }
}

bool WorkspaceSet::empty() const {
    return m_workspaces.empty();
}

std::span<Workspace> WorkspaceSet::workspaces() {
    return m_workspaces;
}

std::span<const Workspace> WorkspaceSet::workspaces() const {
    return m_workspaces;
}

Workspace& WorkspaceSet::active() {
    return m_workspaces[m_active];
}

const Workspace& WorkspaceSet::active() const {
    return m_workspaces[m_active];
}

Workspace* WorkspaceSet::find(std::string_view name) {
    const auto found =
        std::ranges::find_if(m_workspaces, [name](const Workspace& workspace) { return workspace.name() == name; });
    return found == m_workspaces.end() ? nullptr : &*found;
}

AreaResult WorkspaceSet::requestActivation(std::string_view name) {
    for (size_t index = 0; index < m_workspaces.size(); ++index) {
        if (m_workspaces[index].name() == name) {
            if (index == m_active) {
                m_requested.reset();
            } else {
                m_requested = index;
            }
            return {.success = true};
        }
    }
    return {.error = "There is no workspace named \"" + std::string(name) + "\""};
}

std::optional<size_t> WorkspaceSet::requestedActivation() const {
    return m_requested;
}

void WorkspaceSet::completeActivation() {
    if (m_requested) {
        m_active = *m_requested;
        m_requested.reset();
    }
}

void WorkspaceSet::sync(const AreaRegistry& registry) {
    for (Workspace& workspace : m_workspaces) {
        workspace.sync(registry);
    }
}
