#include "engine/graphics/Particles.hpp"
#include "engine/graphics/RenderConfiguration.hpp"
#include "engine/scene/SceneDocument.hpp"

#include "support/Doctest.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_BEGIN("Particle");

namespace {

bool rejects(ParticleEmitterComponent emitter) {
    return !validateParticleEmitter(emitter).empty();
}

TEST_CASE("Particle: validation") {
    CHECK_MESSAGE(validateParticleEmitter(ParticleEmitterComponent{}).empty(), "the default emitter is valid");
    CHECK_MESSAGE(rejects({.maxParticles = 0}), "an emitter needs at least one particle");
    CHECK_MESSAGE(rejects({.maxParticles = maximumParticlesPerEmitter + 1}), "emitters are capped per emitter");
    CHECK_MESSAGE(rejects({.spawnRate = -1.0f}), "negative spawn rates are rejected");
    CHECK_MESSAGE(rejects({.lifetimeMin = 0.0f}), "lifetimes must be positive");
    CHECK_MESSAGE(rejects({.lifetimeMin = 2.0f, .lifetimeMax = 1.0f}), "the lifetime range must be ordered");
    CHECK_MESSAGE(rejects({.direction = glm::vec3(0.0f)}), "the emission direction must be non-zero");
    CHECK_MESSAGE(rejects({.coneAngleRadians = 4.0f}), "cones wider than a sphere are rejected");
    CHECK_MESSAGE(rejects({.speedMin = 3.0f, .speedMax = 1.0f}), "the speed range must be ordered");
    CHECK_MESSAGE(rejects({.drag = -0.5f}), "negative drag is rejected");
    CHECK_MESSAGE(rejects({.gravity = glm::vec3(0.0f, std::nanf(""), 0.0f)}), "non-finite gravity is rejected");
    CHECK_MESSAGE(rejects({.startColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.5f)}), "opacity above one is rejected");
    CHECK_MESSAGE(rejects({.endSize = -1.0f}), "negative sizes are rejected");
    CHECK_MESSAGE(rejects({.softDistance = -0.1f}), "negative soft distances are rejected");
}

ParticleEmitterComponent authoredEmitter() {
    return {.maxParticles = 4096,
            .spawnRate = 250.0f,
            .burstCount = 40,
            .burstInterval = 1.5f,
            .lifetimeMin = 0.5f,
            .lifetimeMax = 1.25f,
            .direction = {0.0f, 0.0f, 1.0f},
            .coneAngleRadians = 0.75f,
            .speedMin = 2.0f,
            .speedMax = 4.0f,
            .spawnRadius = 0.3f,
            .gravity = {0.0f, -9.81f, 0.0f},
            .drag = 0.4f,
            .startSize = 0.05f,
            .endSize = 0.01f,
            .angularSpeed = 2.0f,
            .startColor = {1.0f, 0.6f, 0.2f, 1.0f},
            .endColor = {0.8f, 0.1f, 0.0f, 0.0f},
            .emissiveIntensity = 8.0f,
            .lit = true,
            .blendMode = ParticleBlendMode::Additive,
            .space = ParticleSimulationSpace::Local,
            .softDistance = 0.5f,
            .emitting = false};
}

