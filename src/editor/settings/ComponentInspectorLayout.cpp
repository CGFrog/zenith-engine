#include "ComponentInspectorLayout.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <utility>

namespace {
using Json = nlohmann::json;

constexpr int currentVersion = 1;

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
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        if (error) {
            return {.error = "Cannot inspect component inspector settings " + path.string() + ": " + error.message()};
        }
        return {.success = true};
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {.error = "Cannot read component inspector settings " + path.string() + "; using defaults."};
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    const auto invalid = [&path](const std::string& reason) {
        return ComponentInspectorLayoutResult{.error = "Invalid component inspector settings in " + path.string() +
                                                       " (" + reason + "); using defaults."};
    };
    const Json document = Json::parse(contents.str(), nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return invalid("the file is not a JSON object");
    }
    const auto version = document.find("version");
    if (version == document.end() || !version->is_number_integer() || version->get<int>() != currentVersion) {
        return invalid("unsupported version (expected " + std::to_string(currentVersion) + ")");
    }
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
    const Json document{{"version", currentVersion}, {"order", m_order}, {"collapsed", m_collapsed}};
    std::error_code error;
    if (const std::filesystem::path parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            return {.error = "Cannot create the component inspector settings directory: " + error.message()};
        }
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    // Replace invalid UTF-8 rather than throwing from dump().
    output << document.dump(2, ' ', false, Json::error_handler_t::replace) << '\n';
    if (!output) {
        return {.error = "Cannot write component inspector settings " + path.string()};
    }
    return {.success = true};
}
