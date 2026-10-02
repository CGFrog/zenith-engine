#include "IrradianceVolumeBaker.hpp"

#include "engine/assets/Assets.hpp"
#include "engine/graphics/EnvironmentLighting.hpp"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <thread>

namespace {
constexpr float pi = 3.14159265358979f;
// Ray origins move this far (world units) off surfaces to avoid self-hits.
constexpr float surfaceOffset = 0.001f;
// A probe whose rays hit single-sided back faces this often is inside geometry.
constexpr float invalidBackfaceFraction = 0.25f;
// Back-face hits count as this fraction of their distance in the depth
// moments, so geometry a probe sits inside occludes it strongly.
constexpr float backfaceDistanceScale = 0.2f;
// Cosine power that filters ray distances into octahedral depth texels.
constexpr float depthFilterSharpness = 20.0f;
// Resolution the environment map is box-filtered to before the bake samples it.
constexpr uint32_t skyWidth = 64;
constexpr uint32_t skyHeight = 32;
constexpr uint32_t bvhLeafTriangles = 4;

struct Ray {
    glm::vec3 origin{0.0f};
    glm::vec3 direction{0.0f, 1.0f, 0.0f};
};

struct Hit {
    float distance{std::numeric_limits<float>::infinity()};
    uint32_t triangle{0};
};

// Moller-Trumbore; returns the distance along the ray or a negative value.
float intersect(const Ray& ray, const IrradianceBakeTriangle& triangle) {
    const glm::vec3 edge1 = triangle.b - triangle.a;
    const glm::vec3 edge2 = triangle.c - triangle.a;
    const glm::vec3 p = glm::cross(ray.direction, edge2);
    const float determinant = glm::dot(edge1, p);
    if (std::abs(determinant) < 1e-12f) {
        return -1.0f;
    }
    const float inverse = 1.0f / determinant;
    const glm::vec3 t = ray.origin - triangle.a;
    const float u = glm::dot(t, p) * inverse;
    if (u < 0.0f || u > 1.0f) {
        return -1.0f;
    }
    const glm::vec3 q = glm::cross(t, edge1);
    const float v = glm::dot(ray.direction, q) * inverse;
    if (v < 0.0f || u + v > 1.0f) {
        return -1.0f;
    }
    return glm::dot(edge2, q) * inverse;
}

// Bounding volume hierarchy over the bake triangles, built by median splits
// on the longest centroid axis with index tie-breaks, so it is deterministic.
class TriangleBvh {
  public:
    explicit TriangleBvh(const std::vector<IrradianceBakeTriangle>& triangles)
        : m_triangles(triangles) {
        m_order.resize(triangles.size());
        std::iota(m_order.begin(), m_order.end(), 0u);
        m_centroids.reserve(triangles.size());
        for (const IrradianceBakeTriangle& triangle : triangles) {
            m_centroids.push_back((triangle.a + triangle.b + triangle.c) / 3.0f);
        }
        if (!triangles.empty()) {
            build(0, static_cast<uint32_t>(triangles.size()));
        }
    }

    // Nearest hit closer than maximumDistance.
    std::optional<Hit> closest(const Ray& ray, float maximumDistance) const {
        Hit best{.distance = maximumDistance};
        bool found = false;
        traverse(ray, best.distance, [&](uint32_t triangle, float distance) {
            best = {distance, triangle};
            found = true;
            return false;
        });
        return found ? std::optional<Hit>(best) : std::nullopt;
    }

    bool occluded(const Ray& ray, float maximumDistance) const {
        bool hit = false;
        float limit = maximumDistance;
        traverse(ray, limit, [&](uint32_t, float) {
            hit = true;
            return true;
        });
        return hit;
    }

  private:
    struct Node {
        glm::vec3 minimum{0.0f};
        glm::vec3 maximum{0.0f};
        // Leaves cover m_order[first, first + count); inner nodes have count 0
        // and two children.
        uint32_t first{0};
        uint32_t count{0};
        uint32_t left{0};
        uint32_t right{0};
    };