TEST_CASE("Particle: scene round trip") {
    const std::filesystem::path assetRoot = ZENITH_ASSET_DIR;
    const SceneDocumentOptions options{.assetRoot = assetRoot};
    AssetManager assets;
    Scene scene;
    const Entity entity = scene.createEntity("Sparks");
    ParticleEmitterComponent emitter = authoredEmitter();
    const AssetLoadResult<TextureHandle> texture = assets.loadTexture(assetRoot / "textures/wood_siding_basecolor.png");
    CHECK_MESSAGE((static_cast<bool>(texture)), texture.error);
    emitter.texture = texture.handle;
    scene.registry().emplace<ParticleEmitterComponent>(entity, emitter);

    std::string document;
    CHECK_MESSAGE(SceneDocument::serialize(scene, assets, options, document).success, "emitter scenes serialize");
    Scene restored;
    const SceneDocumentResult loaded = SceneDocument::deserialize(document, assets, options, restored);
    CHECK_MESSAGE(loaded.success, loaded.error);
    const Entity restoredEntity = restored.findEntity(scene.id(entity));
    const auto* restoredEmitter =
        restoredEntity != entt::null ? restored.registry().try_get<ParticleEmitterComponent>(restoredEntity) : nullptr;
    CHECK_MESSAGE(restoredEmitter != nullptr, "the emitter component survives a round trip");
    if (restoredEmitter != nullptr) {
        const ParticleEmitterComponent& value = *restoredEmitter;
        CHECK_MESSAGE((value.maxParticles == 4096 && value.spawnRate == 250.0f && value.burstCount == 40 &&
                       value.burstInterval == 1.5f && value.lifetimeMin == 0.5f && value.lifetimeMax == 1.25f),
                      "spawn and lifetime parameters round trip");
        CHECK_MESSAGE((value.direction == glm::vec3(0.0f, 0.0f, 1.0f) && value.coneAngleRadians == 0.75f &&
                       value.speedMin == 2.0f && value.speedMax == 4.0f && value.spawnRadius == 0.3f &&
                       value.gravity == glm::vec3(0.0f, -9.81f, 0.0f) && value.drag == 0.4f),
                      "motion parameters round trip");
        CHECK_MESSAGE((value.startSize == 0.05f && value.endSize == 0.01f && value.angularSpeed == 2.0f &&
                       value.startColor == glm::vec4(1.0f, 0.6f, 0.2f, 1.0f) &&
                       value.endColor == glm::vec4(0.8f, 0.1f, 0.0f, 0.0f) && value.emissiveIntensity == 8.0f),
                      "appearance over life round trips");
        CHECK_MESSAGE((value.lit && value.blendMode == ParticleBlendMode::Additive &&
                       value.space == ParticleSimulationSpace::Local && value.softDistance == 0.5f && !value.emitting),
                      "shading and simulation modes round trip");
        CHECK_MESSAGE((static_cast<bool>(value.texture) && value.textureAsset == "textures/wood_siding_basecolor.png"),
                      "the sprite texture round trips as a project-relative asset");
    }
    std::string again;
    CHECK_MESSAGE((SceneDocument::serialize(restored, assets, options, again).success && again == document),
                  "serializing after a load reproduces the document");

    const auto load = [&](std::string_view components) {
        Scene target;
        const std::string text =
            std::string(
                R"({"version":1,"entities":[{"id":1,"name":"e","local_transform":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"parent":null,"components":{"particle_emitter":)") +
            std::string(components) + "}}]}";
        const SceneDocumentResult result = SceneDocument::deserialize(text, assets, options, target);
        const Entity found = target.findEntity(SceneEntityId{1});
        const auto* component =
            found != entt::null ? target.registry().try_get<ParticleEmitterComponent>(found) : nullptr;
        return std::pair{result.success, component ? *component : ParticleEmitterComponent{}};
    };
    const auto [minimalLoaded, minimal] = load("{}");
    CHECK_MESSAGE((minimalLoaded && minimal.maxParticles == ParticleEmitterComponent{}.maxParticles &&
                   minimal.blendMode == ParticleBlendMode::Alpha && minimal.textureAsset.empty() && !minimal.texture),
                  "every particle_emitter field is optional");
    CHECK_MESSAGE(!load(R"({"blend_mode":"multiply"})").first, "unknown blend modes are rejected");
    CHECK_MESSAGE(!load(R"({"space":"screen"})").first, "unknown simulation spaces are rejected");
    CHECK_MESSAGE(!load(R"({"lifetime":[2,1]})").first, "invalid lifetimes are rejected on load");
    CHECK_MESSAGE(!load(R"({"start_color":[1,1,1]})").first, "colors need four components");
    CHECK_MESSAGE(!load(R"({"texture_asset":"textures/missing.png"})").first, "missing sprite textures are rejected");
}

