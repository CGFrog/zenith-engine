#pragma once

// ImGui drag-and-drop payload types the Asset Browser offers. Each payload is
// the dragged file's absolute path as a null-terminated string: the model and
// prefab payloads in the narrow (native code page) encoding, the others in
// UTF-8 (generic_u8string).

// A model file (.gltf, .glb): viewports and the inspector place it.
inline constexpr const char* modelAssetPayload = "ZENITH_MODEL_ASSET";
// A prefab file (.prefab): viewports and the outliner instantiate it.
inline constexpr const char* prefabAssetPayload = "ZENITH_PREFAB_ASSET";
// An image file (.png, .jpg, .hdr, .ktx2): asset fields assign it.
inline constexpr const char* textureAssetPayload = "ZENITH_TEXTURE_ASSET";
// A sound file (.wav, .ogg): asset fields assign it.
inline constexpr const char* audioAssetPayload = "ZENITH_AUDIO_ASSET";
// A material file (.zmat): asset fields assign it.
inline constexpr const char* materialAssetPayload = "ZENITH_MATERIAL_ASSET";
