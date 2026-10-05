#pragma once

// A dungeon floor built the way a roguelike builds one: a grid of room
// prefabs spawned through a session's EntityCommandBuffer, then torn down by
// destroying the rooms. Shared by FloorLifecycleTests (correctness) and
// FloorBenchmark (costs).

#include "engine/assets/Assets.hpp"
#include "engine/audio/AudioSystem.hpp"
#include "engine/graphics/DebugDraw.hpp"
#include "engine/module/BuiltInSystems.hpp"
#include "engine/module/GameServices.hpp"
#include "engine/physics/PhysicsComponents.hpp"
#include "engine/physics/PhysicsWorld.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/Prefab.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace floor_fixture {
// Static bodies in one room: the floor slab, four walls, and a pillar.
inline constexpr uint32_t bodiesPerRoom = 6;
inline constexpr float roomSize = 10.0f;

template <typename Result> void require(const Result& result, const std::string& what) {
    if (!result) {
        throw std::runtime_error(what + ": " + result.error);
    }
}

// A second of 48 kHz mono silence as 16-bit PCM.
inline void writeTorchSound(const std::filesystem::path& path) {
    const uint32_t samples = 48'000;
    std::ofstream output(path, std::ios::binary);
    const auto put16 = [&output](uint16_t value) {
        output.put(static_cast<char>(value & 0xff));
        output.put(static_cast<char>(value >> 8));
    };
    const auto put32 = [&put16](uint32_t value) {
        put16(static_cast<uint16_t>(value & 0xffff));
        put16(static_cast<uint16_t>(value >> 16));
    };
    output.write("RIFF", 4);
    put32(36 + samples * 2);
    output.write("WAVEfmt ", 8);
    put32(16);
    put16(1);
    put16(1);
    put32(48'000);
    put32(48'000 * 2);
    put16(2);
    put16(16);
    output.write("data", 4);
    put32(samples * 2);
    for (uint32_t index = 0; index < samples; ++index) {
        put16(0);
    }
}

inline Entity addStaticBox(Scene& scene, const std::string& name, Entity parent, glm::vec3 position,
                           glm::vec3 halfExtents) {
    const Entity entity = scene.createEntity(name);
    scene.registry().get<TransformComponent>(entity).local = glm::translate(glm::mat4(1.0f), position);
    scene.registry().emplace<ColliderComponent>(entity, BoxCollider{halfExtents});
    scene.registry().emplace<RigidBodyComponent>(entity);
    if (parent != entt::null) {
        scene.setParent(entity, parent);
    }
    return entity;
}

// Writes room.prefab and torch.wav into assetRoot: a walled room with a
// pillar, a torch light, and a looping torch sound.
inline void writeRoomPrefab(const std::filesystem::path& assetRoot, AssetManager& assets,
                            const SceneDocumentOptions& documents) {
    writeTorchSound(assetRoot / "torch.wav");
    Scene authoring;
    const float half = roomSize * 0.5f;
    const Entity room = addStaticBox(authoring, "Room", entt::null, {0.0f, -0.1f, 0.0f}, {half, 0.1f, half});
    addStaticBox(authoring, "North wall", room, {0.0f, 1.5f, -half}, {half, 1.5f, 0.1f});
    addStaticBox(authoring, "South wall", room, {0.0f, 1.5f, half}, {half, 1.5f, 0.1f});
    addStaticBox(authoring, "East wall", room, {half, 1.5f, 0.0f}, {0.1f, 1.5f, half});
    addStaticBox(authoring, "West wall", room, {-half, 1.5f, 0.0f}, {0.1f, 1.5f, half});
    addStaticBox(authoring, "Pillar", room, {2.0f, 1.5f, 2.0f}, {0.5f, 1.5f, 0.5f});
    const Entity torch = authoring.createEntity("Torch");
    authoring.registry().get<TransformComponent>(torch).local =
        glm::translate(glm::mat4(1.0f), glm::vec3(-3.0f, 2.0f, -3.0f));
    authoring.registry().emplace<PointLightComponent>(torch);
    auto& sound = authoring.registry().emplace<AudioEmitterComponent>(torch);
    sound.clipAsset = "torch.wav";
    sound.loop = true;
    authoring.setParent(torch, room);

    PrefabSystem prefabs(assets, documents);
    PrefabCapture capture;
    require(prefabs.capture(authoring, room, "room.prefab", capture), "capture the room prefab");
    require(savePrefab(assetRoot / "room.prefab", capture.document), "save the room prefab");
}

// One gameplay session over a scene: services with audio, physics, and the
// engine's built-in systems, with a persistent player body that floors must
// leave alone. The player is kinematic, so it is never walkable geometry.
struct FloorSession {
    explicit FloorSession(const std::filesystem::path& assetRoot)
        : documents{.assetRoot = assetRoot, .componentTypes = builtInComponentTypes()} {
        require(physics.init(), "initialize physics");
        require(audio.init(AudioConfig{.maxVoices = 256, .outputEnabled = false}), "initialize audio");
        writeRoomPrefab(assetRoot, assets, documents);
        services = std::make_unique<GameServices>(assets, documents, debugDraw, &logger);
        services->setAudio(&audio);
        const Entity body = addStaticBox(scene, "Player", entt::null, {0.0f, 50.0f, 0.0f}, glm::vec3(0.4f));
        auto& rigidBody = scene.registry().get<RigidBodyComponent>(body);
        rigidBody.motionType = PhysicsMotionType::Kinematic;
        rigidBody.layer = PhysicsLayer::Player;
        player = scene.id(body);
    }

    // Spawns rooms in a grid of columns, recording their ids, and applies
    // the commands as a phase's flush would.
    void spawnFloor(uint32_t roomCount, uint32_t columns) {
        for (uint32_t index = 0; index < roomCount; ++index) {
            const glm::vec3 position(static_cast<float>(index % columns) * roomSize, 0.0f,
                                     static_cast<float>(index / columns) * roomSize);
            services->commands().spawn(
                "room.prefab", {.localTransform = glm::translate(glm::mat4(1.0f), position)},
                [this](Scene& spawnedIn, Entity entity) { rooms.push_back(spawnedIn.id(entity)); });
        }
        services->flush(scene);
    }

    void destroyFloor() {
        for (const SceneEntityId room : rooms) {
            services->commands().destroy(room);
        }
        rooms.clear();
        services->flush(scene);
    }

    // What the host does after the scene changed: a physics step (bodies are
    // created and removed there), the LateUpdate systems (emitters load their
    // clips), and the frame's audio sync.
    void step() {
        require(physics.step(scene, assets, physics.config().fixedTimeStep), "step physics");
        runGameplayPhase(systems, SystemPhase::LateUpdate,
                         {.scene = scene, .mode = SimulationMode::Play, .logger = &logger, .services = services.get()});
        scene.updateWorldTransforms();
        audio.syncScene(scene);
    }

    // Voices holding a clip, summed over every decoded clip.
    uint32_t activeVoices() const {
        uint32_t voices = 0;
        for (const AudioAssetDiagnostic& diagnostic : audio.assetDiagnostics()) {
            voices += diagnostic.strongReferences;
        }
        return voices;
    }

    Logger logger;
    AssetManager assets{&logger};
    DebugDraw debugDraw;
    SceneDocumentOptions documents;
    PhysicsWorld physics;
    AudioSystem audio{&logger};
    std::unique_ptr<GameServices> services;
    const SystemScheduler systems{builtInSystems()};
    Scene scene;
    SceneEntityId player;
    std::vector<SceneEntityId> rooms;
};
} // namespace floor_fixture