TEST_CASE("Particle: render settings") {
    RenderSettings settings;
    CHECK_MESSAGE(settings.particles == ParticleQuality::Low, "particles default to the Low budget");
    settings.particles = ParticleQuality::High;
    settings.budgets.particleSimulation = {0.1, 0.4};
    settings.budgets.particleDraw = {0.05, 0.6};
    std::string document;
    RenderSettings restored;
    CHECK_MESSAGE(
        (serializeRenderSettings(settings, document).success && deserializeRenderSettings(document, restored).success &&
         restored.particles == ParticleQuality::High && restored.budgets.particleSimulation.gpuMilliseconds == 0.4 &&
         restored.budgets.particleDraw.gpuMilliseconds == 0.6),
        "the particle quality and budgets round trip");
    RenderSettings legacy;
    CHECK_MESSAGE((deserializeRenderSettings(R"({"version":1,"antialiasing":1,"shadows":1,"lighting":1,"bloom":0,)"
                                             R"("environment":0,"localLights":1})",
                                             legacy)
                       .success &&
                   legacy.particles == ParticleQuality::Low),
                  "documents without a particle quality keep the default");
    RenderSettings invalid;
    CHECK_MESSAGE(!deserializeRenderSettings(R"({"version":1,"antialiasing":1,"shadows":1,"lighting":1,"bloom":0,)"
                                             R"("environment":0,"localLights":1,"particles":3})",
                                             invalid)
                       .success,
                  "unknown particle qualities are rejected");
    RenderCapabilities capabilities;
    capabilities.particles = false;
    const ValidatedRenderSettings validated = validateRenderSettings(settings, capabilities);
    CHECK_MESSAGE((validated.settings.particles == ParticleQuality::Off && !validated.fallbacks.empty()),
                  "devices without particle support fall back to Off with an explanation");
    CHECK_MESSAGE((particles::particleBudget(ParticleQuality::Off) == 0 &&
                   particles::particleBudget(ParticleQuality::Low) == 16384 &&
                   particles::particleBudget(ParticleQuality::High) == 131072),
                  "quality budgets are 0, 16384, and 131072 particles");
}

TEST_CASE("Particle: sort plan") {
    using particles::SortPass;
    using particles::SortStep;
    CHECK_MESSAGE((particles::sortElementCount(0) == 1024 && particles::sortElementCount(1024) == 1024 &&
                   particles::sortElementCount(1025) == 2048 && particles::sortElementCount(131072) == 131072),
                  "sorts cover the live particles in power-of-two blocks of at least 1024 keys");
    CHECK_MESSAGE((particles::bitonicSortPlan(1024) == std::vector<SortPass>{{SortStep::LocalSort, 1024, 0}}),
                  "one block sorts in a single shared-memory dispatch");
    CHECK_MESSAGE((particles::bitonicSortPlan(2048) == std::vector<SortPass>{{SortStep::LocalSort, 1024, 0},
                                                                             {SortStep::GlobalMerge, 2048, 1024},
                                                                             {SortStep::LocalMerge, 2048, 512}}),
                  "two blocks merge with one global step and one local merge");
    // log2(n / 1024) merge sizes, each with its global steps and one local merge.
    CHECK_MESSAGE(particles::bitonicSortPlan(131072).size() == 1 + (1 + 2 + 3 + 4 + 5 + 6 + 7) + 7,
                  "the High budget sorts in 36 dispatches");
    CHECK_MESSAGE(particles::sortDispatchGroups(131072) == 128, "every sort dispatch covers 1024 keys per workgroup");

    std::mt19937 random(1234);
    for (uint32_t elements : {1024u, 4096u, 16384u}) {
        std::vector<particles::SortKey> keys(elements);
        for (uint32_t index = 0; index < elements; ++index) {
            // Few distinct keys so the index tie-break matters; a tail of padding.
            keys[index] = index + 100 < elements ? particles::SortKey{random() % 97u, index} : particles::SortKey{};
        }
        std::vector<particles::SortKey> expected = keys;
        std::ranges::sort(expected);
        particles::sortReference(keys);
        CHECK_MESSAGE(keys == expected, "the bitonic network sorts keys ascending by key then index");
    }
    CHECK_MESSAGE((particles::backToFrontKey(20.0f) < particles::backToFrontKey(10.0f) &&
                   particles::backToFrontKey(10.0f) < particles::backToFrontKey(0.5f) &&
                   particles::backToFrontKey(-3.0f) == particles::backToFrontKey(0.0f)),
                  "ascending keys put farther particles first and clamp depths behind the eye");
}

particles::EmitterSnapshot snapshot(uint64_t id, ParticleEmitterComponent emitter) {
    return {.id = id, .emitter = std::move(emitter)};
}

