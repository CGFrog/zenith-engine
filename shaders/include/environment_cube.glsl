// Cube map face addressing shared by the environment compute shaders.
#ifndef ZENITH_ENVIRONMENT_CUBE_GLSL
#define ZENITH_ENVIRONMENT_CUBE_GLSL

// The world direction through uv ([0, 1]^2) of a cube face, in Vulkan's
// face order (+X, -X, +Y, -Y, +Z, -Z).
vec3 cubeFaceDirection(uint face, vec2 uv) {
    vec2 c = uv * 2.0 - 1.0;
    vec3 direction;
    if (face == 0u) {
        direction = vec3(1.0, -c.y, -c.x);
    } else if (face == 1u) {
        direction = vec3(-1.0, -c.y, c.x);
    } else if (face == 2u) {
        direction = vec3(c.x, 1.0, c.y);
    } else if (face == 3u) {
        direction = vec3(c.x, -1.0, -c.y);
    } else if (face == 4u) {
        direction = vec3(c.x, -c.y, 1.0);
    } else {
        direction = vec3(-c.x, -c.y, -1.0);
    }
    return normalize(direction);
}

#endif