    uint32_t build(uint32_t first, uint32_t count) {
        const uint32_t index = static_cast<uint32_t>(m_nodes.size());
        m_nodes.push_back({});
        Node node{.minimum = glm::vec3(std::numeric_limits<float>::max()),
                  .maximum = glm::vec3(std::numeric_limits<float>::lowest())};
        glm::vec3 centroidMinimum(std::numeric_limits<float>::max());
        glm::vec3 centroidMaximum(std::numeric_limits<float>::lowest());
        for (uint32_t entry = first; entry < first + count; ++entry) {
            const IrradianceBakeTriangle& triangle = m_triangles[m_order[entry]];
            node.minimum = glm::min(node.minimum, glm::min(triangle.a, glm::min(triangle.b, triangle.c)));
            node.maximum = glm::max(node.maximum, glm::max(triangle.a, glm::max(triangle.b, triangle.c)));
            centroidMinimum = glm::min(centroidMinimum, m_centroids[m_order[entry]]);
            centroidMaximum = glm::max(centroidMaximum, m_centroids[m_order[entry]]);
        }
        if (count <= bvhLeafTriangles) {
            node.first = first;
            node.count = count;
            m_nodes[index] = node;
            return index;
        }
        const glm::vec3 extent = centroidMaximum - centroidMinimum;
        const int axis = extent.x >= extent.y && extent.x >= extent.z ? 0 : (extent.y >= extent.z ? 1 : 2);
        const auto begin = m_order.begin() + first;
        std::sort(begin, begin + count, [&](uint32_t left, uint32_t right) {
            const float leftValue = m_centroids[left][axis];
            const float rightValue = m_centroids[right][axis];
            return leftValue != rightValue ? leftValue < rightValue : left < right;
        });
        const uint32_t half = count / 2;
        node.left = build(first, half);
        node.right = build(first + half, count - half);
        m_nodes[index] = node;
        return index;
    }

    static bool hitsBox(const Ray& ray, const glm::vec3& inverseDirection, const Node& node, float limit) {
        const glm::vec3 near = (node.minimum - ray.origin) * inverseDirection;
        const glm::vec3 far = (node.maximum - ray.origin) * inverseDirection;
        const glm::vec3 entry = glm::min(near, far);
        const glm::vec3 exit = glm::max(near, far);
        const float enter = std::max({entry.x, entry.y, entry.z, 0.0f});
        const float leave = std::min({exit.x, exit.y, exit.z, limit});
        return enter <= leave;
    }

    // Calls visit(triangle, distance) for hits closer than `limit`, which the
    // caller may shrink through the reference; visit returns true to stop.
    template <typename Visit> void traverse(const Ray& ray, float& limit, Visit visit) const {
        if (m_nodes.empty()) {
            return;
        }
        // Zero components become tiny ones so slab tests never compute 0 * inf.
        glm::vec3 inverseDirection(0.0f);
        for (int axis = 0; axis < 3; ++axis) {
            const float component = ray.direction[axis];
            inverseDirection[axis] = 1.0f / (std::abs(component) < 1e-8f ? std::copysign(1e-8f, component) : component);
        }
        uint32_t stack[64];
        uint32_t depth = 0;
        stack[depth++] = 0;
        while (depth > 0) {
            const Node& node = m_nodes[stack[--depth]];
            if (!hitsBox(ray, inverseDirection, node, limit)) {
                continue;
            }
            if (node.count == 0) {
                stack[depth++] = node.left;
                stack[depth++] = node.right;
                continue;
            }
            for (uint32_t entry = node.first; entry < node.first + node.count; ++entry) {
                const float distance = intersect(ray, m_triangles[m_order[entry]]);
                if (distance > 0.0f && distance < limit) {
                    limit = distance;
                    if (visit(m_order[entry], distance)) {
                        return;
                    }
                }
            }
        }
    }

    const std::vector<IrradianceBakeTriangle>& m_triangles;
    std::vector<uint32_t> m_order;
    std::vector<glm::vec3> m_centroids;
    std::vector<Node> m_nodes;
};

// What one probe ray saw. Surfaces store their facing normal and the radiance
// they reflect from direct light plus their emission.
struct RaySample {
    float distance{0.0f};
    bool escaped{true};
    bool backface{false};
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f};
    glm::vec3 albedo{0.0f};
    glm::vec3 directRadiance{0.0f};
};