TEST_CASE("Particle: scheduler layout") {
    particles::ParticleScheduler scheduler;
    const std::vector<particles::EmitterSnapshot> emitters{
        snapshot(1, {.maxParticles = 30, .spawnRate = 0.0f}), snapshot(3, {.maxParticles = 60, .spawnRate = 0.0f}),
        snapshot(7, {.maxParticles = 50, .spawnRate = 0.0f}), snapshot(9, {.maxParticles = 5, .spawnRate = 0.0f})};
    const particles::ParticleFramePlan plan = scheduler.plan(emitters, 100, 0.016f);
    CHECK_MESSAGE(plan.resetPool, "the first layout resets the pool");
    CHECK_MESSAGE((plan.emitters.size() == 3 && plan.emitters[0].firstSlot == 0 && plan.emitters[0].capacity == 30 &&
                   plan.emitters[1].firstSlot == 30 && plan.emitters[1].capacity == 60 &&
                   plan.emitters[2].firstSlot == 90 && plan.emitters[2].capacity == 10 && plan.poolSlots == 100),
                  "emitters take consecutive slots in id order until the budget runs out");
    CHECK_MESSAGE(!scheduler.plan(emitters, 100, 0.016f).resetPool, "an unchanged layout keeps the pool");
    std::vector<particles::EmitterSnapshot> resized = emitters;
    resized[0].emitter.maxParticles = 20;
    CHECK_MESSAGE(scheduler.plan(resized, 100, 0.016f).resetPool,
                  "a capacity change that no free range of a full budget fits compacts and resets the pool");
    CHECK_MESSAGE(scheduler.plan(resized, 200, 0.016f).resetPool, "changing the budget resets the pool");
    resized[1].emitter.space = ParticleSimulationSpace::Local;
    const particles::ParticleFramePlan respaced = scheduler.plan(resized, 200, 0.016f);
    CHECK_MESSAGE(
        (!respaced.resetPool && respaced.clearRanges == std::vector<particles::SlotRange>{{.first = 20, .count = 60}}),
        "changing a simulation space clears only that emitter's slots (live positions are in the old space)");
    CHECK_MESSAGE(
        (scheduler.plan(resized, 200, 0.016f, 7).resetPool && !scheduler.plan(resized, 200, 0.016f, 7).resetPool),
        "a new scene content generation resets the pool once");
    Scene scene;
    const uint64_t generation = scene.contentGeneration();
    scene.clear();
    Scene replacement;
    const uint64_t cleared = scene.contentGeneration();
    scene.replaceWith(std::move(replacement));
    CHECK_MESSAGE((cleared != generation && scene.contentGeneration() != cleared),
                  "clearing or replacing a scene changes its content generation");
    CHECK_MESSAGE(scheduler.plan({}, 200, 0.016f).emitters.empty(), "no emitters schedule nothing");

    std::vector<particles::EmitterSnapshot> many;
    for (uint64_t id = 1; id <= particles::maximumEmitters + 4; ++id) {
        many.push_back(snapshot(id, {.maxParticles = 1}));
    }
    const particles::ParticleFramePlan capped = scheduler.plan(many, 1000, 0.016f);
    CHECK_MESSAGE((capped.emitters.size() == particles::maximumEmitters &&
                   capped.emitters.back().id == particles::maximumEmitters),
                  "at most maximumEmitters emitters are simulated, the lowest scene ids first");
    CHECK_MESSAGE(capped.droppedEmitters == 4, "emitters beyond maximumEmitters are counted as dropped");
    CHECK_MESSAGE(scheduler.plan(resized, 1000, 0.016f).droppedEmitters == 0, "a plan within the cap drops no emitter");
}

particles::ScheduledEmitter scheduledEmitter(const particles::ParticleFramePlan& plan, uint64_t id) {
    for (const particles::ScheduledEmitter& emitter : plan.emitters) {
        if (emitter.id == id) {
            return emitter;
        }
    }
    return {};
}

