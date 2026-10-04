#pragma once

#include "Rendering.hpp"
#include "engine/scene/ParticleEmitter.hpp"
#include "engine/scene/SceneVisibilityMode.hpp"

#include <glm/glm.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

class Scene;

// CPU side of the GPU particle system shared by ParticlePass, its shaders
// (particle_emit.comp, particle_simulate.comp, particle_sort.comp,
// particle.vert, particle.frag), and the CPU tests.
//
// Pool layout: every emitter owns a contiguous ring of slots in one particle
// pool (see ParticleScheduler). Each frame an emitter spawns into the next
// slots of its ring, so once the ring is full the oldest particle is replaced.
// Slot assignment, spawn counts, and every random value derive from the
// emitter's stable scene id, the spawn ordinal, and the frame time step, so a
// fixed time step (ZENITH_FIXED_FRAME_TIME) renders identical frames.
//
// Frame order: emit writes the new particles; simulate ages and moves every
// live particle, frees expired ones, culls against the view, lights lit ones,
// and appends the visible ones to the sort keys and the indirect draw; the
// bitonic sort then orders the keys back to front; the draw pulls one
// camera-facing quad per key.
//
// Units: world units (meters) and seconds; colors and radiance are linear.
namespace particles {

// Emitters simulated per frame; further emitters (in scene id order) are skipped.
inline constexpr uint32_t maximumEmitters = 64;
// Sprite textures per frame; slot 0 is the procedural soft round sprite.
inline constexpr uint32_t maximumSpriteTextures = 8;
// Mirrors local_size_x in particle_emit.comp and particle_simulate.comp.
inline constexpr uint32_t simulationWorkgroupSize = 64;
// Mirrors local_size_x in particle_sort.comp; each workgroup sorts or merges a
// block of twice as many keys in shared memory.
inline constexpr uint32_t sortWorkgroupSize = 512;
inline constexpr uint32_t sortBlockElements = sortWorkgroupSize * 2;
// One camera-facing quad (two triangles) per particle.
inline constexpr uint32_t verticesPerParticle = 6;

// Bits of GpuParticleEmitter::spawn.w and GpuParticle::info.z.
inline constexpr uint32_t flagAdditive = 1u << 0;
inline constexpr uint32_t flagLit = 1u << 1;
inline constexpr uint32_t flagLocalSpace = 1u << 2;
inline constexpr uint32_t textureSlotShift = 8;

// Particle slots shared by every emitter at a quality; zero for Off.
uint32_t particleBudget(ParticleQuality quality);

// Mirrors Particle in the particle shaders (std430).
struct GpuParticle {
    // xyz: simulation-space position (world, or entity space for Local
    // emitters); w: age in seconds.
    glm::vec4 positionAge{0.0f};
    // xyz: simulation-space velocity; w: lifetime in seconds, zero for a free slot.
    glm::vec4 velocityLifetime{0.0f};
    // Written by simulate for the draw: xyz world center, w billboard size.
    glm::vec4 worldPositionSize{0.0f};
    // Written by simulate: rgb emitted plus reflected radiance, a opacity.
    glm::vec4 radiance{0.0f};
    // x: rotation in radians; y: soft-particle fade distance.
    glm::vec4 render{0.0f};
    // x: emitter index; y: random seed; z: flags and texture slot.
    glm::uvec4 info{0u};
};

static_assert(sizeof(GpuParticle) == 96, "GpuParticle must match the std430 Particle struct");
static_assert(offsetof(GpuParticle, radiance) == 48 && offsetof(GpuParticle, info) == 80,
              "GpuParticle members must match the std430 Particle struct");

// Mirrors Emitter in particle_emit.comp and particle_simulate.comp (std430).
struct GpuParticleEmitter {
    // Entity world transform; simulation space to world for Local emitters.
    glm::mat4 localToWorld{1.0f};
    // xyz: acceleration in simulation space; w: drag per second.
    glm::vec4 gravityDrag{0.0f};
    // xyz: unit cone axis in entity space; w: cosine of the cone half angle.
    glm::vec4 direction{0.0f, 1.0f, 0.0f, 1.0f};
    // x, y: speed range; z, w: lifetime range.
    glm::vec4 speedLifetime{0.0f};
    // x, y: start and end size; z: angular speed; w: spawn radius.
    glm::vec4 sizeSpin{0.0f};
    glm::vec4 startColor{1.0f};
    glm::vec4 endColor{1.0f};
    // x: emissive intensity; y: soft distance.
    glm::vec4 shading{0.0f};
    // x: first pool slot; y: ring capacity; z: ring offset of this frame's
    // first spawn; w: particles spawned this frame.
    glm::uvec4 pool{0u};
    // x: index of the emitter's first spawn among this frame's spawns; y:
    // spawn ordinal of that particle (wraps); z: emitter seed; w: flags.
    glm::uvec4 spawn{0u};
};

static_assert(sizeof(GpuParticleEmitter) == 208, "GpuParticleEmitter must match the std430 Emitter struct");
static_assert(offsetof(GpuParticleEmitter, pool) == 176 && offsetof(GpuParticleEmitter, spawn) == 192,
              "GpuParticleEmitter members must match the std430 Emitter struct");

// Mirrors ParticleFrame in particle_emit.comp and particle_simulate.comp (std140).
struct GpuParticleFrame {
    glm::mat4 viewProjection{1.0f};
    std::array<glm::mat4, 3> lightViewProjections{};
    // xyz: camera position; w: simulation time step in seconds.
    glm::vec4 cameraPosition{0.0f};
    // xyz: toward the sun.
    glm::vec4 lightDirection{0.0f, 1.0f, 0.0f, 0.0f};
    glm::vec4 lightRadiance{0.0f};
    // Far distance of each shadow cascade from the camera.
    glm::vec4 cascadeSplits{0.0f};
    // x, y: cluster slice = log(view depth) * x + y; z, w: projection[0][0], [1][1].
    glm::vec4 clusterDepth{0.0f};
    // Cluster columns, rows, depth slices, uploaded local-light count.
    glm::uvec4 clusterGrid{0u};
    // x: pool slots in use; y: spawns this frame; z: sort keys; w: shadow cascades.
    glm::uvec4 counts{0u};
    // rgb: ambient radiance without image-based lighting; w: environment
    // diffuse intensity applied to the irradiance SH.
    glm::vec4 ambient{0.0f};
    // x: 1 reads ambient from the irradiance SH; y: emitter count.
    glm::uvec4 flags{0u};
    // Irradiance volume; mirrors the camera data of the same name.
    glm::vec4 irradianceVolumeMinimum{0.0f};
    glm::vec4 irradianceVolumeCellSize{1.0f};
    glm::uvec4 irradianceVolumeCounts{0u};
};

static_assert(sizeof(GpuParticleFrame) == 4 * 64 + 12 * 16, "GpuParticleFrame must match the std140 block");

// One entry of the sort buffer: key orders back to front, index names the
// pool slot. Unused entries hold 0xffffffff in both.
struct SortKey {
    uint32_t key{0xffffffffu};
    uint32_t index{0xffffffffu};