class Baker {
  public:
    Baker(const IrradianceBakeScene& scene, const IrradianceVolumeBakeRequest& request)
        : m_scene(scene)
        , m_request(request)
        , m_bvh(scene.triangles) {
        const uint32_t size = request.settings.directionsPerFace;
        for (uint32_t face = 0; face < 6; ++face) {
            for (uint32_t y = 0; y < size; ++y) {
                for (uint32_t x = 0; x < size; ++x) {
                    m_directions.push_back(environment_lighting::cubeFaceDirection(face, (glm::vec2(x, y) + 0.5f) /
                                                                                             static_cast<float>(size)));
                }
            }
        }
        m_volume.minimum = request.minimum;
        m_volume.maximum = request.maximum;
        m_volume.probeCounts = request.probeCounts;
        m_volume.probes.resize(static_cast<size_t>(request.probeCounts.x) * request.probeCounts.y *
                               request.probeCounts.z);
        m_maximumDistance = 1.5f * glm::length(m_volume.cellSize());
        m_samples.resize(m_volume.probes.size());
        // Weights that filter ray distances into each octahedral depth texel,
        // the same for every probe.
        m_depthWeights.resize(static_cast<size_t>(irradianceProbeDepthTexels) * m_directions.size());
        for (uint32_t texel = 0; texel < irradianceProbeDepthTexels; ++texel) {
            const glm::vec3 texelDirection = irradiance_volume::depthTexelDirection(
                texel % irradianceProbeDepthResolution, texel / irradianceProbeDepthResolution);
            for (size_t ray = 0; ray < m_directions.size(); ++ray) {
                const float cosine = glm::dot(texelDirection, m_directions[ray]);
                m_depthWeights[texel * m_directions.size() + ray] =
                    cosine > 0.0f ? std::pow(cosine, depthFilterSharpness) : 0.0f;
            }
        }
        // Surfaces outside the volume's influence are lit by the environment at
        // runtime, so secondary bounces fall back to it there too.
        m_environment = environment_lighting::projectIrradiance(
            [this](const glm::vec3& direction) {
                return m_scene.skyRadiance ? glm::max(m_scene.skyRadiance(direction), glm::vec3(0.0f))
                                           : glm::vec3(0.0f);
            },
            16);
    }

    IrradianceVolumeData run() {
        forEachProbe([this](size_t probe) { traceProbe(probe); });
        const uint32_t bounces = std::max(m_request.settings.bounces, 1u);
        for (uint32_t bounce = 0; bounce < bounces; ++bounce) {
            const IrradianceVolumeData previous = m_volume;
            const bool includeIndirect = bounce > 0;
            forEachProbe([&](size_t probe) { gatherProbe(probe, previous, includeIndirect); });
        }
        return std::move(m_volume);
    }

  private:
    template <typename Work> void forEachProbe(Work work) const {
        const uint32_t requested = m_request.settings.workerThreads;
        const uint32_t workers = std::max(requested == 0 ? std::thread::hardware_concurrency() : requested, 1u);
        std::atomic<size_t> next{0};
        const size_t count = m_volume.probes.size();
        const auto loop = [&] {
            for (size_t probe = next++; probe < count; probe = next++) {
                work(probe);
            }
        };
        std::vector<std::jthread> threads;
        for (uint32_t worker = 1; worker < std::min<size_t>(workers, count); ++worker) {
            threads.emplace_back(loop);
        }
        loop();
    }

    glm::uvec3 probeCoordinate(size_t probe) const {
        const glm::uvec3 counts = m_volume.probeCounts;
        return {static_cast<uint32_t>(probe % counts.x), static_cast<uint32_t>((probe / counts.x) % counts.y),
                static_cast<uint32_t>(probe / (static_cast<size_t>(counts.x) * counts.y))};
    }

