#pragma once

#include "engine/scene/Scene.hpp"
#include "engine/scene/SceneEntityId.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <variant>
#include <vector>

class EventQueue;
class LogChannel;
class PrefabSystem;

// Where a spawned entity goes. parent zero (or one that no longer exists when
// the buffer flushes) means the scene root.
struct SpawnPlacement {
    SceneEntityId parent;
    // Relative to the parent; empty keeps the prefab's (or identity).
    std::optional<glm::mat4> localTransform;
};

// Runs when the buffer creates the entity, so a system can configure what it
// spawned. It must not keep the Entity beyond the call.
using SpawnedCallback = std::function<void(Scene& scene, Entity entity)>;

// Scene changes recorded while systems iterate the scene and applied together
// at a flush point, so a system never invalidates the view it is walking. The
// host flushes after each system phase (and after each fixed step's
// FixedUpdate); commands apply in the order they were recorded. A command
// whose entity no longer exists is skipped; one that fails is logged and
// skipped.
//
// Lifetime: callbacks usually hold game module code (see GameServices).
class EntityCommandBuffer {
  public:
    // Instantiates the prefab at a project-relative reference.
    void spawn(std::filesystem::path prefab, SpawnPlacement placement = {}, SpawnedCallback onSpawned = {});
    // Creates an entity with only a name, transform, and relationship.
    void spawnEmpty(std::string name, SpawnPlacement placement = {}, SpawnedCallback onSpawned = {});
    // Destroys the entity and its descendants.
    void destroy(SceneEntityId entity);
    // Moves child under parent (zero for the root), keeping its world transform.
    void reparent(SceneEntityId child, SceneEntityId parent);

    bool empty() const;
    size_t size() const;

    // Called with each entity a destroy command is about to destroy (its
    // descendants go with it), while it still exists.
    using DestroyingCallback = std::function<void(Scene& scene, Entity entity)>;

    // Applies and forgets every command, including commands recorded by the
    // callbacks it runs. Publishes EntitySpawned and EntityDestroyed to events.
    void flush(Scene& scene, PrefabSystem& prefabs, EventQueue& events, const LogChannel& log,
               const DestroyingCallback& destroying = {});
    void clear();

  private:
    struct Spawn {
        std::filesystem::path prefab;
        std::string name;
        SpawnPlacement placement;
        SpawnedCallback onSpawned;
    };

    struct Destroy {
        SceneEntityId entity;
    };

    struct Reparent {
        SceneEntityId child;
        SceneEntityId parent;
    };

    using Command = std::variant<Spawn, Destroy, Reparent>;

    void apply(Spawn& command, Scene& scene, PrefabSystem& prefabs, EventQueue& events, const LogChannel& log);

    std::vector<Command> m_commands;
};
