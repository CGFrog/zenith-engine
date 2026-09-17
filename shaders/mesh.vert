#version 450

layout(set = 0, binding = 0) uniform CameraData {
    mat4 viewProjection;
} camera;

layout(push_constant) uniform DrawData {
    mat4 model;
    vec4 baseColor;
} drawData;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 0) out vec3 worldNormal;
layout(location = 1) out vec4 baseColor;

void main() {
    gl_Position = camera.viewProjection * drawData.model * vec4(inPosition, 1.0);
    worldNormal = normalize(transpose(inverse(mat3(drawData.model))) * inNormal);
    baseColor = drawData.baseColor;
}