    glm::vec3 directLight(const glm::vec3& position, const glm::vec3& normal) const {
        const glm::vec3 origin = position + normal * surfaceOffset;
        glm::vec3 irradiance(0.0f);
        if (m_scene.sun) {
            const float nDotL = glm::dot(normal, m_scene.sun->direction);
            if (nDotL > 0.0f &&
                (!m_scene.sun->castsShadows ||
                 !m_bvh.occluded({origin, m_scene.sun->direction}, std::numeric_limits<float>::infinity()))) {
                irradiance += m_scene.sun->irradiance * nDotL;
            }
        }
        for (const LocalLight& light : m_scene.localLights) {
            const glm::vec3 toLight = light.position - position;
            const float distance = glm::length(toLight);
            if (distance >= light.range || distance <= 0.0001f) {
                continue;
            }
            const glm::vec3 direction = toLight / distance;
            const float nDotL = glm::dot(normal, direction);
            if (nDotL <= 0.0f) {
                continue;
            }
            // Mirrors the local-light attenuation in mesh.frag.
            const float falloff = std::max(1.0f - distance / light.range, 0.0f);
            float attenuation = falloff * falloff / std::max(distance * distance, 0.01f);
            if (light.type == LocalLightType::Spot) {
                attenuation *= glm::smoothstep(light.outerConeCosine, light.innerConeCosine,
                                               glm::dot(-direction, light.direction));
            }
            if (attenuation <= 0.0f || m_bvh.occluded({origin, direction}, distance - surfaceOffset * 2.0f)) {
                continue;
            }
            irradiance += light.color * light.intensity * attenuation * nDotL;
        }
        return irradiance;
    }

    void traceProbe(size_t probe) {
        const glm::vec3 position = m_volume.probePosition(probeCoordinate(probe));
        std::vector<RaySample>& samples = m_samples[probe];
        samples.resize(m_directions.size());
        uint32_t backfaces = 0;
        for (size_t ray = 0; ray < m_directions.size(); ++ray) {
            const glm::vec3 direction = m_directions[ray];
            RaySample& sample = samples[ray];
            const std::optional<Hit> hit = m_bvh.closest({position, direction}, std::numeric_limits<float>::infinity());
            if (!hit) {
                sample = {.distance = m_maximumDistance, .escaped = true};
                continue;
            }
            const IrradianceBakeTriangle& triangle = m_scene.triangles[hit->triangle];
            glm::vec3 normal = glm::cross(triangle.b - triangle.a, triangle.c - triangle.a);
            normal = glm::dot(normal, normal) > 0.0f ? glm::normalize(normal) : -direction;
            const bool facesRay = glm::dot(normal, direction) < 0.0f;
            sample = {.distance = std::min(hit->distance, m_maximumDistance),
                      .escaped = false,
                      .position = position + direction * hit->distance};
            if (!facesRay && !triangle.doubleSided) {
                sample.backface = true;
                sample.distance *= backfaceDistanceScale;
                ++backfaces;
                continue;
            }
            sample.normal = facesRay ? normal : -normal;
            sample.albedo = triangle.albedo;
            sample.directRadiance =
                triangle.albedo / pi * directLight(sample.position, sample.normal) + triangle.emission;
        }
        IrradianceProbe& output = m_volume.probes[probe];
        output.valid = static_cast<float>(backfaces) < invalidBackfaceFraction * static_cast<float>(samples.size());
        filterDepthMoments(samples, output);
    }

    void filterDepthMoments(const std::vector<RaySample>& samples, IrradianceProbe& output) const {
        for (uint32_t texel = 0; texel < irradianceProbeDepthTexels; ++texel) {
            const float* weights = m_depthWeights.data() + texel * samples.size();
            glm::vec2 moments(0.0f);
            float totalWeight = 0.0f;
            for (size_t ray = 0; ray < samples.size(); ++ray) {
                const float distance = samples[ray].distance;
                moments += weights[ray] * glm::vec2(distance, distance * distance);
                totalWeight += weights[ray];
            }
            output.depthMoments[texel] = totalWeight > 0.0f
                                             ? moments / totalWeight
                                             : glm::vec2(m_maximumDistance, m_maximumDistance * m_maximumDistance);
        }
    }

    glm::vec3 radianceAlong(const RaySample& sample, const glm::vec3& direction, const IrradianceVolumeData& previous,
                            bool includeIndirect) const {
        if (sample.escaped) {
            return m_scene.skyRadiance ? glm::max(m_scene.skyRadiance(direction), glm::vec3(0.0f)) : glm::vec3(0.0f);
        }
        if (sample.backface) {
            return glm::vec3(0.0f);
        }
        glm::vec3 radiance = sample.directRadiance;
        if (includeIndirect) {
            // Mirrors indirectDiffuseRadiance in mesh.frag.
            const irradiance_volume::Sample bounce =
                irradiance_volume::sample(previous, m_request.shading, sample.position, sample.normal, -direction);
            const glm::vec3 environment = environment_lighting::evaluateIrradiance(m_environment, sample.normal);
            radiance += sample.albedo * glm::mix(environment, bounce.irradiance, bounce.volumeWeight);
        }
        return radiance;
    }

