#include "OutlinerFilter.hpp"

#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/Prefab.hpp"
#include "engine/scene/Tags.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <utility>

namespace {
char lower(char character) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
}

bool equalsIgnoringCase(std::string_view left, std::string_view right) {
    return std::ranges::equal(left, right, [](char a, char b) { return lower(a) == lower(b); });
}

bool isSpace(char character) {
    return std::isspace(static_cast<unsigned char>(character)) != 0;
}

std::optional<OutlinerFilterField> fieldForPrefix(std::string_view prefix) {
    constexpr std::array<std::pair<std::string_view, OutlinerFilterField>, 6> prefixes{{
        {"c", OutlinerFilterField::Component},
        {"component", OutlinerFilterField::Component},
        {"t", OutlinerFilterField::Tag},
        {"tag", OutlinerFilterField::Tag},
        {"p", OutlinerFilterField::Prefab},
        {"prefab", OutlinerFilterField::Prefab},
    }};
    for (const auto& [name, field] : prefixes) {
        if (equalsIgnoringCase(prefix, name)) {
            return field;
        }
    }
    return std::nullopt;
}

// Reads one term starting at position (past any whitespace): characters up
// to the next unquoted whitespace, with quotes removed.
std::string readWord(std::string_view text, size_t& position) {
    std::string word;
    bool quoted = false;
    for (; position < text.size(); ++position) {
        const char character = text[position];
        if (character == '"') {
            quoted = !quoted;
        } else if (!quoted && isSpace(character)) {
            break;
        } else {
            word += character;
        }
    }
    return word;
}

OutlinerFilterTerm termFromWord(std::string word) {
    if (const size_t colon = word.find(':'); colon != std::string::npos) {
        if (const std::optional<OutlinerFilterField> field = fieldForPrefix(std::string_view(word).substr(0, colon))) {
            return {.field = *field, .text = word.substr(colon + 1)};
        }
    }
    return {.field = OutlinerFilterField::Name, .text = std::move(word)};
}
} // namespace

bool OutlinerFilter::namesOnly() const {
    return std::ranges::all_of(terms,
                               [](const OutlinerFilterTerm& term) { return term.field == OutlinerFilterField::Name; });
}

OutlinerFilter parseOutlinerFilter(std::string_view text) {
    OutlinerFilter filter;
    size_t position = 0;
    while (position < text.size()) {
        if (isSpace(text[position])) {
            ++position;
            continue;
        }
        // A quoted prefix value ("c:"Rigid Body"") stays one word because
        // the quote toggles inside readWord.
        std::string word = readWord(text, position);
        if (!word.empty()) {
            filter.terms.push_back(termFromWord(std::move(word)));
        }
    }
    return filter;
}

bool containsIgnoringCase(std::string_view text, std::string_view part) {
    if (part.empty()) {
        return true;
    }
    if (part.size() > text.size()) {
        return false;
    }
    for (size_t start = 0; start + part.size() <= text.size(); ++start) {
        if (equalsIgnoringCase(text.substr(start, part.size()), part)) {
            return true;
        }
    }
    return false;
}

OutlinerFilterMatcher::OutlinerFilterMatcher(const OutlinerFilter& filter, const ComponentTypeRegistry& types)
    : m_filter(filter)
    , m_components(filter.terms.size()) {
    for (size_t index = 0; index < filter.terms.size(); ++index) {
        const OutlinerFilterTerm& term = filter.terms[index];
        if (term.field != OutlinerFilterField::Component) {
            continue;
        }
        for (const ComponentType& type : types.types()) {
            if (containsIgnoringCase(type.key(), term.text) || containsIgnoringCase(type.displayName(), term.text)) {
                m_components[index].push_back(&type);
            }
        }
    }
}

bool OutlinerFilterMatcher::termMatches(size_t term, const Scene& scene, Entity entity, std::string_view label) const {
    const OutlinerFilterTerm& filterTerm = m_filter.terms[term];
    const entt::registry& registry = scene.registry();
    switch (filterTerm.field) {
    case OutlinerFilterField::Name:
        return containsIgnoringCase(label, filterTerm.text);
    case OutlinerFilterField::Component:
        return std::ranges::any_of(m_components[term],
                                   [&scene, entity](const ComponentType* type) { return type->has(scene, entity); });
    case OutlinerFilterField::Tag: {
        const auto* tags = registry.try_get<TagsComponent>(entity);
        return tags != nullptr && std::ranges::any_of(tags->tags, [&filterTerm](const std::string& tag) {
                   return containsIgnoringCase(tag, filterTerm.text);
               });
    }
    case OutlinerFilterField::Prefab: {
        const auto* instance = registry.try_get<PrefabInstanceComponent>(entity);
        return instance != nullptr && containsIgnoringCase(instance->prefab, filterTerm.text);
    }
    }
    return false;
}

bool OutlinerFilterMatcher::matchesEntity(const Scene& scene, Entity entity, std::string_view label) const {
    for (size_t term = 0; term < m_filter.terms.size(); ++term) {
        if (!termMatches(term, scene, entity, label)) {
            return false;
        }
    }
    return true;
}

bool OutlinerFilterMatcher::matchesLabel(std::string_view label) const {
    return std::ranges::all_of(m_filter.terms, [label](const OutlinerFilterTerm& term) {
        return term.field == OutlinerFilterField::Name && containsIgnoringCase(label, term.text);
    });
}

std::string outlinerEntityLabel(const Scene& scene, Entity entity) {
    const auto* name = scene.registry().try_get<NameComponent>(entity);
    return name == nullptr || name->name.empty() ? std::string("Unnamed Entity") : name->name;
}

std::vector<SceneEntityId> findOutlinerMatches(const Scene& scene, const OutlinerFilter& filter,
                                               const ComponentTypeRegistry& types) {
    std::vector<SceneEntityId> matches;
    if (filter.empty()) {
        return matches;
    }
    const OutlinerFilterMatcher matcher(filter, types);
    for (const Entity entity : scene.registry().view<const SceneEntityIdComponent, const RelationshipComponent>()) {
        if (matcher.matchesEntity(scene, entity, outlinerEntityLabel(scene, entity))) {
            matches.push_back(scene.id(entity));
        }
    }
    std::ranges::sort(matches);
    return matches;
}
