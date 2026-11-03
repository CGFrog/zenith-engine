#version 450
#extension GL_GOOGLE_include_directive : require

#include "camera.glsl"

#include "render_object.glsl"
layout(set = 0, binding = 2, std430) readonly buffer ObjectBuffer {
    RenderObject values[];
} objects;
layout(set = 0, binding = 3, std430) readonly buffer SkinPalettes {
    mat4 joints[];
} skinPalettes;

layout(push_constant) uniform OutlineSettings {
    vec4 localCenterAndWidth;
} outline;

layout(location = 0) in vec3 inPosition;
layout(location = 4) in uvec4 inJoints;
layout(location = 5) in vec4 inWeights;

void main() {
    RenderObject object = objects.values[gl_InstanceIndex];
    mat4 skin = mat4(1.0);
    if (object.skin.y > 0 && dot(inWeights, vec4(1.0)) > 0.0001) {
        uvec4 joint = min(inJoints, uvec4(object.skin.y - 1));
        skin = skinPalettes.joints[object.skin.x + joint.x] * inWeights.x +
               skinPalettes.joints[object.skin.x + joint.y] * inWeights.y +
               skinPalettes.joints[object.skin.x + joint.z] * inWeights.z +
               skinPalettes.joints[object.skin.x + joint.w] * inWeights.w;
    }
    mat4 model = object.model;
    vec4 clipPosition = camera.viewProjection * model * skin * vec4(inPosition, 1.0);
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