// Adding or removing one emitter leaves the others' slots, table entries,
// rings, and spawn accumulators alone; only the affected slots are cleared.
TEST_CASE("Particle: scheduler keeps unaffected emitters") {
    particles::ParticleScheduler scheduler;
    // 3 particles per second with 0.25 s steps: the accumulator carries
    // fractions, so a restart would show up in the spawn counts.
    const ParticleEmitterComponent stream{.maxParticles = 10, .spawnRate = 3.0f};
    std::vector<particles::EmitterSnapshot> emitters{snapshot(2, stream), snapshot(5, stream), snapshot(8, stream)};
    particles::ParticleFramePlan plan = scheduler.plan(emitters, 100, 0.25f);
    CHECK_MESSAGE(
        (plan.resetPool && scheduledEmitter(plan, 8).firstSlot == 20 && scheduledEmitter(plan, 8).emitterIndex == 2),
        "the first layout packs emitters in id order");
    plan = scheduler.plan(emitters, 100, 0.25f); // accumulators at 0.5 after 0.75 + 0.75 spawns 1
    const particles::ScheduledEmitter before = scheduledEmitter(plan, 8);

    // Add emitter 3 (between existing ids) and remove emitter 5.
    std::vector<particles::EmitterSnapshot> changed{snapshot(2, stream), snapshot(3, stream), snapshot(8, stream)};
    plan = scheduler.plan(changed, 100, 0.25f);
    const particles::ScheduledEmitter kept = scheduledEmitter(plan, 8);
    const particles::ScheduledEmitter added = scheduledEmitter(plan, 3);
    CHECK_MESSAGE(!plan.resetPool, "adding and removing emitters keeps the pool");
    CHECK_MESSAGE((kept.firstSlot == before.firstSlot && kept.capacity == before.capacity &&
                   kept.emitterIndex == before.emitterIndex && kept.ringStart == before.ringStart + before.spawnCount &&
                   kept.firstOrdinal == before.firstOrdinal + before.spawnCount),
                  "an unaffected emitter keeps its slots, table entry, and ring");
    CHECK_MESSAGE((kept.spawnCount == 1 && scheduledEmitter(plan, 2).spawnCount == 1),
                  "an unaffected emitter keeps its spawn accumulator (0.5 + 0.75 spawns 1)");
    CHECK_MESSAGE(
        (added.firstSlot == 10 && added.emitterIndex == 1 && added.firstOrdinal == 0 && added.spawnCount == 0),
        "a new emitter takes the first free range and table entry and starts fresh");
    CHECK_MESSAGE((plan.clearRanges == std::vector<particles::SlotRange>{{.first = 10, .count = 10}}),
                  "the removed emitter's slots, now the new emitter's, are cleared");
    CHECK_MESSAGE((plan.emitterTableSize == 3 && plan.poolSlots == 30),
                  "the table and pool cover every scheduled emitter");

    // Removing the last emitter frees its slots and shrinks nothing else.
    changed.pop_back();
    plan = scheduler.plan(changed, 100, 0.25f);
    CHECK_MESSAGE((!plan.resetPool &&
                   plan.clearRanges == std::vector<particles::SlotRange>{{.first = 20, .count = 10}} &&
                   plan.poolSlots == 20 && plan.emitterTableSize == 2),
                  "a removed emitter's slots are cleared and leave the simulated prefix");

    // A larger emitter that does not fit a gap goes after the others.
    changed.push_back(snapshot(9, {.maxParticles = 40, .spawnRate = 3.0f}));
    plan = scheduler.plan(changed, 100, 0.25f);
    CHECK_MESSAGE(
        (!plan.resetPool && scheduledEmitter(plan, 9).firstSlot == 20 && scheduledEmitter(plan, 9).emitterIndex == 2),
        "a new emitter takes the first range that fits");

    // Fragmentation that leaves no range large enough compacts the layout.
    changed.erase(changed.begin()); // frees slots 0-9
    changed.push_back(snapshot(11, {.maxParticles = 45, .spawnRate = 3.0f}));
    plan = scheduler.plan(changed, 100, 0.25f);
    CHECK_MESSAGE((plan.resetPool && plan.clearRanges.empty() && scheduledEmitter(plan, 3).firstSlot == 0 &&
                   scheduledEmitter(plan, 9).firstSlot == 10 && scheduledEmitter(plan, 11).firstSlot == 50),
                  "when no free range fits, emitters are packed again in id order and the pool resets");

    // The layout is a function of the plan sequence alone.
    particles::ParticleScheduler replay;
    replay.plan(emitters, 100, 0.25f);
    replay.plan(emitters, 100, 0.25f);
    const particles::ParticleFramePlan replayed = replay.plan(
        std::vector<particles::EmitterSnapshot>{snapshot(2, stream), snapshot(3, stream), snapshot(8, stream)}, 100,
        0.25f);
    CHECK_MESSAGE((scheduledEmitter(replayed, 3).firstSlot == 10 && scheduledEmitter(replayed, 8).firstSlot == 20),
                  "the same plan sequence produces the same layout");

    // Emptying the set and starting again rebuilds from scratch.
    CHECK_MESSAGE((scheduler.plan({}, 100, 0.25f).emitters.empty() && scheduler.plan(emitters, 100, 0.25f).resetPool),
                  "a plan after an empty one resets the pool, forgetting particles of emitters that left meanwhile");
}

