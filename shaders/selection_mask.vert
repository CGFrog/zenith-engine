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

layout(location = 0) in vec3 inPosition;

void main() {
    gl_Position = camera.viewProjection * objects.models[gl_InstanceIndex] * vec4(inPosition, 1.0);
}
