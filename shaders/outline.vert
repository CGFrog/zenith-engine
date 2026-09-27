#version 450

layout(set = 0, binding = 0) uniform CameraData {
    mat4 viewProjection;
    mat4 lightViewProjections[3];
    vec4 cameraPosition;
    vec4 lightDirection;
    vec4 lightColor;
    vec4 environmentSky;
    vec4 environmentGround;
    vec4 shadowSettings;
    vec4 cascadeSplits;
    vec4 renderingSettings;
} camera;

layout(set = 0, binding = 2, std430) readonly buffer ObjectBuffer {
    mat4 models[];
} objects;

layout(push_constant) uniform OutlineSettings {
    vec4 localCenterAndWidth;
} outline;

layout(location = 0) in vec3 inPosition;

void main() {
    mat4 model = objects.models[gl_InstanceIndex];
    vec4 clipPosition = camera.viewProjection * model * vec4(inPosition, 1.0);
    vec4 clipCenter = camera.viewProjection * model * vec4(outline.localCenterAndWidth.xyz, 1.0);
    vec2 positionNdc = clipPosition.xy / clipPosition.w;
    vec2 centerNdc = clipCenter.xy / clipCenter.w;
    vec2 fromCenter = positionNdc - centerNdc;
    if (dot(fromCenter, fromCenter) > 0.0000001) {
        vec2 pixelSizeNdc = camera.renderingSettings.yz * 2.0;
        positionNdc += normalize(fromCenter) * pixelSizeNdc * outline.localCenterAndWidth.w;
        clipPosition.xy = positionNdc * clipPosition.w;
    }
    gl_Position = clipPosition;
}