    void gatherProbe(size_t probe, const IrradianceVolumeData& previous, bool includeIndirect) {
        const std::vector<RaySample>& samples = m_samples[probe];
        const uint32_t size = m_request.settings.directionsPerFace;
        const environment_lighting::IrradianceCoefficients coefficients = environment_lighting::projectIrradiance(
            [&](const glm::vec3& direction) {
                // Recover the ray from the direction so the result does not
                // depend on projectIrradiance's iteration order.
                const environment_lighting::CubeFaceCoordinate coordinate =
                    environment_lighting::cubeFaceCoordinate(direction);
                const glm::uvec2 texel =
                    glm::min(glm::uvec2(coordinate.uv * static_cast<float>(size)), glm::uvec2(size - 1));
                const size_t ray = (static_cast<size_t>(coordinate.face) * size + texel.y) * size + texel.x;
                return radianceAlong(samples[ray], m_directions[ray], previous, includeIndirect);
            },
            size);
        std::copy(coefficients.begin(), coefficients.end(), m_volume.probes[probe].irradiance.begin());
    }

    const IrradianceBakeScene& m_scene;
    const IrradianceVolumeBakeRequest& m_request;
    TriangleBvh m_bvh;
    std::vector<glm::vec3> m_directions;
    IrradianceVolumeData m_volume;
    std::vector<std::vector<RaySample>> m_samples;
    // Row per depth texel, column per ray.
    std::vector<float> m_depthWeights;
    environment_lighting::IrradianceCoefficients m_environment{};
    float m_maximumDistance{1.0f};
};

template <typename Component> const Component* primaryComponent(const Scene& scene) {
    // Mirrors resolvePrimary in MeshPass.cpp: the primary component on the
    // lowest entity handle.
    const auto components = scene.registry().view<const Component>();
    Entity selected = entt::null;
    for (Entity entity : components) {
        if (components.template get<const Component>(entity).primary &&
            (selected == entt::null || entt::to_integral(entity) < entt::to_integral(selected))) {
            selected = entity;
        }
    }
    return selected == entt::null ? nullptr : &components.template get<const Component>(selected);
}

glm::vec3 srgbToLinear(uint8_t red, uint8_t green, uint8_t blue) {
    const auto convert = [](uint8_t value) {
        const float encoded = static_cast<float>(value) / 255.0f;
        return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
    };
    return {convert(red), convert(green), convert(blue)};
}

// Mean linear color of a decoded RGBA8 texture, or one when the texture is
// missing or only block compressed.
glm::vec3 averageTextureColor(const AssetManager& assets, TextureHandle handle,
                              std::map<std::pair<uint32_t, uint32_t>, glm::vec3>& cache) {
    const TextureAsset* texture = handle ? assets.texture(handle) : nullptr;
    if (texture == nullptr || texture->rgba.size() < 4) {
        return glm::vec3(1.0f);
    }
    const auto key = std::make_pair(handle.index, handle.generation);
    if (const auto found = cache.find(key); found != cache.end()) {
        return found->second;
    }
    const size_t texels = texture->rgba.size() / 4;
    const size_t stride = std::max<size_t>(texels / 65536, 1);
    glm::dvec3 sum(0.0);
    size_t count = 0;
    for (size_t texel = 0; texel < texels; texel += stride) {
        const uint8_t* rgba = texture->rgba.data() + texel * 4;
        sum += glm::dvec3(srgbToLinear(rgba[0], rgba[1], rgba[2]));
        ++count;
    }
    const glm::vec3 average = glm::vec3(sum / static_cast<double>(count));
    cache.emplace(key, average);
    return average;
}

