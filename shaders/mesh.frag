#version 450

layout(location = 0) in vec3 worldNormal;
layout(location = 1) in vec4 baseColor;
layout(location = 0) out vec4 outColor;

void main() {
    const vec3 lightDirection = normalize(vec3(0.4, 0.8, 0.5));
    const float ambient = 0.2;
    float lighting = ambient + max(dot(normalize(worldNormal), lightDirection), 0.0) * (1.0 - ambient);
    outColor = vec4(baseColor.rgb * lighting, baseColor.a);
}