// A pooled effect restarts its emitter by changing restartGeneration: the
// first burst fires again without a layout change.
TEST_CASE("Particle: scheduler restart") {
    particles::ParticleScheduler scheduler;
    std::vector<particles::EmitterSnapshot> once{snapshot(4, {.maxParticles = 16, .spawnRate = 0.0f, .burstCount = 5})};
    CHECK_MESSAGE(scheduler.plan(once, 64, 0.25f).totalSpawns == 5, "a one-shot emitter bursts when it starts");
    CHECK_MESSAGE(scheduler.plan(once, 64, 0.25f).totalSpawns == 0, "a one-shot emitter bursts once");
    ++once[0].emitter.restartGeneration;
    const particles::ParticleFramePlan restarted = scheduler.plan(once, 64, 0.25f);
    CHECK_MESSAGE((restarted.totalSpawns == 5 && !restarted.resetPool),
                  "a new restart generation fires the burst again without resetting the pool");
    CHECK_MESSAGE(scheduler.plan(once, 64, 0.25f).totalSpawns == 0, "a restart bursts once");
}

TEST_CASE("Particle: scheduler spawns") {
    particles::ParticleScheduler scheduler;
    // Rates and steps are exact in binary so the expected counts are exact.
    const std::vector<particles::EmitterSnapshot> stream{
        snapshot(5, {.maxParticles = 4, .spawnRate = 6.0f, .burstCount = 3, .burstInterval = 0.5f})};
    particles::ParticleFramePlan plan = scheduler.plan(stream, 64, 0.25f);
    CHECK_MESSAGE((plan.totalSpawns == 4 && plan.emitters[0].ringStart == 0 && plan.emitters[0].firstOrdinal == 0),
                  "the first frame spawns the burst plus the accumulated rate (3 + 1.5 -> 4)");
    CHECK_MESSAGE((plan.emitters[0].liveBound == 4 && plan.liveBound == 4), "the live bound follows the spawned count");
    plan = scheduler.plan(stream, 64, 0.25f);
    CHECK_MESSAGE((plan.totalSpawns == 2 && plan.emitters[0].ringStart == 0 && plan.emitters[0].firstOrdinal == 4),
                  "fractional spawns carry over (0.5 + 1.5) and the ring wraps to its start");
    plan = scheduler.plan(stream, 64, 0.25f);
    CHECK_MESSAGE((plan.totalSpawns == 4 && plan.emitters[0].ringStart == 2 && plan.emitters[0].firstOrdinal == 6),
                  "a burst fires every burstInterval");
    CHECK_MESSAGE(plan.emitters[0].liveBound == 4, "the live bound never exceeds the ring");

    particles::ParticleScheduler oversized;
    const std::vector<particles::EmitterSnapshot> burst{
        snapshot(2, {.maxParticles = 8, .spawnRate = 0.0f, .burstCount = 50})};
    CHECK_MESSAGE(oversized.plan(burst, 64, 0.016f).totalSpawns == 8, "a frame never spawns more than the ring holds");

    particles::ParticleScheduler paused;
    std::vector<particles::EmitterSnapshot> idle{
        snapshot(2, {.spawnRate = 100.0f, .burstCount = 7, .emitting = false})};
    CHECK_MESSAGE(paused.plan(idle, 64, 0.5f).totalSpawns == 0, "an emitter that is not emitting spawns nothing");
    idle[0].emitter.emitting = true;
    CHECK_MESSAGE(paused.plan(idle, 64, 0.0f).totalSpawns == 7, "the start burst fires when emission starts");

    particles::ParticleScheduler two;
    const std::vector<particles::EmitterSnapshot> pair{snapshot(1, {.spawnRate = 0.0f, .burstCount = 3}),
                                                       snapshot(2, {.spawnRate = 0.0f, .burstCount = 5})};
    plan = two.plan(pair, 1 << 14, 0.016f);
    CHECK_MESSAGE((plan.totalSpawns == 8 && plan.emitters[0].spawnOffset == 0 && plan.emitters[1].spawnOffset == 3),
                  "each emitter's spawns follow the previous emitter's in the emit dispatch");
}

