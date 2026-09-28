#version 450

layout(set = 1, binding = 0) uniform sampler2D baseColorMap;

layout(push_constant) uniform ShadowDrawData {
    vec4 settings;
} drawData;

layout(location = 0) in vec2 texCoord;

void main() {
    if (drawData.settings.y > 0.5 && texture(baseColorMap, texCoord).a < drawData.settings.z) {
        discard;
    }
}
