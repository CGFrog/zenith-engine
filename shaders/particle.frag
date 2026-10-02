#version 450
#extension GL_EXT_nonuniform_qualifier : require

// Shades one particle quad: sprite times the particle's radiance, faded
// against the opaque depth behind it (soft particles), with the volumetric fog
// in front of it, written as premultiplied color. Additive particles write
// zero coverage so they only add light. The motion target's reactive channel
// (z) takes the strongest particle coverage so the temporal resolve trusts
// history less where particles move. See ParticlePass.cpp.

const uint FLAG_ADDITIVE = 1u;
const uint TEXTURE_SLOT_SHIFT = 8u;
const uint TEXTURE_SLOT_MASK = 0xffu;

// Prefix of CameraData in mesh.frag (std140) through the fog members.
layout(set = 0, binding = 0) uniform CameraData {
    mat4 viewProjection;
    mat4 previousViewProjection;
    mat4 lightViewProjections[3];
    vec4 cameraPosition;
    vec4 lightDirection;
    vec4 lightColor;
    vec4 environmentSky;
    vec4 environmentGround;
    vec4 shadowSettings;
    vec4 cascadeSplits;
    vec4 renderingSettings;
    vec4 environmentSpecular[6];
    vec4 clusterDepth; // zw: view depth = z / (depth + w)
    vec4 clusterTileScale;
    uvec4 clusterGrid;
    vec4 environmentParams;
    vec4 reflectionProbeParams;
    vec4 fogVolume; // x: fog active; y, z: froxel slice = log(depth) * y + z; w: slices
    vec4 fogScreen; // xy: integrated-volume uv per render pixel
} camera;

layout(set = 0, binding = 18) uniform sampler3D volumetricFog;

// Opaque scene depth (reversed Z), read-only while particles draw.
layout(set = 1, binding = 2) uniform sampler2D sceneDepth;
// Slot 0 is unused: it selects the procedural soft round sprite.
layout(set = 1, binding = 3) uniform sampler2D sprites[8];

layout(location = 0) in vec2 spriteUv;
layout(location = 1) in vec4 particleRadiance;
layout(location = 2) flat in uint particleFlags;
layout(location = 3) flat in float softDistance;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outMotionReactive;

float viewDepth(float depth) {
    return camera.clusterDepth.z / (depth + camera.clusterDepth.w);
}

// Mirrors applyVolumetricFog in mesh.frag: rgb in-scattered light between the
// eye and this fragment, a transmittance.
vec4 volumetricFogAt(float depth) {
    if (camera.fogVolume.x < 0.5) {
        return vec4(0.0, 0.0, 0.0, 1.0);
    }
    float slices = camera.fogVolume.w;
    float slice = clamp(log(depth) * camera.fogVolume.y + camera.fogVolume.z, 0.0, slices);
    float w = clamp((slice - 0.5) / slices, 0.5 / slices, (slices - 0.5) / slices);
    return textureLod(volumetricFog, vec3(gl_FragCoord.xy * camera.fogScreen.xy, w), 0.0);
}

void main() {
    uint slot = (particleFlags >> TEXTURE_SLOT_SHIFT) & TEXTURE_SLOT_MASK;
    vec4 sprite;
    if (slot == 0u) {
        float radius = length(spriteUv * 2.0 - 1.0);
        float falloff = clamp(1.0 - radius, 0.0, 1.0);
        sprite = vec4(1.0, 1.0, 1.0, falloff * falloff * (3.0 - 2.0 * falloff));
    } else {
        sprite = texture(sprites[nonuniformEXT(slot)], spriteUv);
    }
    float particleDepth = viewDepth(gl_FragCoord.z);
    float soft = 1.0;
    if (softDistance > 0.0) {
        float opaqueDepth = viewDepth(texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r);
        soft = clamp((opaqueDepth - particleDepth) / softDistance, 0.0, 1.0);
    }
    float coverage = clamp(particleRadiance.a * sprite.a * soft, 0.0, 1.0);
    if (coverage < 1.0 / 1024.0) {
        discard;
    }
    vec3 color = particleRadiance.rgb * sprite.rgb;
    vec4 fog = volumetricFogAt(particleDepth);
    bool additive = (particleFlags & FLAG_ADDITIVE) != 0u;
    if (additive) {
        outColor = vec4(color * fog.a * coverage, 0.0);
    } else {
        outColor = vec4((color * fog.a + fog.rgb) * coverage, coverage);
    }
    float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float reactive = additive ? clamp(brightness * coverage, 0.0, 1.0) : coverage;
    outMotionReactive = vec4(0.0, 0.0, reactive, 0.0);
}
