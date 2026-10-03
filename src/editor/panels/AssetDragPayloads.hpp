#pragma once

// ImGui drag-and-drop payload types the Asset Browser offers. Each payload is
// the dragged file's absolute path as a null-terminated narrow string.

// A model file (.gltf, .glb): viewports and the inspector place it.
inline constexpr const char* modelAssetPayload = "ZENITH_MODEL_ASSET";
// A prefab file (.prefab): viewports and the outliner instantiate it.
inline constexpr const char* prefabAssetPayload = "ZENITH_PREFAB_ASSET";