void appendMeshTriangles(const Scene& scene, const AssetManager& assets, std::vector<IrradianceBakeTriangle>& output) {
    std::vector<Entity> entities;
    const auto meshes = scene.registry().view<const MeshRendererComponent, const TransformComponent>();
    entities.assign(meshes.begin(), meshes.end());
    std::ranges::sort(entities, {}, [&scene](Entity entity) { return scene.id(entity).value; });
    std::map<std::pair<uint32_t, uint32_t>, glm::vec3> textureAverages;
    for (Entity entity : entities) {
        const MeshAsset* mesh = assets.mesh(meshes.get<const MeshRendererComponent>(entity).mesh);
        if (mesh == nullptr) {
            continue;
        }
        const glm::mat4& world = meshes.get<const TransformComponent>(entity).world;
        const bool mirrored = glm::determinant(glm::mat3(world)) < 0.0f;
        for (const MeshPrimitive& primitive : mesh->primitives) {
            const MaterialAsset defaults;
            const MaterialAsset* found = assets.material(primitive.material);
            const MaterialAsset& material = found ? *found : defaults;
            if (material.translucent()) {
                continue;
            }
            const glm::vec3 baseColor =
                glm::vec3(material.baseColor) * averageTextureColor(assets, material.baseColorTexture, textureAverages);
            const glm::vec3 albedo = glm::clamp(baseColor * (1.0f - std::clamp(material.metallic, 0.0f, 1.0f)),
                                                glm::vec3(0.0f), glm::vec3(1.0f));
            const glm::vec3 emission = glm::max(
                material.emissive * material.emissiveStrength *
                    (material.emissiveTexture ? averageTextureColor(assets, material.emissiveTexture, textureAverages)
                                              : glm::vec3(1.0f)),
                glm::vec3(0.0f));
            const uint32_t end = std::min<uint32_t>(primitive.firstIndex + primitive.indexCount,
                                                    static_cast<uint32_t>(mesh->indices.size()));
            for (uint32_t index = primitive.firstIndex; index + 2 < end; index += 3) {
                const auto corner = [&](uint32_t offset) {
                    const uint32_t vertex = mesh->indices[index + offset];
                    return vertex < mesh->vertices.size()
                               ? glm::vec3(world * glm::vec4(mesh->vertices[vertex].position, 1.0f))
                               : glm::vec3(0.0f);
                };
                IrradianceBakeTriangle triangle{.a = corner(0),
                                                .b = corner(1),
                                                .c = corner(2),
                                                .albedo = albedo,
                                                .emission = emission,
                                                .doubleSided = material.doubleSided};
                if (mirrored) {
                    std::swap(triangle.b, triangle.c);
                }
                output.push_back(triangle);
            }
        }
    }
}

// Box-filters the environment map to a small equirectangular grid of linear
// radiance; empty when the map is missing or undecoded.
std::vector<glm::vec3> filteredSky(const TextureAsset* map) {
    if (map == nullptr || map->width == 0 || map->height == 0) {
        return {};
    }
    const size_t texels = static_cast<size_t>(map->width) * map->height;
    const bool half = map->rgbaHalf.size() >= texels * 4;
    if (!half && map->rgba.size() < texels * 4) {
        return {};
    }
    std::vector<glm::dvec3> sums(static_cast<size_t>(skyWidth) * skyHeight, glm::dvec3(0.0));
    std::vector<uint32_t> counts(sums.size(), 0);
    for (uint32_t y = 0; y < map->height; ++y) {
        const uint32_t cellY = y * skyHeight / map->height;
        for (uint32_t x = 0; x < map->width; ++x) {
            const size_t offset = (static_cast<size_t>(y) * map->width + x) * 4;
            // Decoded like environment_lighting's CPU reference (gamma 2.2 for
            // 8-bit maps), so the bake matches the environment it blends into.
            const glm::vec3 color =
                half ? glm::vec3(glm::unpackHalf1x16(map->rgbaHalf[offset]),
                                 glm::unpackHalf1x16(map->rgbaHalf[offset + 1]),
                                 glm::unpackHalf1x16(map->rgbaHalf[offset + 2]))
                     : glm::pow(glm::vec3(map->rgba[offset], map->rgba[offset + 1], map->rgba[offset + 2]) / 255.0f,
                                glm::vec3(2.2f));
            const size_t cell = static_cast<size_t>(cellY) * skyWidth + x * skyWidth / map->width;
            sums[cell] += glm::dvec3(glm::max(color, glm::vec3(0.0f)));
            ++counts[cell];
        }
    }
    std::vector<glm::vec3> cells(sums.size(), glm::vec3(0.0f));
    for (size_t cell = 0; cell < cells.size(); ++cell) {
        if (counts[cell] > 0) {
            cells[cell] = glm::vec3(sums[cell] / static_cast<double>(counts[cell]));
        }
    }
    return cells;
}

