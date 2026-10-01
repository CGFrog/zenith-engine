#version 450

// Prefix of CameraData in shaders/mesh.frag and MeshPass.cpp.
layout(set = 0, binding = 0) uniform CameraData {
    mat4 viewProjection;
    mat4 previousViewProjection;
    mat4 lightViewProjections[3];
    vec4 cameraPosition;
    vec4 lightDirection; // xyz toward the light, w intensity
    vec4 lightColor;
} camera;

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