TEST_CASE("Particle: pack emitter") {
    ParticleEmitterComponent emitter = authoredEmitter();
    emitter.gravity = {0.0f, -10.0f, 0.0f};
    emitter.direction = {0.0f, 0.0f, 2.0f};
    emitter.coneAngleRadians = 0.0f;
    const particles::EmitterSnapshot local{
        .id = 42, .emitter = emitter, .world = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f))};
    const particles::ScheduledEmitter scheduled{.id = 42,
                                                .firstSlot = 16,
                                                .capacity = 32,
                                                .ringStart = 5,
                                                .spawnCount = 3,
                                                .spawnOffset = 9,
                                                .firstOrdinal = 77};
    const particles::GpuParticleEmitter packed = particles::packEmitter(local, scheduled, 3);
    CHECK_MESSAGE(packed.gravityDrag == glm::vec4(0.0f, -5.0f, 0.0f, 0.4f),
                  "local emitters receive gravity in entity space and their drag");
    CHECK_MESSAGE(packed.direction == glm::vec4(0.0f, 0.0f, 1.0f, 1.0f), "the cone axis is normalized with its cosine");
    CHECK_MESSAGE(packed.spawn.w == (particles::flagAdditive | particles::flagLit | particles::flagLocalSpace |
                                     (3u << particles::textureSlotShift)),
                  "flags carry blend mode, lighting, space, and sprite slot");
    CHECK_MESSAGE((packed.pool == glm::uvec4(16u, 32u, 5u, 3u) && packed.spawn.x == 9 && packed.spawn.y == 77 &&
                   packed.spawn.z == particles::emitterSeed(42)),
                  "the pool range, spawns, and seed come from the schedule");
    emitter.space = ParticleSimulationSpace::World;
    const particles::GpuParticleEmitter world =
        particles::packEmitter({.id = 42, .emitter = emitter, .world = local.world}, scheduled, 0);
    CHECK_MESSAGE((world.gravityDrag.y == -10.0f && (world.spawn.w & particles::flagLocalSpace) == 0),
                  "world emitters keep world gravity");
    CHECK_MESSAGE((particles::emitterSeed(1) != particles::emitterSeed(2) &&
                   particles::emitterSeed(9) == particles::emitterSeed(9)),
                  "seeds are stable per id and differ between ids");
}

TEST_CASE("Particle: gather emitters") {
    Scene scene;
    const Entity first = scene.createEntity("first");
    const Entity invalid = scene.createEntity("invalid");
    const Entity second = scene.createEntity("second");
    scene.registry().emplace<ParticleEmitterComponent>(second);
    scene.registry().emplace<ParticleEmitterComponent>(invalid, ParticleEmitterComponent{.maxParticles = 0});
    scene.registry().emplace<ParticleEmitterComponent>(first);
    scene.registry().get<TransformComponent>(first).local[3] = glm::vec4(1.0f, 2.0f, 3.0f, 1.0f);
    scene.updateWorldTransforms();
    const std::vector<particles::EmitterSnapshot> emitters = particles::gatherEmitters(scene);
    CHECK_MESSAGE(
        (emitters.size() == 2 && emitters[0].id == scene.id(first).value && emitters[1].id == scene.id(second).value),
        "valid emitters are gathered in stable id order");
    CHECK_MESSAGE((!emitters.empty() && emitters[0].world[3] == glm::vec4(1.0f, 2.0f, 3.0f, 1.0f)),
                  "gathered emitters carry their world transform");
}
} // namespace

TEST_SUITE_END();
