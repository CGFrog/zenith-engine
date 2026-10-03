#pragma once

// Private to the physics subsystem: includes Jolt headers.
#include <Jolt/Jolt.h>

// Jolt/Jolt.h defines configuration macros required by all other Jolt headers.
// Jolt publishes JPH_DEBUG_RENDERER to its users with the same value it was
// built with; the root CMakeLists.txt enables it in every configuration.
#ifndef JPH_DEBUG_RENDERER
#error "JoltDebugDraw needs Jolt built with its debug renderer (DEBUG_RENDERER_IN_DISTRIBUTION)"
#endif

#include <Jolt/Renderer/DebugRendererSimple.h>

#include "engine/graphics/DebugDraw.hpp"

// Forwards Jolt's debug rendering (PhysicsSystem::DrawBodies and friends) into
// a DebugDraw: lines and triangle edges become debug lines and 3D text becomes
// labels. Jolt colours are treated as display-referred.
//
// Jolt allows one DebugRenderer per process (JPH::DebugRenderer::sInstance),
// so a JoltDebugDraw must not be created while another debug renderer exists.
// Constructing it builds Jolt's shared debug geometry, so keep it alive
// rather than creating one per frame: JoltRuntimeLease owns the one every
// PhysicsWorld draws through. Use from one thread at a time.
class JoltDebugDraw final : public JPH::DebugRendererSimple {
  public:
    // Primitives go to `target` (null drops them) with the given depth mode.
    void setTarget(DebugDraw* target, DebugDepth depth) {
        m_target = target;
        m_depth = depth;
    }

    void DrawLine(JPH::RVec3Arg from, JPH::RVec3Arg to, JPH::ColorArg color) override;
    void DrawText3D(JPH::RVec3Arg position, const std::string_view& text, JPH::ColorArg color, float height) override;

  private:
    DebugDraw* m_target{nullptr};
    DebugDepth m_depth{DebugDepth::Tested};
};
