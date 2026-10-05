#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

class ComponentTypeRegistry;

// Where an entity reference was found inside a scene document entity.
struct EntityReferenceSite {
    // Component type key, for example "follow".
    std::string_view component;
    // RFC 6901 pointer to the value inside the component's document object,
    // for example "/target" or "/settings/target".
    std::string_view field;
    // Whether the value may be none (written as null). Reflected entity
    // reference fields may; the instance roots of model nodes and skins may
    // not.
    bool nullable{true};
};

// The id a copied reference becomes: nullopt keeps the value, zero makes a
// nullable reference none, anything else replaces it.
using EntityReferenceMapping = std::function<std::optional<uint64_t>(uint64_t id, const EntityReferenceSite& site)>;

// Rewrites the entity references held by the component objects of one scene
// document entity ({component key: document object}) through mapping, so a
// copied set of entities can point at its copies. References are found
// generically: every reflected FieldKind::EntityReference field of the
// registered component types (including those nested in Struct fields), plus
// the instance roots of the built-in custom-codec types model_node and skin.
// Unregistered keys, missing values, and none (null or zero) are skipped.
void remapEntityReferences(nlohmann::json& components, const ComponentTypeRegistry& types,
                           const EntityReferenceMapping& mapping);

// Rewrites a scene document entity's optional "children" order (the ids of
// its children in sibling order, see SceneDocument) through mapping, which
// returns an id's copy or nullopt for a child that was not copied. Ids
// without a copy are dropped, and an order left empty is removed.
void remapChildOrder(nlohmann::json& entity, const std::function<std::optional<uint64_t>(uint64_t id)>& mapping);
