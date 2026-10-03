#pragma once

#include "editor/operators/OperatorRegistry.hpp"
#include "editor/settings/ComponentInspectorLayout.hpp"
#include "engine/scene/Scene.hpp"

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

class ComponentTypeRegistry;

// Component operators that need only the document, the selection, a text
// clipboard, and the inspector layout, so they run headless:
//
// | id                                 | acts on                                          |
// | ---------------------------------- | ------------------------------------------------ |
// | `object.component_reset`           | every selected object with the component         |
// | `object.component_copy`            | the active object's values, to the clipboard     |
// | `object.component_paste`           | every selected object with the component         |
// | `object.component_copy_to_selected`| the other selected objects (added where missing) |
// | `object.component_move`            | the inspector's display order (not undoable)     |
//
// Each takes the component type key as its "component" property. Values are
// copied as the component's scene-document JSON (see componentClipboardText),
// so custom codecs, asset references, and schema versions round-trip.

// The text the component copy and paste operators read and write. The editor
// session backs it with the system clipboard.
class TextClipboard {
  public:
    virtual std::string text() const = 0;
    virtual void setText(std::string text) = 0;

  protected:
    ~TextClipboard() = default;
};

// A clipboard held in memory, for headless callers.
class MemoryClipboard final : public TextClipboard {
  public:
    std::string text() const override {
        return m_text;
    }

    void setText(std::string text) override {
        m_text = std::move(text);
    }

  private:
    std::string m_text;
};

// What the component operators use besides their OperatorContext. Both must
// outlive the registry the operators are added to.
struct ComponentOperatorServices {
    TextClipboard& clipboard;
    ComponentInspectorLayout& layout;
};

struct ComponentMoveProperties {
    // Component type key, for example "point_light".
    std::string component;
    ComponentMoveDirection direction{ComponentMoveDirection::Up};
    // The category label of the sections the inspector lists (a Properties
    // category tab), so the component moves past its visible neighbours;
    // empty when every category is listed.
    std::string category;
};

// Clipboard text holding one component's document JSON:
// {"zenith_component": key, "value": {...}}.
std::string componentClipboardText(std::string_view componentKey, const nlohmann::json& value);

// The non-internal component type keys entity has, in the inspector's display
// order, limited to one category label (see componentCategoryLabel) unless
// category is empty. The component move operator reorders within this list.
std::vector<std::string> displayedComponentKeys(const ComponentTypeRegistry& types,
                                                const ComponentInspectorLayout& layout, const Scene& scene,
                                                SceneEntityId entity, std::string_view category = {});

OperatorRegistrationResult registerComponentOperators(OperatorRegistry& registry, ComponentOperatorServices services);
