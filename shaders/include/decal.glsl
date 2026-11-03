// One clustered decal and the decal limits.
// Mirrors GpuDecal and the texture flags in
// engine/graphics/passes/DecalClusterPass.hpp (std430), and maximumDecals,
// maximumDecalsPerCluster and decalDepthFadeFraction in
// engine/graphics/Decals.hpp.
#ifndef ZENITH_DECAL_GLSL
#define ZENITH_DECAL_GLSL

const uint MAXIMUM_DECALS = 1024;
const uint MAXIMUM_DECALS_PER_CLUSTER = 31;
// Per cluster: a count, then up to MAXIMUM_DECALS_PER_CLUSTER record indices.
const uint DECAL_CLUSTER_STRIDE = MAXIMUM_DECALS_PER_CLUSTER + 1;
const uint DECAL_HAS_ALBEDO = 1u;
const uint DECAL_HAS_NORMAL = 2u;
const float DECAL_DEPTH_FADE_FRACTION = 0.25;

struct DecalRecord {
    vec4 worldToDecal[3]; // rows of the world-to-decal-space transform
    vec4 tintOpacity;     // linear tint, opacity
    vec4 surface;         // roughness, roughness strength, metallic, metallic strength
    vec4 blend;           // base color strength, normal strength, angle fade scale, angle fade bias
    uvec4 textures;       // albedo slot (sRGB), normal slot (linear), DECAL_HAS_* flags
    vec4 viewMinimum;     // view-space bounds, used by the cluster pass
    vec4 viewMaximum;
};

#endif