    auto operator<=>(const SortKey&) const = default;
};

static_assert(sizeof(SortKey) == 8, "SortKey must match uvec2");

// Ascending keys draw farther particles first; mirrors particle_simulate.comp.
uint32_t backToFrontKey(float viewDepth);

// Keys sorted per frame: a power of two of at least one sort block, covering
// liveParticles.
uint32_t sortElementCount(uint32_t liveParticles);

enum class SortStep : uint32_t {
    // Sorts each block of sortBlockElements keys in shared memory (all merge
    // sizes up to the block).
    LocalSort = 0,
    // One compare-exchange step of distance >= sortBlockElements in global memory.
    GlobalMerge = 1,
    // Finishes a merge size inside each block (distances below a block).
    LocalMerge = 2,
};

// One dispatch of the bitonic network; mirrors the push constants of
// particle_sort.comp. mergeSize is the bitonic sequence size k, distance the
// compare distance j (both powers of two).
struct SortPass {
    SortStep step{SortStep::LocalSort};
    uint32_t mergeSize{0};
    uint32_t distance{0};

    bool operator==(const SortPass&) const = default;
};

// Dispatches that sort elementCount keys (a sortElementCount result) in
// ascending order. Every pass dispatches sortDispatchGroups(elementCount)
// workgroups of sortWorkgroupSize threads.
std::vector<SortPass> bitonicSortPlan(uint32_t elementCount);
uint32_t sortDispatchGroups(uint32_t elementCount);
// Runs the plan exactly as particle_sort.comp indexes it; for tests.
void sortReference(std::span<SortKey> keys);

// Seed of every random value an emitter produces, from its stable scene id.
uint32_t emitterSeed(uint64_t sceneEntityId);

// An emitter as the scheduler sees it this frame.
struct EmitterSnapshot {
    // Stable scene entity id; orders emitters and seeds their random values.
    uint64_t id{0};
    ParticleEmitterComponent emitter;
    glm::mat4 world{1.0f};
};

// One scheduled emitter of a frame.
struct ScheduledEmitter {
    uint64_t id{0};
    uint32_t firstSlot{0};
    uint32_t capacity{0};
    uint32_t ringStart{0};
    uint32_t spawnCount{0};
    // Index of the first spawn among the frame's spawns.
    uint32_t spawnOffset{0};
    // Spawn ordinal of the first spawn (wraps at 2^32).
    uint32_t firstOrdinal{0};
    // Most particles that can be alive: min(capacity, particles ever spawned).
    uint32_t liveBound{0};
};

struct ParticleFramePlan {
    // In scene id order; emitters that got no pool slots are left out.
    std::vector<ScheduledEmitter> emitters;
    // Pool slots owned by the scheduled emitters (a prefix of the pool).
    uint32_t poolSlots{0};
    uint32_t totalSpawns{0};
    uint32_t liveBound{0};
    // The emitter layout changed: every slot must be freed before emitting.
    bool resetPool{false};
};

// Decides each frame's pool layout and spawns. Layout: emitters in scene id
// order receive min(maxParticles, remaining budget) consecutive slots; a
// change of ids, capacities, simulation spaces, budget, or scene content
// generation (see Scene::contentGeneration) restarts every emitter and asks
// for a pool reset. Spawns: spawnRate accumulates fractional particles across
// frames; bursts fire when an emitter starts emitting and then every
// burstInterval.
class ParticleScheduler {
  public:
    // snapshots must be sorted by id with unique ids.
    ParticleFramePlan plan(std::span<const EmitterSnapshot> snapshots, uint32_t budget, float deltaSeconds,
                           uint64_t sceneGeneration = 0);
    void reset();

  private:
    struct EmitterState {
        uint64_t id{0};
        uint32_t capacity{0};
        ParticleSimulationSpace space{ParticleSimulationSpace::World};
        double spawnAccumulator{0.0};
        double burstTimer{0.0};
        uint64_t spawned{0};
        uint32_t cursor{0};
        bool started{false};
        // The emitter's restartGeneration when it last (re)started.
        uint32_t restartGeneration{0};
    };

    std::vector<EmitterState> m_states;
    uint32_t m_budget{0};
    uint64_t m_sceneGeneration{0};
};

// The scene's valid emitters (see validateParticleEmitter) that the visibility
// mode draws, with their world transforms, sorted by stable scene id; entities
// without one are skipped.
std::vector<EmitterSnapshot> gatherEmitters(const Scene& scene,
                                            SceneVisibilityMode visibility = SceneVisibilityMode::Runtime);

// Packs one scheduled emitter for the GPU. textureSlot selects the sprite.
GpuParticleEmitter packEmitter(const EmitterSnapshot& snapshot, const ScheduledEmitter& scheduled,
                               uint32_t textureSlot);

} // namespace particles
