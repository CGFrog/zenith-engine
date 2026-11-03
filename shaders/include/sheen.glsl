// Charlie sheen distribution and visibility (KHR_materials_sheen), shared by
// the shading in mesh.frag and the sheen term of env_brdf_lut.comp.
// Mirrors the functions of the same names in
// engine/graphics/EnvironmentLighting.cpp. Include constants.glsl first.
#ifndef ZENITH_SHEEN_GLSL
#define ZENITH_SHEEN_GLSL

float charlieDistribution(float nDotH, float alpha) {
    float inverseAlpha = 1.0 / alpha;
    float sinSquared = max(1.0 - nDotH * nDotH, 0.0);
    return (2.0 + inverseAlpha) * pow(sinSquared, 0.5 * inverseAlpha) / (2.0 * PI);
}

float sheenLambdaHelper(float x, float alpha) {
    float oneMinusAlphaSquared = (1.0 - alpha) * (1.0 - alpha);
    float a = mix(21.5473, 25.3245, oneMinusAlphaSquared);
    float b = mix(3.82987, 3.32435, oneMinusAlphaSquared);
    float c = mix(0.19823, 0.16801, oneMinusAlphaSquared);
    float d = mix(-1.97760, -1.27393, oneMinusAlphaSquared);
    float e = mix(-4.32054, -4.85967, oneMinusAlphaSquared);
    return a / (1.0 + b * pow(x, c)) + d * x + e;
}

float sheenLambda(float cosine, float alpha) {
    if (abs(cosine) < 0.5) {
        return exp(sheenLambdaHelper(cosine, alpha));
    }
    return exp(2.0 * sheenLambdaHelper(0.5, alpha) - sheenLambdaHelper(1.0 - cosine, alpha));
}

float sheenVisibility(float nDotL, float nDotV, float alpha) {
    float denominator = (1.0 + sheenLambda(nDotV, alpha) + sheenLambda(nDotL, alpha)) * (4.0 * nDotV * nDotL);
    return clamp(1.0 / max(denominator, 1e-6), 0.0, 1.0);
}

#endif
