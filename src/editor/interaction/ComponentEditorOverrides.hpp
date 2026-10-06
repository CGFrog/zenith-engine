#pragma once

// Custom component editors contributed from outside EntityPropertiesView, for example by a game module, keyed by
// component type key.

#include "engine/scene/Scene.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct EditorContext;

// Draws a component in place and returns whether it changed it.
using ComponentEditorDraw = std::function<bool(EditorContext& context, Entity entity, void* component)>;

struct ComponentEditorOverrideResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// Custom component editors contributed from outside EntityPropertiesView, for
// example by a game module, keyed by component type key. An override replaces
// the reflected fields and any built-in custom editor of its type in every
// Inspector and Properties editor. The session owns it; whoever adds an
// override must remove it before the code its draw function runs is unloaded.
class ComponentEditorOverrides {
  public:
    // Rejects an empty key, a missing draw function, and a key that already
    // has an override.
    ComponentEditorOverrideResult add(std::string componentKey, ComponentEditorDraw draw);
    // Returns whether the key had an override.
    bool remove(std::string_view componentKey);
    // The override for the key, or null; valid until the next add or
    // remove.
    const ComponentEditorDraw* find(std::string_view componentKey) const;
    size_t size() const;

  private:
    std::vector<std::pair<std::string, ComponentEditorDraw>> m_overrides;
};