IrradianceVolumeBakeResult writeAtomically(const std::filesystem::path& destination,
                                           const std::vector<std::byte>& bytes) {
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) {
        return {.error = "Could not create irradiance-volume bake directory: " + error.message()};
    }
    const std::filesystem::path temporary = destination.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output) {
            output.close();
            std::filesystem::remove(temporary, error);
            return {.error = "Could not write irradiance-volume bake: " + temporary.string()};
        }
    }
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return {.error = "Could not replace irradiance-volume bake " + destination.string()};
    }
    return {.success = true};
}
} // namespace

IrradianceBakeScene gatherIrradianceBakeScene(const Scene& scene, const AssetManager& assets) {
    IrradianceBakeScene result;
    appendMeshTriangles(scene, assets, result.triangles);
    // The renderer falls back to default lights when none is primary; so does the bake.
    const DirectionalLightComponent* primarySun = primaryComponent<DirectionalLightComponent>(scene);
    const DirectionalLightComponent sun = primarySun ? *primarySun : DirectionalLightComponent{};
    if (glm::dot(sun.direction, sun.direction) > 0.0f) {
        result.sun =
            IrradianceBakeSun{.direction = glm::normalize(sun.direction),
                              .irradiance = glm::max(sun.color, glm::vec3(0.0f)) * std::max(sun.intensity, 0.0f),
                              .castsShadows = sun.shadows.enabled};
    }
    result.localLights = gatherLocalLights(scene);
    const EnvironmentLightComponent* primaryEnvironment = primaryComponent<EnvironmentLightComponent>(scene);
    const EnvironmentLightComponent environment =
        primaryEnvironment ? *primaryEnvironment : EnvironmentLightComponent{};
    const float intensity = std::max(environment.diffuseIntensity, 0.0f);
    std::vector<glm::vec3> sky =
        filteredSky(environment.environmentMap ? assets.texture(environment.environmentMap) : nullptr);
    if (sky.empty()) {
        result.skyRadiance = [sky = environment.skyColor, ground = environment.groundColor,
                              intensity](const glm::vec3& direction) {
            return environment_lighting::proceduralSkyRadiance(direction, sky, ground) * intensity;
        };
    } else {
        result.skyRadiance = [cells = std::move(sky), intensity](const glm::vec3& direction) {
            const glm::vec2 uv = environment_lighting::equirectangularCoordinate(direction);
            const uint32_t x = std::min(static_cast<uint32_t>(uv.x * skyWidth), skyWidth - 1);
            const uint32_t y = std::min(static_cast<uint32_t>(uv.y * skyHeight), skyHeight - 1);
            return cells[static_cast<size_t>(y) * skyWidth + x] * intensity;
        };
    }
    return result;
}

IrradianceVolumeBakeResult bakeIrradianceVolume(const IrradianceBakeScene& scene,
                                                const IrradianceVolumeBakeRequest& request,
                                                IrradianceVolumeData& output) {
    if (request.settings.directionsPerFace == 0 || request.settings.directionsPerFace > 64) {
        return {.error = "Irradiance-volume directions per face must be between 1 and 64"};
    }
    const glm::uvec3 counts = request.probeCounts;
    if (glm::any(glm::equal(counts, glm::uvec3(0u))) ||
        glm::any(glm::greaterThan(counts, glm::uvec3(maximumIrradianceProbesPerAxis))) ||
        static_cast<uint64_t>(counts.x) * counts.y * counts.z > maximumIrradianceProbes) {
        return {.error = "Irradiance-volume probe counts must be 1 to " +
                         std::to_string(maximumIrradianceProbesPerAxis) + " per axis and at most " +
                         std::to_string(maximumIrradianceProbes) + " in total"};
    }
    if (glm::any(glm::lessThanEqual(request.maximum, request.minimum))) {
        return {.error = "Irradiance-volume bounds must have a positive size on every axis"};
    }
    IrradianceVolumeData baked = Baker(scene, request).run();
    if (IrradianceVolumeResult valid = validateIrradianceVolume(baked); !valid) {
        return {.error = "Irradiance-volume bake produced invalid data: " + valid.error};
    }
    output = std::move(baked);
    return {.success = true};
}

