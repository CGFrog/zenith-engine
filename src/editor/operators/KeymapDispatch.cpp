#include "KeymapDispatch.hpp"

#include <algorithm>
#include <cmath>

namespace {
ImGuiKey chordKey(ImGuiKeyChord chord) {
    return static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
}

KeymapInvocation invocationOf(const KeymapBinding& binding) {
    return {.operatorId = binding.operatorId, .properties = binding.properties, .region = binding.region};
}
} // namespace

bool chordReachesGame(ImGuiKeyChord chord) {
    if ((chord & (ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiMod_Super)) != 0) {
        return false;
    }
    const ImGuiKey key = chordKey(chord);
    const bool functionKey = key >= ImGuiKey_F1 && key <= ImGuiKey_F24;
    return !functionKey && key != ImGuiKey_Escape;
}

KeymapGesture::KeymapGesture(KeymapRegion region, ImGuiKeyChord chord, double pressSeconds, ImVec2 pressMouse)
    : m_region(region)
    , m_chord(chord)
    , m_pressSeconds(pressSeconds)
    , m_pressMouse(pressMouse) {}

bool KeymapGesture::fire(const Keymap& keymap, KeyTrigger trigger, std::vector<KeymapInvocation>& fired) const {
    const KeymapBinding* binding = keymap.find(m_region, m_chord, trigger);
    if (binding == nullptr) {
        return false;
    }
    fired.push_back(invocationOf(*binding));
    return true;
}

std::vector<KeymapInvocation> KeymapGesture::advance(const Keymap& keymap, const KeymapGestureInput& input,
                                                     float dragThreshold) {
    std::vector<KeymapInvocation> fired;
    if (m_finished) {
        return fired;
    }
    if (!input.down) {
        m_finished = true;
        fire(keymap, KeyTrigger::Release, fired);
        if (!m_moved && !m_holdRan) {
            fire(keymap, KeyTrigger::Click, fired);
        }
        return fired;
    }
    const float distance = std::hypot(input.mouse.x - m_pressMouse.x, input.mouse.y - m_pressMouse.y);
    if (!m_moved && distance >= dragThreshold) {
        m_moved = true;
        fire(keymap, KeyTrigger::Drag, fired);
    }
    if (!m_holdChecked && input.timeSeconds - m_pressSeconds >= keyHoldSeconds) {
        m_holdChecked = true;
        m_holdRan = fire(keymap, KeyTrigger::Hold, fired);
    }
    return fired;
}

bool KeymapGesture::finished() const {
    return m_finished;
}

KeymapRegion KeymapGesture::region() const {
    return m_region;
}

ImGuiKeyChord KeymapGesture::chord() const {
    return m_chord;
}

std::optional<KeymapInvocation> KeymapDispatcher::pressKeys(Keymap& keymap, KeymapRegion region, KeyboardOwner owner) {
    for (const ImGuiKeyChord chord : keymap.chords(region)) {
        if (owner == KeyboardOwner::Game && chordReachesGame(chord)) {
            continue;
        }
        // Global routes rank below focused widgets, so text fields keep their
        // own Ctrl+Z.
        const bool pressed = region == KeymapRegion::Global ? ImGui::Shortcut(chord, ImGuiInputFlags_RouteGlobal)
                                                            : ImGui::IsKeyChordPressed(chord);
        if (!pressed) {
            continue;
        }
        if (keymap.tracksChord(region, chord)) {
            follow(region, chord);
        }
        const KeymapBinding* binding =
            keymap.resolve(region, chord, ImGui::GetTime(), ImGui::GetIO().MouseDoubleClickTime);
        if (binding == nullptr) {
            return std::nullopt;
        }
        return invocationOf(*binding);
    }
    return std::nullopt;
}

KeymapMousePress KeymapDispatcher::pressMouse(Keymap& keymap, KeymapRegion region, uint32_t allowedButtons) {
    KeymapMousePress press;
    for (int button = 0; button < ImGuiMouseButton_COUNT; ++button) {
        if ((allowedButtons & (1u << button)) == 0 || !ImGui::IsMouseClicked(button)) {
            continue;
        }
        const ImGuiKeyChord chord = ImGui::GetIO().KeyMods | mouseButtonKey(button);
        if (!claims(keymap, region, chord)) {
            continue;
        }
        press.claimed |= 1u << button;
        if (keymap.tracksChord(region, chord)) {
            follow(region, chord);
        }
        const KeymapBinding* binding =
            keymap.resolve(region, chord, ImGui::GetTime(), ImGui::GetIO().MouseDoubleClickTime);
        if (binding != nullptr && registered(binding->operatorId)) {
            press.invocations.push_back(invocationOf(*binding));
        }
    }
    return press;
}

std::vector<KeymapInvocation> KeymapDispatcher::update(const Keymap& keymap) {
    const int frame = ImGui::GetFrameCount();
    if (frame == m_updatedFrame || !m_gesture) {
        m_updatedFrame = frame;
        return {};
    }
    m_updatedFrame = frame;
    const ImGuiIO& io = ImGui::GetIO();
    const KeymapGestureInput input{
        .down = ImGui::IsKeyDown(chordKey(m_gesture->chord())), .timeSeconds = ImGui::GetTime(), .mouse = io.MousePos};
    std::vector<KeymapInvocation> fired = m_gesture->advance(keymap, input, io.MouseDragThreshold);
    if (m_gesture->finished()) {
        m_gesture.reset();
    }
    std::erase_if(fired, [this](const KeymapInvocation& invocation) { return !registered(invocation.operatorId); });
    return fired;
}

uint32_t KeymapDispatcher::dispatchMouse(Keymap& keymap, KeymapRegion region, uint32_t allowedButtons,
                                         const std::function<void(const KeymapInvocation&)>& run) {
    for (const KeymapInvocation& invocation : update(keymap)) {
        run(invocation);
    }
    const KeymapMousePress press = pressMouse(keymap, region, allowedButtons);
    for (const KeymapInvocation& invocation : press.invocations) {
        run(invocation);
    }
    return press.claimed;
}

void KeymapDispatcher::cancel() {
    m_gesture.reset();
}

const std::optional<KeymapGesture>& KeymapDispatcher::gesture() const {
    return m_gesture;
}

bool KeymapDispatcher::mouseButtonBound(const Keymap& keymap, KeymapRegion region, int button) const {
    const ImGuiKey key = mouseButtonKey(button);
    return key != ImGuiKey_None && claims(keymap, region, ImGui::GetIO().KeyMods | key);
}

void KeymapDispatcher::setOperatorFilter(std::function<bool(std::string_view operatorId)> registered) {
    m_registered = std::move(registered);
}

uint64_t KeymapDispatcher::gestureSerial() const {
    return m_gestureSerial;
}

void KeymapDispatcher::follow(KeymapRegion region, ImGuiKeyChord chord) {
    m_gesture.emplace(region, chord, ImGui::GetTime(), ImGui::GetIO().MousePos);
    ++m_gestureSerial;
}

bool KeymapDispatcher::registered(std::string_view operatorId) const {
    return !m_registered || m_registered(operatorId);
}

bool KeymapDispatcher::claims(const Keymap& keymap, KeymapRegion region, ImGuiKeyChord chord) const {
    return std::ranges::any_of(keymap.bindings(), [&](const KeymapBinding& binding) {
        return binding.region == region && binding.chord == chord && registered(binding.operatorId);
    });
}
