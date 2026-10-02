#pragma once

#include "engine/scene/Scene.hpp"

#include <cstdint>

class AssetManager;

// Builds the reusable, document-safe portion of the demo. Gameplay-only
// entities such as the player are composed by GameplayScene. Benchmark lights
// are laid out on a grid above the benchmark objects.
SceneResult populateDemoScene(Scene& scene, AssetManager& assets, uint32_t benchmarkObjectCount = 0,
                              uint32_t benchmarkLightCount = 0);
