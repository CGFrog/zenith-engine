// The material record and its lobe flags.
// Mirrors MaterialRecord and material_lobes in
// engine/graphics/MaterialRecord.hpp (std430); change both together.
#ifndef ZENITH_MATERIAL_GLSL
#define ZENITH_MATERIAL_GLSL

struct MaterialRecord {
    vec4 baseColor;
    vec4 factors;  // metallic, roughness, normal scale, occlusion strength
    vec4 emissive; // rgb radiance; w unused
    vec4 surface;  // alpha cutoff, alpha mode (0 opaque, 1 mask, 2 blend), ior, transmission
    uvec4 textures; // bindless slots: base color, normal, metallic-roughness, occlusion
    uvec4 extraTextures; // emissive slot, lobe flags (LOBE_*), transmission slot, unused
    vec4 clearcoat; // factor, perceptual roughness, normal scale, unused
    vec4 sheen;     // linear rgb color, perceptual roughness
    vec4 specular;  // linear rgb color, factor
    vec4 volume;    // attenuation coefficient per world unit (rgb), thickness
    uvec4 lobeTextures;  // bindless slots: clearcoat, clearcoat roughness, clearcoat normal, sheen color
    uvec4 layerTextures; // bindless slots: sheen roughness, specular, specular color; w unused
    vec4 water;          // wave length, steepness, speed, shore fade (LOBE_WATER)
    uvec4 terrainAlbedo; // terrain layers' bindless slots (LOBE_TERRAIN)
    uvec4 terrainNormal;
    uvec4 terrainArm;
    vec4 terrainTileSize; // metres per repeat of each layer
};

const uint LOBE_CLEARCOAT = 1u;
const uint LOBE_SHEEN = 2u;
const uint LOBE_TRANSMISSION = 4u;
const uint LOBE_SPECULAR_TEXTURES = 8u;
const uint LOBE_VOLUME = 16u;
const uint LOBE_WATER = 32u;
const uint LOBE_TERRAIN = 64u;

#endif
