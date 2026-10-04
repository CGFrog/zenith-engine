#include "ComponentInspectorLayout.hpp"

#include "engine/platform/VersionedJson.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <utility>

namespace {
using Json = nlohmann::json;

const VersionedJsonFormat componentInspectorLayoutFormat{.description = "component inspector settings"};

bool contains(std::span<const std::string> keys, std::string_view key) {
    return std::ranges::find(keys, key) != keys.end();
}

// Reads an array of distinct component keys.
bool readKeys(const Json& document, const char* name, std::vector<std::string>& keys, std::string& error) {
    const auto entry = document.find(name);
    if (entry == document.end()) {
        return true;
    }
    if (!entry->is_array()) {
        error = std::string(name) + " must be an array of component keys";
        return false;
    }
    for (const Json& key : *entry) {
        if (!key.is_string() || key.get_ref<const std::string&>().empty()) {
            error = std::string(name) + " must be an array of component keys";
            return false;
        }
        if (!contains(keys, key.get_ref<const std::string&>())) {
            keys.push_back(key.get<std::string>());
        }
    }
    return true;
}
} // namespace

bool ComponentInspectorLayout::collapsed(std::string_view componentKey) const {
    return contains(m_collapsed, componentKey);
}

void ComponentInspectorLayout::setCollapsed(std::string_view componentKey, bool collapsed) {
    if (this->collapsed(componentKey) == collapsed) {
        return;
    }
    if (collapsed) {
        m_collapsed.emplace_back(componentKey);
    } else {
        std::erase(m_collapsed, componentKey);
    }
    m_changed = true;
}

std::vector<std::string> ComponentInspectorLayout::sorted(std::vector<std::string> keys) const {
    const auto rank = [this](const std::string& key) {
        return static_cast<size_t>(std::ranges::find(m_order, key) - m_order.begin());
    };
    std::ranges::stable_sort(keys, {}, rank);
    return keys;
}

bool ComponentInspectorLayout::move(std::string_view componentKey, ComponentMoveDirection direction,
                                    std::span<const std::string> displayed) {
    const auto found = std::ranges::find(displayed, componentKey);
    if (found == displayed.end()) {
        return false;
    }
    const size_t index = static_cast<size_t>(found - displayed.begin());
    const size_t last = displayed.size() - 1;
    size_t target = index;
    switch (direction) {
    case ComponentMoveDirection::Up:
        target = index == 0 ? 0 : index - 1;
        break;
    case ComponentMoveDirection::Down:
        target = std::min(index + 1, last);
        break;
    case ComponentMoveDirection::Top:
        target = 0;
        break;
    case ComponentMoveDirection::Bottom:
        target = last;
        break;
    }
    if (target == index) {
        return false;
    }
    std::vector<std::string> reordered(displayed.begin(), displayed.end());
    std::string moved = std::move(reordered[index]);
    reordered.erase(reordered.begin() + static_cast<std::ptrdiff_t>(index));
    reordered.insert(reordered.begin() + static_cast<std::ptrdiff_t>(target), std::move(moved));

    // Displayed keys take the stored slots displayed keys held, so stored keys
    // that are not displayed keep their places.
    std::vector<std::string> order = m_order;
    for (const std::string& key : displayed) {
        if (!contains(order, key)) {
            order.push_back(key);
        }
    }
    size_t next = 0;
    for (std::string& key : order) {
        if (next < reordered.size() && contains(displayed, key)) {
            key = reordered[next++];
        }
    }
    m_order = std::move(order);
    m_changed = true;
    return true;
}

const std::vector<std::string>& ComponentInspectorLayout::order() const {
    return m_order;
}

const std::vector<std::string>& ComponentInspectorLayout::collapsedKeys() const {
    return m_collapsed;
}

bool ComponentInspectorLayout::takeChanged() {
    return std::exchange(m_changed, false);
}

ComponentInspectorLayoutResult ComponentInspectorLayout::load(const std::filesystem::path& path) {
    m_order.clear();
    m_collapsed.clear();
    m_changed = false;
    const VersionedJsonResult read = readVersionedJson(path, componentInspectorLayoutFormat);
    if (!read) {
        return {.error = read.error + "; using defaults."};
    }
    if (!read.found) {
        return {.success = true};
    }
    const Json& document = read.document;
    const auto invalid = [&path](const std::string& reason) {
        return ComponentInspectorLayoutResult{.error = "Invalid component inspector settings in " + path.string() +
                                                       " (" + reason + "); using defaults."};
    };
    std::vector<std::string> order;
    std::vector<std::string> collapsedKeys;
    std::string reason;
    if (!readKeys(document, "order", order, reason) || !readKeys(document, "collapsed", collapsedKeys, reason)) {
        return invalid(reason);
    }
    m_order = std::move(order);
    m_collapsed = std::move(collapsedKeys);
    return {.success = true};
}

ComponentInspectorLayoutResult ComponentInspectorLayout::save(const std::filesystem::path& path) const {
    const Json document{{"order", m_order}, {"collapsed", m_collapsed}};
    if (const PlatformResult written = writeVersionedJson(path, componentInspectorLayoutFormat, document); !written) {
        return {.error = written.error};
    }
    return {.success = true};
}
