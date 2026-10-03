#pragma once

#include "editor/areas/AreaInstance.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class AreaRegistry;

struct AreaResult {
    bool success{false};
    std::string error;
    // The area the operation showed or changed, when it succeeded.
    std::string areaId;

    explicit operator bool() const {
        return success;
    }
};

// A named arrangement of areas with its own docking layout, shown as a tab in
// the main menu bar (Layout, Lighting, Debug).
//
// Invariants once synced with a registry: every registered type has at least
// one area, and at most one hidden area. Closing the last area of a type only
// hides it, so its dock slot is remembered for when it is shown again; closing
// any other area removes it. Operations never draw: they change which areas
// exist and request dock placement, and the next drawn frame applies it.
class Workspace {
  public:
    explicit Workspace(std::string name);

    const std::string& name() const;
    // Areas in drawing order.
    std::span<AreaInstance> areas();
    std::span<const AreaInstance> areas() const;
    // ImGui settings text (SaveIniSettingsToMemory) captured when the
    // workspace was last active; empty means build the default layout.
    const std::string& layout() const;
    void setLayout(std::string layout);

    AreaInstance* find(std::string_view areaId);
    const AreaInstance* find(std::string_view areaId) const;
    bool anyVisible(std::string_view typeId) const;

    // Adds an area as stored on disk; its panel is created by sync(). Fails
    // on a duplicate id or a malformed type id.
    AreaResult addStoredArea(std::string typeId, uint32_t number, bool visible);
    // Creates panels for areas of registered types, adds a hidden area for
    // each registered type that has none, drops extra areas of
    // single-instance types (keeping a visible one), and releases the panels
    // of areas whose type is no longer registered.
    void sync(const AreaRegistry& registry);

    // Shows the type: does nothing if an area of it is visible, otherwise
    // shows its hidden area or adds one.
    AreaResult open(const AreaRegistry& registry, std::string_view typeId);
    // Adds another area of a multiple-instance type, tabbed beside a visible
    // area of the type. Single-instance types behave as open().
    AreaResult openNew(const AreaRegistry& registry, std::string_view typeId);
    AreaResult close(std::string_view areaId);
    // Closes every visible area of the type, or opens it if none is visible.
    AreaResult toggle(const AreaRegistry& registry, std::string_view typeId);
    // Shows typeId in the area's place and closes the area. A single-instance
    // type that is open elsewhere moves here.
    AreaResult changeType(const AreaRegistry& registry, std::string_view areaId, std::string_view typeId);

  private:
    AreaInstance* hiddenOfType(std::string_view typeId);
    AreaInstance* firstOfType(std::string_view typeId, bool visibleOnly);
    size_t countOfType(std::string_view typeId) const;
    uint32_t nextNumber(std::string_view typeId) const;
    AreaInstance& addArea(const AreaRegistry& registry, std::string_view typeId);

    std::string m_name;
    std::vector<AreaInstance> m_areas;
    std::string m_layout;
};

// The editor's workspaces and which one is active. Switching is requested
// while a frame is drawn and applied between frames (see AreaWindows.hpp),
// because ImGui layouts may only be replaced outside a frame.
class WorkspaceSet {
  public:
    // Replaces every workspace; active names the active one (the first when
    // absent). Pending switches are dropped.
    void assign(std::vector<Workspace> workspaces, std::string_view active);

    bool empty() const;
    std::span<Workspace> workspaces();
    std::span<const Workspace> workspaces() const;
    // Requires !empty().
    Workspace& active();
    const Workspace& active() const;
    Workspace* find(std::string_view name);

    // Fails when no workspace has the name. Requesting the active workspace
    // cancels a pending switch.
    AreaResult requestActivation(std::string_view name);
    std::optional<size_t> requestedActivation() const;
    // Makes the requested workspace active and clears the request.
    void completeActivation();

    void sync(const AreaRegistry& registry);

  private:
    std::vector<Workspace> m_workspaces;
    size_t m_active{0};
    std::optional<size_t> m_requested;
};
