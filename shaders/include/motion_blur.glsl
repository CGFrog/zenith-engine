// Push constants and the per-pixel blur velocity shared by the motion blur
// passes. Mirrors MotionBlurSettings in
// engine/graphics/passes/MotionBlurPass.cpp.
#ifndef ZENITH_MOTION_BLUR_GLSL
#define ZENITH_MOTION_BLUR_GLSL

layout(push_constant) uniform MotionBlurSettings {
    // Unjittered current clip space to the previous frame's unjittered clip
    // space; reprojects pixels that no surface wrote motion for.
    mat4 currentToPreviousClip;
    vec4 colorRegion; // width, height, 1 / width, 1 / height
    vec2 motionRegion; // motion texels holding this frame
    float velocityScale;
    float radiusPixels;
    ivec2 tileCount;
    int tileSize;
    int sampleCount;
} settings;

// Blur velocity (xy, colour-region pixels) and relative view distance (z).
vec3 velocityAndDistance(sampler2D motionVectors, ivec2 pixel) {
    vec2 uv = (vec2(pixel) + 0.5) * settings.colorRegion.zw;
    ivec2 motionTexel = clamp(ivec2(uv * settings.motionRegion), ivec2(0), ivec2(settings.motionRegion) - 1);
    vec4 motionDepth = texelFetch(motionVectors, motionTexel, 0);
    vec2 motion = motionDepth.xy;
    if (motionDepth.w <= 0.0) {
        // No surface wrote motion here; the far plane moves with the camera.
        vec4 previousClip = settings.currentToPreviousClip * vec4(uv * 2.0 - 1.0, 0.0, 1.0);
        motion = previousClip.w > 0.0 ? uv - (previousClip.xy / previousClip.w * 0.5 + 0.5) : vec2(0.0);
    }
    vec2 velocity = 0.5 * motion * settings.colorRegion.xy * settings.velocityScale;
    float velocityLength = length(velocity);
    if (velocityLength > settings.radiusPixels) {
        velocity *= settings.radiusPixels / velocityLength;
    }
    // Reversed-Z depth is proportional to 1 / view distance.
    return vec3(velocity, 1.0 / max(motionDepth.w, 1e-7));
}

#endif
