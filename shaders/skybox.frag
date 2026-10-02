#version 450

// Prefix of CameraData in shaders/mesh.frag and MeshPass.cpp, through the fog fields.
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
    vec4 clusterDepth;
    vec4 clusterTileScale;
    uvec4 clusterGrid;
    vec4 environmentParams;
    // Reflection probes (U9); unused here but keeps the fog fields' offsets.
    vec4 reflectionProbeParams;
    // Volumetric fog (U10): x enables it, w is the froxel depth slice count.
    vec4 fogVolume;
    // xy: integrated-volume uv per render pixel.
    vec4 fogScreen;
} camera;

// Integrated volumetric fog; the sky lies beyond the volume, so it receives
// the fog accumulated through the last froxel slice.
layout(set = 0, binding = 18) uniform sampler3D volumetricFog;

layout(set = 1, binding = 0) uniform samplerCube environment;

layout(push_constant) uniform SkySettings {
    vec4 parameters;     // x: radiance scale, y: 1 draws the sun disc (analytic sky only)
    vec4 temporalJitter; // NDC jitter of the current (xy) and previous (zw) projections
} sky;

layout(location = 0) in vec4 farPoint;
layout(location = 1) in vec2 currentNdc;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outMotionReactive;

const float SUN_DISC_COSINE = 0.99985;
const float SUN_EDGE_COSINE = 0.9996;
const float SUN_DISC_SCALE = 8.0;

void main() {
    vec3 worldFar = farPoint.xyz / farPoint.w;
    vec3 direction = normalize(worldFar - camera.cameraPosition.xyz);
    vec3 radiance = textureLod(environment, direction, 0.0).rgb * sky.parameters.x;
    if (sky.parameters.y > 0.5) {
        float sunCosine = dot(direction, normalize(camera.lightDirection.xyz));
        float disc = smoothstep(SUN_EDGE_COSINE, SUN_DISC_COSINE, sunCosine);
        radiance += camera.lightColor.rgb * camera.lightDirection.w * SUN_DISC_SCALE * disc;
    }
    if (camera.fogVolume.x > 0.5) {
        float slices = camera.fogVolume.w;
        vec4 fog = textureLod(volumetricFog, vec3(gl_FragCoord.xy * camera.fogScreen.xy, (slices - 0.5) / slices), 0.0);
        radiance = radiance * fog.a + fog.rgb;
    }
    outColor = vec4(radiance, 0.0);

    // The background moves with the camera only; reproject its far-plane point.
    vec4 previousClip = camera.previousViewProjection * vec4(worldFar, 1.0);
    // Jitter is removed like in mesh.frag so motion carries camera movement only.
    vec2 currentUv = (currentNdc + sky.temporalJitter.xy) * 0.5 + 0.5;
    vec2 previousUv = (previousClip.xy / max(abs(previousClip.w), 0.00001) + sky.temporalJitter.zw) * 0.5 + 0.5;
    bool previousOffscreen = previousClip.w <= 0.0 || any(lessThan(previousUv, vec2(0.0))) ||
                             any(greaterThan(previousUv, vec2(1.0)));
    outMotionReactive = vec4(currentUv - previousUv, previousOffscreen ? 1.0 : 0.0, gl_FragCoord.z);
}
