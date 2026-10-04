#pragma once

#include "PhysicsComponents.hpp"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The name list (FieldPresentation::nameList) that physics layer fields
// index: the project's layer names.
inline constexpr std::string_view physicsLayerNameList = "physics_layers";

// A project's collision layers and which of them collide. A layer's index is
// its position in names; a body names its layer by index
// (RigidBodyComponent::layer). Layers beyond names collide with nothing.
struct PhysicsLayerTable {
    std::vector<std::string> names;
    // Bit j of collidesWith[i] says layers i and j collide; kept symmetric.
    std::array<uint32_t, maxPhysicsLayers> collidesWith{};

    // Static, Dynamic, and Player, where everything collides except two
    // Static bodies.
    static PhysicsLayerTable defaults();

    size_t size() const;
    bool contains(PhysicsLayer layer) const;
    bool collides(PhysicsLayer first, PhysicsLayer second) const;
    // Sets both directions.
    void setCollides(PhysicsLayer first, PhysicsLayer second, bool collide);
    std::optional<PhysicsLayer> find(std::string_view name) const;
    // Appends a layer that collides with nothing yet; nullopt when the table
    // is full or the name is empty or taken.
    std::optional<PhysicsLayer> add(std::string name);
    // The mask of every layer layer collides with.
    uint32_t mask(PhysicsLayer layer) const;

    // Empty when valid: 1 to maxPhysicsLayers unique, non-empty names, and a
    // symmetric matrix that names only existing layers.
    std::string validate() const;

    bool operator==(const PhysicsLayerTable&) const = default;
};

// A query mask hitting exactly the given layers.
uint32_t physicsLayerMask(std::initializer_list<PhysicsLayer> layers);
