#version 450

// AMD FidelityFX Super Resolution 1.0, RCAS (robust contrast-adaptive
// sharpening) with its noise attenuation: a GLSL port of FsrRcasF from
// ffx_fsr1.h. The CPU reference (rcasSharpen in
// src/engine/graphics/SpatialUpscale.cpp) mirrors this shader; keep them in
// sync. It runs as the fullscreen draw into the output so any renderable
// output format works (swapchain images need not support storage).
// Reciprocals of flat ranges are clamped instead of relying on the
// approximate reciprocal of zero being finite.
//
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.

// Gamma-2 encoded EASU output at output resolution.
layout(set = 0, binding = 0) uniform sampler2D upscaledScene;

// Mirrors RcasSettings in SpatialUpscalePass.cpp.
layout(push_constant) uniform RcasSettings {
    ivec2 outputMaximum;
    // 2^-stops of attenuation (rcasLobeScale).
    float lobeScale;
    float padding;
} settings;

layout(location = 0) out vec4 outColor;

// The largest negative lobe that can never clip.
const float rcasLimit = 0.25 - 1.0 / 16.0;
const float minimumDivisor = 1.0e-8;

vec3 load(ivec2 texel) { return texelFetch(upscaledScene, clamp(texel, ivec2(0), settings.outputMaximum), 0).rgb; }

float approximateLuma(vec3 color) { return color.b * 0.5 + (color.r * 0.5 + color.g); }

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    //    n
    //  w c e
    //    s
    vec3 north = load(pixel + ivec2(0, -1));
    vec3 west = load(pixel + ivec2(-1, 0));
    vec3 center = load(pixel);
    vec3 east = load(pixel + ivec2(1, 0));
    vec3 south = load(pixel + ivec2(0, 1));
    float northLuma = approximateLuma(north);
    float westLuma = approximateLuma(west);
    float centerLuma = approximateLuma(center);
    float eastLuma = approximateLuma(east);
    float southLuma = approximateLuma(south);

    // Noise detection: a center that stands out from its ring is attenuated.
    float lumaRange = max(max(max(northLuma, westLuma), max(centerLuma, eastLuma)), southLuma) -
                      min(min(min(northLuma, westLuma), min(centerLuma, eastLuma)), southLuma);
    float noise = 0.25 * (northLuma + westLuma + eastLuma + southLuma) - centerLuma;
    noise = clamp(abs(noise) / max(lumaRange, minimumDivisor), 0.0, 1.0);
    noise = 1.0 - 0.5 * noise;

    vec3 ringMinimum = min(min(north, west), min(east, south));
    vec3 ringMaximum = max(max(north, west), max(east, south));
    vec3 hitMinimum = min(ringMinimum, center) / max(4.0 * ringMaximum, vec3(minimumDivisor));
    vec3 hitMaximum = (1.0 - max(ringMaximum, center)) / min(4.0 * ringMinimum - 4.0, vec3(-minimumDivisor));
    vec3 channelLobe = max(-hitMinimum, hitMaximum);
    float lobe = max(-rcasLimit, min(max(channelLobe.r, max(channelLobe.g, channelLobe.b)), 0.0));
    lobe *= settings.lobeScale * noise;
    vec3 sharpened = (lobe * (north + west + east + south) + center) / (4.0 * lobe + 1.0);
    // Back to display-linear; an sRGB output re-encodes on store, as post.frag's output does.
    sharpened = clamp(sharpened, 0.0, 1.0);
    outColor = vec4(sharpened * sharpened, 1.0);
}
