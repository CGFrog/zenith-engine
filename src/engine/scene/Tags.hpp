#pragma once

#include "engine/scene/Scene.hpp"

#include <string>
#include <string_view>
#include <vector>

// Names an entity carries for game code to find it by: "enemy",
// "pickup.health". A project lists its tags in its manifest
// (ProjectConfig::tags) for the editor's picker, but any name works at
// runtime. Saved as {"tags": ["enemy", ...]} under the "tags" key.
struct TagsComponent {
    std::vector<std::string> tags;

    bool has(std::string_view tag) const;
    // Adds tag unless the entity has it; returns whether it was added.
    bool add(std::string tag);
    // Returns whether the entity had tag.
    bool remove(std::string_view tag);
};

// Whether entity has a TagsComponent listing tag.
bool hasTag(const Scene& scene, Entity entity, std::string_view tag);
// Every entity with tag, in scene id order so results are deterministic.
std::vector<Entity> findEntitiesWithTag(const Scene& scene, std::string_view tag);
