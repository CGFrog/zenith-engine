#include "MenuRegistry.hpp"

#include "editor/operators/OperatorSearch.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <utility>

namespace {
// Deeper submenu nesting is treated as a cycle.
constexpr int maxSubmenuDepth = 8;
constexpr std::string_view pathSeparator = " > ";

// Why entry cannot be shown, or empty when it can.
std::string entryProblem(const MenuEntry& entry) {
    if (entry.label.empty()) {
        return "Menu entries need a label";
    }
    if (entry.operatorId.empty() == entry.submenu.empty()) {
        return "Menu entry '" + entry.label + "' needs exactly one of an operator and a submenu";
    }
    if (std::ranges::any_of(entry.category, [](const std::string& category) { return category.empty(); })) {
        return "Menu entry '" + entry.label + "' has an empty category";
    }
    return {};
}

MenuItem& categoryItem(std::vector<MenuItem>& items, const std::string& label) {
    const auto existing =
        std::ranges::find_if(items, [&label](const MenuItem& item) { return item.isGroup() && item.label == label; });
    if (existing != items.end()) {
        return *existing;
    }
    return items.emplace_back(MenuItem{.label = label});
}

void removeEmptyGroups(std::vector<MenuItem>& items) {
    for (MenuItem& item : items) {
        removeEmptyGroups(item.children);
    }
    std::erase_if(items, [](const MenuItem& item) { return item.isGroup() && item.children.empty(); });
}

void collectLeaves(const std::vector<MenuItem>& items, std::string& path, std::string_view query,
                   std::vector<MenuSearchResult>& results) {
    for (const MenuItem& item : items) {
        const size_t pathLength = path.size();
        if (!path.empty()) {
            path += pathSeparator;
        }
        path += item.label;
        if (item.isGroup()) {
            collectLeaves(item.children, path, query, results);
        } else {
            const std::optional<int> labelScore = fuzzyMatchScore(query, item.label);
            const std::optional<int> pathScore = fuzzyMatchScore(query, path);
            if (labelScore || pathScore) {
                results.push_back(
                    {.item = &item, .path = path, .score = std::max(labelScore.value_or(0), pathScore.value_or(0))});
            }
        }
        path.resize(pathLength);
    }
}
} // namespace

MenuOwner MenuRegistry::createOwner() {
    return {m_nextOwner++};
}

MenuResult MenuRegistry::addMenu(MenuOwner owner, MenuDefinition menu) {
    if (menu.id.empty()) {
        return {.error = "Menus need an id"};
    }
    if (find(menu.id) != nullptr) {
        return {.error = "A menu with id '" + menu.id + "' is already registered"};
    }
    m_menus.push_back({.owner = owner, .definition = std::move(menu)});
    return {.success = true};
}

MenuResult MenuRegistry::addEntry(MenuOwner owner, std::string_view menu, MenuEntry entry) {
    if (find(menu) == nullptr) {
        return {.error = "No menu with id '" + std::string(menu) + "' is registered"};
    }
    if (std::string problem = entryProblem(entry); !problem.empty()) {
        return {.error = std::move(problem)};
    }
    if (!entry.submenu.empty() && find(entry.submenu) == nullptr) {
        return {.error = "Menu entry '" + entry.label + "' names the unregistered submenu '" + entry.submenu + "'"};
    }
    if (!entry.properties.empty()) {
        const nlohmann::json parsed = nlohmann::json::parse(entry.properties, nullptr, false);
        if (!parsed.is_object()) {
            return {.error = "Menu entry '" + entry.label + "' has properties that are not a JSON object"};
        }
    }
    m_contributions.push_back({.owner = owner, .menu = std::string(menu), .entry = std::move(entry), .fill = {}});
    return {.success = true};
}

MenuResult MenuRegistry::addSection(MenuOwner owner, std::string_view menu, MenuSectionFill fill) {
    if (find(menu) == nullptr) {
        return {.error = "No menu with id '" + std::string(menu) + "' is registered"};
    }
    if (!fill) {
        return {.error = "Menu sections need a fill function"};
    }
    m_contributions.push_back({.owner = owner, .menu = std::string(menu), .entry = {}, .fill = std::move(fill)});
    return {.success = true};
}

size_t MenuRegistry::removeOwner(MenuOwner owner) {
    const auto owned = [owner](const auto& registered) { return registered.owner == owner; };
    return std::erase_if(m_menus, owned) + std::erase_if(m_contributions, owned);
}

const MenuDefinition* MenuRegistry::find(std::string_view menu) const {
    const auto found = std::ranges::find_if(
        m_menus, [menu](const RegisteredMenu& registered) { return registered.definition.id == menu; });
    return found == m_menus.end() ? nullptr : &found->definition;
}

std::optional<ResolvedMenu> MenuRegistry::resolve(std::string_view menu, const OperatorContext& context) const {
    const MenuDefinition* definition = find(menu);
    if (definition == nullptr) {
        return std::nullopt;
    }
    ResolvedMenu resolved{.id = definition->id, .title = definition->title, .layout = definition->layout, .items = {}};
    resolveInto(menu, context, 0, resolved.items);
    removeEmptyGroups(resolved.items);
    return resolved;
}

void MenuRegistry::resolveInto(std::string_view menu, const OperatorContext& context, int depth,
                               std::vector<MenuItem>& items) const {
    for (const Contribution& contribution : m_contributions) {
        if (contribution.menu != menu) {
            continue;
        }
        if (!contribution.fill) {
            insert(contribution.entry, context, depth, items);
            continue;
        }
        std::vector<MenuEntry> entries;
        contribution.fill(context, entries);
        for (const MenuEntry& entry : entries) {
            if (entryProblem(entry).empty()) {
                insert(entry, context, depth, items);
            }
        }
    }
}

void MenuRegistry::insert(const MenuEntry& entry, const OperatorContext& context, int depth,
                          std::vector<MenuItem>& items) const {
    std::vector<MenuItem>* siblings = &items;
    for (const std::string& category : entry.category) {
        siblings = &categoryItem(*siblings, category).children;
    }
    if (entry.submenu.empty()) {
        siblings->push_back(MenuItem{.label = entry.label,
                                     .icon = entry.icon,
                                     .operatorId = entry.operatorId,
                                     .properties = entry.properties,
                                     .children = {}});
        return;
    }
    if (depth >= maxSubmenuDepth || find(entry.submenu) == nullptr) {
        return;
    }
    MenuItem submenu{.label = entry.label, .icon = entry.icon, .operatorId = {}, .properties = {}, .children = {}};
    resolveInto(entry.submenu, context, depth + 1, submenu.children);
    siblings->push_back(std::move(submenu));
}

std::vector<MenuSearchResult> searchMenu(const ResolvedMenu& menu, std::string_view query) {
    std::vector<MenuSearchResult> results;
    std::string path;
    collectLeaves(menu.items, path, query, results);
    std::ranges::stable_sort(
        results, [](const MenuSearchResult& left, const MenuSearchResult& right) { return left.score > right.score; });
    return results;
}
