#include "Timers.hpp"

#include "engine/scene/Scene.hpp"

#include <algorithm>

TimerHandle Timers::start(float seconds, TimerRepeat repeat, Callback callback, SceneEntityId owner) {
    if (!callback || (repeat == TimerRepeat::Repeat && !(seconds > 0.0f))) {
        return {};
    }
    const TimerHandle handle{m_nextHandle++};
    const double delay = std::max(static_cast<double>(seconds), 0.0);
    m_timers.push_back({.handle = handle,
                        .deadline = m_now + delay,
                        .interval = delay,
                        .repeat = repeat,
                        .owner = owner,
                        .callback = std::move(callback)});
    return handle;
}

bool Timers::cancel(TimerHandle timer) {
    return std::erase_if(m_timers, [timer](const Timer& entry) { return entry.handle == timer; }) != 0;
}

bool Timers::active(TimerHandle timer) const {
    return std::ranges::any_of(m_timers, [timer](const Timer& entry) { return entry.handle == timer; });
}

void Timers::advance(Scene& scene, float simulatedSeconds) {
    if (simulatedSeconds > 0.0f) {
        m_now += simulatedSeconds;
    }
    // Searched afresh after each callback, which may start or cancel timers.
    for (size_t index = nextDue(); index < m_timers.size(); index = nextDue()) {
        Timer& timer = m_timers[index];
        const TimerHandle handle = timer.handle;
        const SceneEntityId owner = timer.owner;
        if (owner && scene.findEntity(owner) == entt::null) {
            m_timers.erase(m_timers.begin() + static_cast<std::ptrdiff_t>(index));
            continue;
        }
        Callback callback;
        if (timer.repeat == TimerRepeat::Once) {
            callback = std::move(timer.callback);
            m_timers.erase(m_timers.begin() + static_cast<std::ptrdiff_t>(index));
        } else {
            // A copy: the callback may cancel its own timer.
            callback = timer.callback;
            timer.deadline += timer.interval;
        }
        TimerContext context{.scene = scene, .timer = handle, .owner = owner};
        callback(context);
    }
}

void Timers::clear() {
    m_timers.clear();
}

size_t Timers::size() const {
    return m_timers.size();
}

size_t Timers::nextDue() const {
    size_t due = m_timers.size();
    for (size_t index = 0; index < m_timers.size(); ++index) {
        const Timer& timer = m_timers[index];
        if (timer.deadline > m_now) {
            continue;
        }
        // Handles grow with start order, so they break deadline ties.
        if (due == m_timers.size() || timer.deadline < m_timers[due].deadline ||
            (timer.deadline == m_timers[due].deadline && timer.handle.value < m_timers[due].handle.value)) {
            due = index;
        }
    }
    return due;
}
