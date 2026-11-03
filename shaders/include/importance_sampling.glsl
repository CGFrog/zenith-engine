// Low-discrepancy GGX importance sampling for the environment prefilter and
// the BRDF table. Include constants.glsl first.
#ifndef ZENITH_IMPORTANCE_SAMPLING_GLSL
#define ZENITH_IMPORTANCE_SAMPLING_GLSL

float radicalInverse(uint bits) {
    return float(bitfieldReverse(bits)) * 2.3283064365386963e-10;
}

// GGX half vector in tangent space (+Z is the normal) for low-discrepancy sample i.
vec3 importanceSampleGgx(uint index, uint count, float roughness) {
    float alpha = roughness * roughness;
    float phi = 2.0 * PI * float(index) / float(count);
    float xi = radicalInverse(index);
    float cosTheta = sqrt((1.0 - xi) / (1.0 + (alpha * alpha - 1.0) * xi));
    float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
    return vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

#endif
