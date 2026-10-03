#include "editor/areas/AreaRegistry.hpp"

#include "editor/panels/EditorPanel.hpp"

#include <algorithm>
#include <utility>

namespace {
// Shows a panel the session owns; destroying the area leaves the panel alone.
class SharedPanelView final : public EditorPanel {
  public:
    explicit SharedPanelView(EditorPanel& panel)
        : m_panel(panel) {}

    const char* title() const override {
        return m_panel.title();
    }

    int windowFlags() const override {
        return m_panel.windowFlags();
    }

    bool editModeOnly() const override {
        return m_panel.editModeOnly();
    }

    void update(EditorContext& context, const InputState& input, float deltaSeconds) override {
        m_panel.update(context, input, deltaSeconds);
    }

    void draw(EditorContext& context) override {
        m_panel.draw(context);
    }

    void hidden(EditorContext& context) override {
        m_panel.hidden(context);
    }

  private:
    EditorPanel& m_panel;
};
} // namespace

AreaType AreaType::shared(std::string id, EditorPanel& panel) {
    return {.id = std::move(id),
            .title = panel.title(),
            .instancing = AreaInstancing::Single,
            .create = [&panel]() -> std::unique_ptr<EditorPanel> { return std::make_unique<SharedPanelView>(panel); }};
}

AreaRegistryResult AreaRegistry::add(AreaType type) {
    if (type.id.empty() || type.id.find_first_of(".#") != std::string::npos) {
        return {.error = "Area type id \"" + type.id + "\" must be non-empty and contain no '.' or '#'"};
    }
    if (type.title.empty()) {
        return {.error = "Area type \"" + type.id + "\" needs a title"};
    }
    if (!type.create) {
        return {.error = "Area type \"" + type.id + "\" needs a panel factory"};
    }
    if (find(type.id) != nullptr) {
        return {.error = "Area type \"" + type.id + "\" is already registered"};
    }
    m_types.push_back(std::move(type));
    return {.success = true};
}

bool AreaRegistry::remove(std::string_view id) {
    return std::erase_if(m_types, [id](const AreaType& type) { return type.id == id; }) > 0;
}

const AreaType* AreaRegistry::find(std::string_view id) const {
    const auto found = std::ranges::find(m_types, id, &AreaType::id);
    return found == m_types.end() ? nullptr : &*found;
}

std::span<const AreaType> AreaRegistry::types() const {
    return m_types;
}
