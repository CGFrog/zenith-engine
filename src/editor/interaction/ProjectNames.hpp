#pragma once

#include "engine/physics/PhysicsLayers.hpp"

#include <string>
#include <string_view>
#include <vector>

// The names a project declares that inspectors offer as choices: its physics
// layers (for fields with FieldPresentation::nameList physicsLayerNameList)
// and its tags (for TagsComponent). The session keeps them current with the
// project file, and the Project Settings window changes them.
struct EditorProjectNames {
    std::vector<std::string> physicsLayers;
    std::vector<std::string> tags;

    // The list a field's nameList names, or null for an unknown list.
    const std::vector<std::string>* list(std::string_view listName) const {
        if (listName == physicsLayerNameList) {
            return &physicsLayers;
        }
        return nullptr;
    }
};
