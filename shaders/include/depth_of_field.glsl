// Push constants and the circle of confusion shared by the depth of field
// passes. Mirrors DepthOfFieldSettingsBlock in
// engine/graphics/passes/DepthOfFieldPass.cpp.
#ifndef ZENITH_DEPTH_OF_FIELD_GLSL
#define ZENITH_DEPTH_OF_FIELD_GLSL

layout(push_constant) uniform DepthOfFieldSettings {
    vec4 fullSize;      // blurred-image width, height, 1 / width, 1 / height
    vec4 gatherSize;    // gather width, height, 1 / width, 1 / height
    vec2 gatherUvScale; // gather extent / gather image extent
    vec2 depthSize;     // depth (render) extent in pixels
    float cocScale;
    float focusDistance; // meters
    float maximumRadius; // full-resolution pixels
    float depthScale;
    float depthBias;
    float ringSpacing;
} settings;

// Signed circle of confusion radius (full-resolution pixels) at a
// full-resolution pixel; motionDepth holds reversed-Z depth in w.
float circleOfConfusion(sampler2D motionDepth, ivec2 pixel) {
    vec2 uv = (vec2(pixel) + 0.5) * settings.fullSize.zw;
    ivec2 depthTexel = clamp(ivec2(uv * settings.depthSize), ivec2(0), ivec2(settings.depthSize) - 1);
    float depth = texelFetch(motionDepth, depthTexel, 0).w;
    float viewDistance = settings.depthBias / (depth + settings.depthScale);
    float radius = settings.cocScale * (1.0 - settings.focusDistance / max(viewDistance, 1e-6));
    return clamp(radius, -settings.maximumRadius, settings.maximumRadius);
}

#endif
