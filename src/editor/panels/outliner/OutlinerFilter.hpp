#pragma once

#include "engine/scene/Scene.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

class ComponentType;
class ComponentTypeRegistry;

// The outliner's search syntax. A filter is whitespace-separated terms that
// must all match (AND); double quotes keep spaces in one term ("Big Crate",
// c:"Rigid Body"). Every comparison is a case-insensitive substring:
//
//   text                the row's name
//   c:x  component:x    has a component whose type key or display name
//                       contains x (c:light matches Point Light and
//                       Directional Light)
//   t:x  tag:x          has a TagsComponent tag containing x
//   p:x  prefab:x       is a prefab instance whose prefab path contains x
//
// A prefix the syntax does not know ("http:x") is ordinary text. A field
// with no value ("t:") matches any tag, any prefab instance, or any
// registered component.
enum class OutlinerFilterField : uint8_t {
    Name,
    Component,
    Tag,
    Prefab,
};

struct OutlinerFilterTerm {
    OutlinerFilterField field{OutlinerFilterField::Name};
    std::string text;

    bool operator==(const OutlinerFilterTerm&) const = default;
};

struct OutlinerFilter {
    std::vector<OutlinerFilterTerm> terms;

    bool empty() const {
        return terms.empty();
    }

    // Whether every term names the label, so rows that are not entities
    // (collections, prefab groups, assets) can match the filter.
    bool namesOnly() const;
};

OutlinerFilter parseOutlinerFilter(std::string_view text);

// Whether text contains part, ignoring ASCII case. An empty part matches.
bool containsIgnoringCase(std::string_view text, std::string_view part);

// Matches rows against a parsed filter. Component terms are resolved once
// against the registry, so matching an entity costs a lookup per resolved
// type. The filter and registry must outlive the matcher.
class OutlinerFilterMatcher {
  public:
    OutlinerFilterMatcher(const OutlinerFilter& filter, const ComponentTypeRegistry& types);

    // An entity row: every term matches its label or the entity.
    bool matchesEntity(const Scene& scene, Entity entity, std::string_view label) const;
    // Any other row: only name terms can match it.
    bool matchesLabel(std::string_view label) const;

  private:
    bool termMatches(size_t term, const Scene& scene, Entity entity, std::string_view label) const;

    const OutlinerFilter& m_filter;
    // Per term: the component types a Component term names.
    std::vector<std::vector<const ComponentType*>> m_components;
};

// The label an entity row shows: its name, or "Unnamed Entity".
std::string outlinerEntityLabel(const Scene& scene, Entity entity);

// The ids of every entity in the scene that matches filter (none for an empty
// filter), ascending; Select All Matching selects them.
std::vector<SceneEntityId> findOutlinerMatches(const Scene& scene, const OutlinerFilter& filter,
                                               const ComponentTypeRegistry& types);
