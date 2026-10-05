#pragma once

#include "engine/graphics/DebugDraw.hpp"
#include "engine/scene/Scene.hpp"

class GameServices;
class PhysicsWorld;

// The Play-mode debugging overlays of one viewport, which look at the running
// game: what its physics and navigation actually hold, rather than the
// authored components the scene gizmos draw.
struct RuntimeDebugOverlays {
    // The game's navigation mesh (GameServices::navigationIfBaked).
    bool navigationMesh{false};
    // Every physics body's collision shape (PhysicsWorld::drawDebug).
    bool physicsShapes{false};
    // The last step's begin and persist contacts: a point and its normal.
    bool contacts{false};
    // Character controller capsules, coloured grounded or airborne, with the
    // ground normal under grounded ones.
    bool characters{false};
    // Navigation agents: their radius, crowd velocity, and the path's end,
    // coloured walking, partial, or failed.
    bool navAgents{false};

    bool any() const {
        return navigationMesh || physicsShapes || contacts || characters || navAgents;
    }
};

// What the overlays look at. physics and services may be null (no world, or
// no game module); nothing is ever baked or stepped.
struct RuntimeDebugSources {
    const Scene& scene;
    const PhysicsWorld* physics{nullptr};
    const GameServices* services{nullptr};
};

// Records the enabled overlays into output (a viewport's own debug list) for
// one frame. The scene's world transforms must be current.
void recordRuntimeDebug(const RuntimeDebugSources& sources, const RuntimeDebugOverlays& overlays, DebugDraw& output);
