#pragma once

// Private to the physics subsystem.

class JoltDebugDraw;

// A share of Jolt's process-wide state: the default allocator hooks, the
// factory with Jolt's registered types (JPH::Factory::sInstance and the
// default physics material), and the single debug renderer Jolt allows
// (JPH::DebugRenderer::sInstance). Jolt keeps these in globals, so every
// initialized PhysicsWorld holds a lease on one shared runtime instead of
// creating and destroying them itself: the first lease creates the runtime,
// the last one released destroys it, and worlds can therefore be initialized
// and shut down in any order. The lease count, the debug renderer, and the
// mutex guarding them are the only mutable process-wide state Zenith adds to
// Jolt's own.
//
// Move-only. A default-constructed lease holds nothing.
class JoltRuntimeLease {
  public:
    JoltRuntimeLease() = default;
    ~JoltRuntimeLease();
    JoltRuntimeLease(JoltRuntimeLease&& other) noexcept;
    JoltRuntimeLease& operator=(JoltRuntimeLease&& other) noexcept;
    JoltRuntimeLease(const JoltRuntimeLease&) = delete;
    JoltRuntimeLease& operator=(const JoltRuntimeLease&) = delete;

    // Creates the runtime if no lease is held yet. Thread safe.
    static JoltRuntimeLease acquire();
    // Gives up the share now; the runtime is destroyed with the last one.
    void release();

    explicit operator bool() const {
        return m_held;
    }

    // The runtime's debug renderer, created on first use and destroyed with
    // the runtime, so it outlives every world drawing through it. Null when
    // this lease is empty or when a debug renderer that is not Zenith's
    // already occupies Jolt's per-process slot. Drawing through it is not
    // thread safe: draw from one thread at a time.
    JoltDebugDraw* debugRenderer() const;

  private:
    bool m_held{false};
};
