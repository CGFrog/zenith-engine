// Push constants of the debug line pipeline (DebugDrawPass): world-space
// segments drawn over the tone-mapped view. Mirrors DebugLinePushConstants in
// engine/graphics/passes/DebugDrawPass.cpp.
#ifndef ZENITH_DEBUG_LINE_GLSL
#define ZENITH_DEBUG_LINE_GLSL

layout(push_constant) uniform DebugLineSettings {
    // Unjittered view-projection of the view's latest recording.
    mat4 viewProjection;
    // x, y: terms of the reversed-Z scene depth, distance = y / (depth + x);
    // z: relative distance by which a line may lie behind the scene and still
    // show; w: 1 draws over every surface.
    vec4 depthTerms;
    // xy: render extent / output extent, mapping output pixels to scene depth
    // texels; z: 1 when the target encodes sRGB on store.
    vec4 target;
} settings;

#endif
