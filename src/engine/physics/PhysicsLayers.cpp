#include "PhysicsLayers.hpp"

#include <algorithm>

namespace {
uint32_t bit(PhysicsLayer layer) {
    return layer.index < maxPhysicsLayers ? 1u << layer.index : 0u;
}
} // namespace

PhysicsLayerTable PhysicsLayerTable::defaults() {
    PhysicsLayerTable table;
    const PhysicsLayer all[] = {PhysicsLayer::Static, PhysicsLayer::Dynamic, PhysicsLayer::Player};
    for (const char* name : {"Static", "Dynamic", "Player"}) {
        table.names.emplace_back(name);
    }
    for (const PhysicsLayer first : all) {
        for (const PhysicsLayer second : all) {
            table.setCollides(first, second, first != PhysicsLayer::Static || second != PhysicsLayer::Static);
        }
    }
    return table;
}

size_t PhysicsLayerTable::size() const {
    return names.size();
}

bool PhysicsLayerTable::contains(PhysicsLayer layer) const {
    return layer.index < names.size();
}

bool PhysicsLayerTable::collides(PhysicsLayer first, PhysicsLayer second) const {
    return contains(first) && contains(second) && (collidesWith[first.index] & bit(second)) != 0;
}

void PhysicsLayerTable::setCollides(PhysicsLayer first, PhysicsLayer second, bool collide) {
    if (first.index >= maxPhysicsLayers || second.index >= maxPhysicsLayers) {
        return;
    }
    if (collide) {
        collidesWith[first.index] |= bit(second);
        collidesWith[second.index] |= bit(first);
    } else {
        collidesWith[first.index] &= ~bit(second);
        collidesWith[second.index] &= ~bit(first);
    }
}

std::optional<PhysicsLayer> PhysicsLayerTable::find(std::string_view name) const {
    const auto found = std::ranges::find(names, name);
    if (found == names.end()) {
        return std::nullopt;
    }
    return PhysicsLayer{static_cast<uint32_t>(found - names.begin())};
}

std::optional<PhysicsLayer> PhysicsLayerTable::add(std::string name) {
    if (name.empty() || names.size() >= maxPhysicsLayers || find(name)) {
        return std::nullopt;
    }
    names.push_back(std::move(name));
    return PhysicsLayer{static_cast<uint32_t>(names.size() - 1)};
}

uint32_t PhysicsLayerTable::mask(PhysicsLayer layer) const {
    return contains(layer) ? collidesWith[layer.index] : 0u;
}

std::string PhysicsLayerTable::validate() const {
    if (names.empty() || names.size() > maxPhysicsLayers) {
        return "A physics layer table needs 1 to " + std::to_string(maxPhysicsLayers) + " layers";
    }
    for (size_t index = 0; index < names.size(); ++index) {
        if (names[index].empty()) {
            return "Physics layer " + std::to_string(index) + " has no name";
        }
        if (std::find(names.begin(), names.begin() + static_cast<std::ptrdiff_t>(index), names[index]) !=
            names.begin() + static_cast<std::ptrdiff_t>(index)) {
            return "Physics layer name '" + names[index] + "' is used twice";
        }
    }
    const uint32_t existing = names.size() == maxPhysicsLayers ? ~0u : (1u << names.size()) - 1u;
    for (uint32_t first = 0; first < maxPhysicsLayers; ++first) {
        if ((collidesWith[first] & ~existing) != 0 || (first >= names.size() && collidesWith[first] != 0)) {
            return "The physics collision matrix names a layer the table does not have";
        }
        for (uint32_t second = 0; second < maxPhysicsLayers; ++second) {
            const bool forward = (collidesWith[first] & (1u << second)) != 0;
            const bool backward = (collidesWith[second] & (1u << first)) != 0;
            if (forward != backward) {
                return "The physics collision matrix is not symmetric";
            }
        }
    }
    return {};
}

uint32_t physicsLayerMask(std::initializer_list<PhysicsLayer> layers) {
    uint32_t mask = 0;
    for (const PhysicsLayer layer : layers) {
        mask |= bit(layer);
    }
    return mask;
}
