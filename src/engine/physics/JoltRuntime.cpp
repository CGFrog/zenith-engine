#include "JoltRuntime.hpp"

#include "JoltDebugDraw.hpp"

#include <Jolt/Jolt.h>

// Jolt/Jolt.h defines configuration macros required by all other Jolt headers.
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>

#include <memory>
#include <mutex>

namespace {
struct RuntimeState {
    std::mutex mutex;
    unsigned leases{0};
    // Created by the first debugRenderer() call; destroyed before the
    // factory, since it holds Jolt geometry.
    std::unique_ptr<JoltDebugDraw> debugRenderer;
};

// A function-local static so its lifetime does not depend on the order in
// which translation units initialize.
RuntimeState& runtimeState() {
    static RuntimeState state;
    return state;
}

void createRuntime() {
    JPH::RegisterDefaultAllocator();
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
}

void destroyRuntime(RuntimeState& state) {
    state.debugRenderer.reset();
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}
} // namespace

JoltRuntimeLease::~JoltRuntimeLease() {
    release();
}

JoltRuntimeLease::JoltRuntimeLease(JoltRuntimeLease&& other) noexcept
    : m_held(other.m_held) {
    other.m_held = false;
}

JoltRuntimeLease& JoltRuntimeLease::operator=(JoltRuntimeLease&& other) noexcept {
    if (this != &other) {
        release();
        m_held = other.m_held;
        other.m_held = false;
    }
    return *this;
}

JoltRuntimeLease JoltRuntimeLease::acquire() {
    RuntimeState& state = runtimeState();
    std::scoped_lock lock(state.mutex);
    if (state.leases == 0) {
        createRuntime();
    }
    ++state.leases;
    JoltRuntimeLease lease;
    lease.m_held = true;
    return lease;
}

void JoltRuntimeLease::release() {
    if (!m_held) {
        return;
    }
    m_held = false;
    RuntimeState& state = runtimeState();
    std::scoped_lock lock(state.mutex);
    if (--state.leases == 0) {
        destroyRuntime(state);
    }
}

JoltDebugDraw* JoltRuntimeLease::debugRenderer() const {
    if (!m_held) {
        return nullptr;
    }
    RuntimeState& state = runtimeState();
    std::scoped_lock lock(state.mutex);
    if (!state.debugRenderer) {
        if (JPH::DebugRenderer::sInstance != nullptr) {
            return nullptr;
        }
        state.debugRenderer = std::make_unique<JoltDebugDraw>();
    }
    return state.debugRenderer.get();
}
