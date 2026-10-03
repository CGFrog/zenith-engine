#pragma once

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class EditorPanel;

// How many areas of one type a workspace may show at once.
enum class AreaInstancing {
    // One area per workspace, typically a view of a panel the session owns.
    Single,
    // Any number of areas, each with its own panel (for example two
    // outliners with different filters).
    Multiple,
};

// Creates the panel an area shows.
using AreaPanelFactory = std::function<std::unique_ptr<EditorPanel>()>;

// An editor type an area can show, in the Blender sense: the Viewport, the
// Hierarchy, a module's panel. The id is persisted in workspaces.json and in
// the window names that key the docking layout, so it must never change once
// shipped; the title is what users see.
struct AreaType {
    // Stable, lowercase, dot-free id such as "hierarchy". Module panels use
    // "module:<panel title>".
    std::string id;
    std::string title;
    AreaInstancing instancing{AreaInstancing::Single};
    AreaPanelFactory create;

    // A single-instance type showing a panel owned elsewhere, titled after
    // the panel. The panel must outlive every area showing it.
    static AreaType shared(std::string id, EditorPanel& panel);
};

struct AreaRegistryResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// The editor types areas can switch between, in View menu order.
class AreaRegistry {
  public:
    // Fails on an empty or duplicate id, an id containing '.' or '#' (which
    // delimit window names), an empty title, or a missing factory.
    AreaRegistryResult add(AreaType type);
    // Returns whether a type was removed. Areas of the type stay in their
    // workspaces without a panel until it is registered again.
    bool remove(std::string_view id);

    const AreaType* find(std::string_view id) const;
    std::span<const AreaType> types() const;

  private:
    std::vector<AreaType> m_types;
};
