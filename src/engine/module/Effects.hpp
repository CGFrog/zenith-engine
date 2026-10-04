#pragma once

#include "engine/scene/Scene.hpp"
#include "engine/scene/SceneEntityId.hpp"

#include <cstddef>
#include <filesystem>
#include <glm/glm.hpp>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

class EntityCommandBuffer;
class LogChannel;
class Timers;

struct EffectOptions {
    // Simulated seconds until the effect ends. Empty derives it from the
    // prefab's particle emitters: the longest particle lifetime of its
    // one-shot emitters (no spawn rate, no repeating bursts). A prefab with a
    // continuous or repeating emitter, or none, needs an explicit lifetime.
    std::optional<float> lifetime;
    // When the effect ends, park the instance (its emitters stopped) and reuse
    // it for the next pooled spawn of the same prefab instead of destroying
    // it. Spawning and destroying entities restarts every particle emitter in
    // the scene, which pooling avoids. Parked instances stay in the scene, so
    // pool particle effects rather than effects with meshes.
    bool pooled{false};
};

// Short-lived effects for a gameplay session (GameServices::effects): a
// prefab placed at a transform that removes itself when it ends. Spawns are
// requested at any time and applied at the session's next flush; an effect
// ends on the session's timers, so it follows the simulation clock's time
// scale and pause. An effect whose entity is destroyed early is forgotten.
class Effects {
  public:
    Effects(EntityCommandBuffer& commands, Timers& timers, const LogChannel& log);

    Effects(const Effects&) = delete;
    Effects& operator=(const Effects&) = delete;

    // Places the prefab (a project-relative reference) with its root at the
    // world transform.
    void spawn(std::filesystem::path prefab, const glm::mat4& transform, EffectOptions options = {});

    // Effects playing now, and instances parked for prefab.
    size_t activeCount() const;
    size_t parkedCount(const std::filesystem::path& prefab) const;

    // Applies the requested spawns: reuses parked instances directly and
    // records new instances on the command buffer. GameServices::flush calls
    // it before applying the commands.
    void flush(Scene& scene);
    // Forgets every request, effect, and parked instance (not their entities).
    void clear();

  private:
    struct Request {
        std::filesystem::path prefab;
        glm::mat4 transform{1.0f};
        EffectOptions options;
    };

    struct Parked {
        SceneEntityId root;
        float lifetime{0.0f};
    };

    void spawned(Scene& scene, Entity root, const std::string& pool, const EffectOptions& options);
    void play(Scene& scene, SceneEntityId root, const std::string& pool, float lifetime, bool pooled);
    void finish(Scene& scene, SceneEntityId root, const std::string& pool, float lifetime, bool pooled);

    EntityCommandBuffer& m_commands;
    Timers& m_timers;
    const LogChannel& m_log;
    std::vector<Request> m_requests;
    std::unordered_set<uint64_t> m_active;
    // By the prefab's generic path.
    std::map<std::string, std::vector<Parked>, std::less<>> m_parked;
};

// The lifetime EffectOptions derives for the effect rooted at root, or
// nullopt (with the reason in error) when it has a continuous or repeating
// particle emitter, or none.
std::optional<float> derivedEffectLifetime(const Scene& scene, Entity root, std::string& error);
