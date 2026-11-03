#version 450
#extension GL_GOOGLE_include_directive : require

// Picking id buffer (IdBufferPass): the object's surface in its animated pose,
// placed by a per-draw model-view-projection so the pass follows the view's
// output pixels rather than its jittered internal resolution.

layout(set = 0, binding = 3, std430) readonly buffer SkinPalettes {
    mat4 joints[];
} skinPalettes;

#include "id_draw.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 4) in uvec4 inJoints;
layout(location = 5) in vec4 inWeights;

void main() {
    mat4 skin = mat4(1.0);
    if (draw.object.y > 0 && dot(inWeights, vec4(1.0)) > 0.0001) {
        uvec4 joint = min(inJoints, uvec4(draw.object.y - 1));
        skin = skinPalettes.joints[draw.object.x + joint.x] * inWeights.x +
               skinPalettes.joints[draw.object.x + joint.y] * inWeights.y +
               skinPalettes.joints[draw.object.x + joint.z] * inWeights.z +
               skinPalettes.joints[draw.object.x + joint.w] * inWeights.w;
    }
    gl_Position = draw.modelViewProjection * skin * vec4(inPosition, 1.0);
}
