// The camera uniform block every scene-drawing shader binds at set 0,
// binding 0 of MeshPass's camera set.
// Mirrors mesh_pass_detail::CameraData in
// engine/graphics/passes/MeshPassInternal.hpp (std140); change both together.
#ifndef ZENITH_CAMERA_GLSL
#define ZENITH_CAMERA_GLSL

layout(set = 0, binding = 0) uniform CameraData {
    mat4 viewProjection;
    mat4 previousViewProjection;
    mat4 lightViewProjections[3];
    vec4 cameraPosition;
    vec4 lightDirection; // xyz toward the light, w intensity
    vec4 lightColor;
    vec4 environmentSky;
    vec4 environmentGround;
    vec4 shadowSettings;
    vec4 cascadeSplits;
    vec4 renderingSettings;
    vec4 environmentSpecular[6];
    // Clustered local lighting.
    vec4 clusterDepth;     // xy: slice log scale, bias; zw: view depth = z / (depth + w)
    vec4 clusterTileScale; // xy: cluster columns, rows per pixel
    uvec4 clusterGrid;     // columns, rows, depth slices, local light count
    // x: image-based lighting, y: probe weight, z: specular mip of roughness 1,
    // w: ambient-occlusion texels per render pixel
    vec4 environmentParams;
    // Reflection probes (U9): x: 1 when per-pixel probe blending is active,
    // y: resident probe count.
    vec4 reflectionProbeParams;
    // Volumetric fog (U10); see VolumetricFog.hpp. x: 1 composites the
    // integrated fog volume; y, z: froxel slice = log(view depth) * y + z;
    // w: froxel depth slices.
    vec4 fogVolume;
    // xy: integrated-volume uv per render pixel (1 / internal render extent).
    vec4 fogScreen;
    // Irradiance volume (U6).
    vec4 irradianceVolumeMinimum;  // xyz: world minimum corner, w: intensity
    vec4 irradianceVolumeCellSize; // xyz: world probe spacing, w: edge blend distance
    uvec4 irradianceVolumeCounts;  // xyz: probes per axis, w: 1 when a volume is bound
    vec4 irradianceVolumeBias;     // x: normal bias, y: view bias
    // Temporal resolve: NDC jitter applied to the current (xy) and previous (zw)
    // projections, removed from motion vectors.
    vec4 temporalJitter;
    // x: seconds of surface animation, y: 1 when sceneDepth holds this
    // frame's opaque depth.
    vec4 surfaceAnimation;
} camera;

#endif
