#include "ComponentEditorOverrides.hpp"

#include <algorithm>

ComponentEditorOverrideResult ComponentEditorOverrides::add(std::string componentKey, ComponentEditorDraw draw) {
    if (componentKey.empty() || !draw) {
        return {.error = "Component editor overrides need a component key and a draw function"};
    }
    if (find(componentKey) != nullptr) {
        return {.error = "The component type '" + componentKey + "' already has an inspector override"};
    }
    m_overrides.emplace_back(std::move(componentKey), std::move(draw));
    return {.success = true};
}

bool ComponentEditorOverrides::remove(std::string_view componentKey) {
    return std::erase_if(m_overrides, [componentKey](const auto& entry) { return entry.first == componentKey; }) > 0;
}

const ComponentEditorDraw* ComponentEditorOverrides::find(std::string_view componentKey) const {
    const auto found =
        std::ranges::find_if(m_overrides, [componentKey](const auto& entry) { return entry.first == componentKey; });
    return found == m_overrides.end() ? nullptr : &found->second;
}

size_t ComponentEditorOverrides::size() const {
    return m_overrides.size();
}
