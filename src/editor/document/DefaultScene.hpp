#pragma once

#include "engine/scene/Scene.hpp"

#include <filesystem>

class AssetManager;

// Project-relative model used for the starter cube.
inline const std::filesystem::path defaultCubeModel = std::filesystem::path("primitives") / "cube.gltf";

// Objects the Add menu (Shift A) creates at the world origin.
Entity addSun(Scene& scene);
Entity addEnvironment(Scene& scene);
Entity addFog(Scene& scene);
Entity addEmpty(Scene& scene);
// A camera that is primary only when no camera in the scene already is, so
// adding one never steals the game's view.
Entity addCamera(Scene& scene);
// Loads assetRoot/defaultCubeModel; cube is set only on success.
SceneResult addCube(Scene& scene, AssetManager& assets, const std::filesystem::path& assetRoot, Entity& cube);

// Adds the editor's starting content to an empty scene: a sun, a sky
// environment, and a cube at the origin. The lights are added even when the
// cube cannot be loaded; the returned error then describes the missing cube.
SceneResult populateDefaultScene(Scene& scene, AssetManager& assets, const std::filesystem::path& assetRoot);
