#pragma once

#include "engine/scene/Scene.hpp"

#include <cstdint>

class AssetManager;

// Builds the reusable, document-safe portion of the demo. Gameplay-only
// entities such as the player are composed by GameplayScene.
SceneResult populateDemoScene(Scene& scene, AssetManager& assets, uint32_t benchmarkObjectCount = 0);