IrradianceVolumeBakeResult bakeIrradianceVolumeFile(const Scene& scene, const AssetManager& assets, Entity entity,
                                                    const std::filesystem::path& directory,
                                                    const IrradianceVolumeBakeSettings& settings,
                                                    IrradianceVolumeData& baked, std::filesystem::path& written) {
    const auto* volume = scene.registry().try_get<IrradianceVolumeComponent>(entity);
    const auto* transform = scene.registry().try_get<TransformComponent>(entity);
    if (volume == nullptr || transform == nullptr) {
        return {.error = "The selected entity has no irradiance volume"};
    }
    const glm::vec3 center = glm::vec3(transform->world[3]);
    const IrradianceVolumeBakeRequest request{.minimum = center - volume->halfExtents,
                                              .maximum = center + volume->halfExtents,
                                              .probeCounts = volume->probeCounts,
                                              .shading = {.normalBias = volume->normalBias,
                                                          .viewBias = volume->viewBias,
                                                          .blendDistance = volume->blendDistance},
                                              .settings = settings};
    IrradianceVolumeData result;
    if (IrradianceVolumeBakeResult bake =
            bakeIrradianceVolume(gatherIrradianceBakeScene(scene, assets), request, result);
        !bake) {
        return bake;
    }
    std::vector<std::byte> bytes;
    if (IrradianceVolumeResult encoded = encodeIrradianceVolume(result, bytes); !encoded) {
        return {.error = encoded.error};
    }
    const std::filesystem::path destination = directory / irradianceVolumeBakeFileName(scene.id(entity), bytes);
    // Names are content addressed, so an existing file already holds these bytes.
    std::error_code error;
    if (!std::filesystem::exists(destination, error)) {
        if (IrradianceVolumeBakeResult saved = writeAtomically(destination, bytes); !saved) {
            return saved;
        }
    }
    baked = std::move(result);
    written = destination;
    return {.success = true};
}

IrradianceVolumeBakeResult bakeSceneIrradianceVolumes(Scene& scene, const AssetManager& assets,
                                                      const std::filesystem::path& assetRoot,
                                                      const std::optional<std::filesystem::path>& scenePath,
                                                      const IrradianceVolumeBakeSettings& settings,
                                                      uint32_t& volumesBaked) {
    volumesBaked = 0;
    scene.updateWorldTransforms();
    std::vector<Entity> volumes;
    const auto view = scene.registry().view<IrradianceVolumeComponent>();
    volumes.assign(view.begin(), view.end());
    std::ranges::sort(volumes, {}, [&scene](Entity entity) { return scene.id(entity).value; });
    const std::filesystem::path directory = irradianceVolumeBakeDirectory(assetRoot, scenePath);
    for (Entity entity : volumes) {
        auto baked = std::make_shared<IrradianceVolumeData>();
        std::filesystem::path written;
        if (IrradianceVolumeBakeResult result =
                bakeIrradianceVolumeFile(scene, assets, entity, directory, settings, *baked, written);
            !result) {
            return result;
        }
        IrradianceVolumeComponent& volume = scene.registry().get<IrradianceVolumeComponent>(entity);
        volume.bakeAsset = written.lexically_relative(assetRoot).generic_string();
        volume.bakedData = std::move(baked);
        ++volumesBaked;
    }
    return {.success = true};
}

std::filesystem::path irradianceVolumeBakeDirectory(const std::filesystem::path& assetRoot,
                                                    const std::optional<std::filesystem::path>& scenePath) {
    const std::string sceneName = scenePath && !scenePath->stem().empty() ? scenePath->stem().string() : "untitled";
    return assetRoot / "baked" / sceneName;
}

std::string irradianceVolumeBakeFileName(SceneEntityId volume, std::span<const std::byte> encoded) {
    uint64_t hash = 1469598103934665603ull;
    for (std::byte value : encoded) {
        hash = (hash ^ static_cast<uint8_t>(value)) * 1099511628211ull;
    }
    char digest[17]{};
    std::snprintf(digest, sizeof(digest), "%016llx", static_cast<unsigned long long>(hash));
    return "irradiance_volume_" + std::to_string(volume.value) + "_" + digest + ".zvol";
}
