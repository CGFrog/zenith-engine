#include "Keymap.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <utility>

namespace {
using Json = nlohmann::json;

constexpr int keymapFileVersion = 1;

struct ModifierName {
    ImGuiKeyChord modifier;
    std::string_view prefix;
};

constexpr std::array modifierNames{ModifierName{ImGuiMod_Ctrl, "Ctrl+"}, ModifierName{ImGuiMod_Shift, "Shift+"},
                                   ModifierName{ImGuiMod_Alt, "Alt+"}, ModifierName{ImGuiMod_Super, "Super+"}};

struct MouseButtonName {
    ImGuiKey key;
    std::string_view name;
};

constexpr std::array mouseButtonNames{MouseButtonName{ImGuiKey_MouseLeft, "LMB"},
                                      MouseButtonName{ImGuiKey_MouseRight, "RMB"},
                                      MouseButtonName{ImGuiKey_MouseMiddle, "MMB"},
                                      MouseButtonName{ImGuiKey_MouseX1, "X1"}, MouseButtonName{ImGuiKey_MouseX2, "X2"}};

constexpr std::array<std::pair<KeyTrigger, const char*>, 6> triggerNames{{{KeyTrigger::Press, "press"},
                                                                          {KeyTrigger::DoubleTap, "double_tap"},
                                                                          {KeyTrigger::Hold, "hold"},
                                                                          {KeyTrigger::Release, "release"},
                                                                          {KeyTrigger::Click, "click"},
                                                                          {KeyTrigger::Drag, "drag"}}};

bool mouseKey(ImGuiKey key) {
    return key >= ImGuiKey_MouseLeft && key <= ImGuiKey_MouseX2;
}

// Keys a binding may use: named keyboard keys before the gamepad range,
// without the modifier keys themselves, and the mouse buttons.
bool bindableKey(ImGuiKey key) {
    if (mouseKey(key)) {
        return true;
    }
    if (key < ImGuiKey_NamedKey_BEGIN || key >= ImGuiKey_GamepadStart) {
        return false;
    }
    return key < ImGuiKey_LeftCtrl || key > ImGuiKey_RightSuper;
}

std::string keyName(ImGuiKey key) {
    for (const MouseButtonName& button : mouseButtonNames) {
        if (button.key == key) {
            return std::string(button.name);
        }
    }
    const std::string_view name = ImGui::GetKeyName(key);
    constexpr std::string_view keypad = "Keypad";
    if (!name.starts_with(keypad)) {
        return std::string(name);
    }
    const std::string_view rest = name.substr(keypad.size());
    constexpr std::array<std::pair<std::string_view, std::string_view>, 6> symbols{
        {{"Decimal", "."}, {"Divide", "/"}, {"Multiply", "*"}, {"Subtract", "-"}, {"Add", "+"}, {"Equal", "="}}};
    for (const auto& [word, symbol] : symbols) {
        if (rest == word) {
            return "Numpad " + std::string(symbol);
        }
    }
    return "Numpad " + std::string(rest);
}

std::optional<ImGuiKey> parseKeyName(std::string_view text) {
    for (const MouseButtonName& button : mouseButtonNames) {
        if (button.name == text) {
            return button.key;
        }
    }
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_GamepadStart; ++key) {
        if (bindableKey(static_cast<ImGuiKey>(key)) && keyName(static_cast<ImGuiKey>(key)) == text) {
            return static_cast<ImGuiKey>(key);
        }
    }
    return std::nullopt;
}

bool regionsOverlap(KeymapRegion first, KeymapRegion second) {
    return first == second || first == KeymapRegion::Global || second == KeymapRegion::Global;
}

// Compact JSON object text, or empty for no properties; nullopt when invalid.
std::optional<std::string> normalizeProperties(std::string_view properties) {
    if (properties.empty()) {
        return std::string{};
    }
    const Json parsed = Json::parse(properties, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::nullopt;
    }
    return parsed.empty() ? std::string{} : parsed.dump();
}

std::string validate(KeymapBinding& binding) {
    if (binding.operatorId.empty()) {
        return "Key bindings need an operator id";
    }
    const ImGuiKey key = static_cast<ImGuiKey>(binding.chord & ~ImGuiMod_Mask_);
    if (!bindableKey(key)) {
        return "Key binding for '" + binding.operatorId + "' needs a keyboard key or mouse button";
    }
    if (mouseKey(key) && binding.region != KeymapRegion::Viewport) {
        return "Mouse binding for '" + binding.operatorId + "' must be in the viewport region";
    }
    const std::optional<std::string> properties = normalizeProperties(binding.properties);
    if (!properties) {
        return "Key binding properties for '" + binding.operatorId + "' must be a JSON object";
    }
    binding.properties = *properties;
    return {};
}

std::string describe(const KeymapBinding& binding) {
    std::string text = formatKeyChord(binding.chord);
    if (binding.trigger != KeyTrigger::Press) {
        text += std::string(" ") + keyTriggerName(binding.trigger);
    }
    return text + " (" + keymapRegionName(binding.region) + ")";
}

Json bindingToJson(const KeymapBinding& binding) {
    Json entry{{"operator", binding.operatorId},
               {"region", keymapRegionName(binding.region)},
               {"chord", formatKeyChord(binding.chord)}};
    if (binding.trigger != KeyTrigger::Press) {
        entry["trigger"] = keyTriggerName(binding.trigger);
    }
    if (!binding.properties.empty()) {
        entry["properties"] = Json::parse(binding.properties);
    }
    return entry;
}

std::optional<KeymapBinding> bindingFromJson(const Json& entry, std::string& error) {
    if (!entry.is_object() || !entry.contains("operator") || !entry["operator"].is_string() ||
        !entry.contains("chord") || !entry["chord"].is_string()) {
        error = "A keymap entry needs \"operator\" and \"chord\" strings";
        return std::nullopt;
    }
    KeymapBinding binding{.operatorId = entry["operator"].get<std::string>()};
    const std::optional<ImGuiKeyChord> chord = parseKeyChord(entry["chord"].get<std::string>());
    if (!chord) {
        error = "Unknown key chord '" + entry["chord"].get<std::string>() + "' for " + binding.operatorId;
        return std::nullopt;
    }
    binding.chord = *chord;
    if (const auto region = entry.find("region"); region != entry.end()) {
        const std::optional<KeymapRegion> parsed =
            region->is_string() ? parseKeymapRegion(region->get<std::string>()) : std::nullopt;
        if (!parsed) {
            error = "Unknown keymap region for " + binding.operatorId;
            return std::nullopt;
        }
        binding.region = *parsed;
    }
    if (const auto trigger = entry.find("trigger"); trigger != entry.end()) {
        const std::optional<KeyTrigger> parsed =
            trigger->is_string() ? parseKeyTrigger(trigger->get<std::string>()) : std::nullopt;
        if (!parsed) {
            error = "Unknown key trigger for " + binding.operatorId;
            return std::nullopt;
        }
        binding.trigger = *parsed;
    }
    if (const auto properties = entry.find("properties"); properties != entry.end()) {
        if (!properties->is_object()) {
            error = "Key binding properties for " + binding.operatorId + " must be an object";
            return std::nullopt;
        }
        binding.properties = properties->dump();
    }
    if (error = validate(binding); !error.empty()) {
        return std::nullopt;
    }
    return binding;
}
} // namespace

std::string formatKeyChord(ImGuiKeyChord chord) {
    const ImGuiKey key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
    if (key == ImGuiKey_None) {
        return {};
    }
    std::string text;
    for (const ModifierName& modifier : modifierNames) {
        if ((chord & modifier.modifier) != 0) {
            text += modifier.prefix;
        }
    }
    return text + keyName(key);
}

std::optional<ImGuiKeyChord> parseKeyChord(std::string_view text) {
    ImGuiKeyChord modifiers = 0;
    bool stripped = true;
    while (stripped) {
        stripped = false;
        for (const ModifierName& modifier : modifierNames) {
            // A trailing "+" alone is the Numpad + key name, not a modifier.
            if (text.starts_with(modifier.prefix) && text.size() > modifier.prefix.size()) {
                modifiers |= modifier.modifier;
                text.remove_prefix(modifier.prefix.size());
                stripped = true;
            }
        }
    }
    const std::optional<ImGuiKey> key = parseKeyName(text);
    if (!key) {
        return std::nullopt;
    }
    return modifiers | *key;
}

bool isMouseChord(ImGuiKeyChord chord) {
    return mouseKey(static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_));
}

ImGuiKey mouseButtonKey(int button) {
    if (button < ImGuiMouseButton_Left || button > ImGuiKey_MouseX2 - ImGuiKey_MouseLeft) {
        return ImGuiKey_None;
    }
    return static_cast<ImGuiKey>(ImGuiKey_MouseLeft + button);
}

const char* keyTriggerName(KeyTrigger trigger) {
    for (const auto& [value, name] : triggerNames) {
        if (value == trigger) {
            return name;
        }
    }
    return "press";
}

std::optional<KeyTrigger> parseKeyTrigger(std::string_view name) {
    for (const auto& [value, triggerName] : triggerNames) {
        if (name == triggerName) {
            return value;
        }
    }
    return std::nullopt;
}

const char* keymapRegionName(KeymapRegion region) {
    switch (region) {
    case KeymapRegion::Global:
        return "global";
    case KeymapRegion::Viewport:
        return "viewport";
    case KeymapRegion::Outliner:
        return "outliner";
    }
    return "global";
}

std::optional<KeymapRegion> parseKeymapRegion(std::string_view name) {
    for (const KeymapRegion region : {KeymapRegion::Global, KeymapRegion::Viewport, KeymapRegion::Outliner}) {
        if (name == keymapRegionName(region)) {
            return region;
        }
    }
    return std::nullopt;
}

Keymap::Keymap(std::vector<KeymapBinding> defaults) {
    for (KeymapBinding& binding : defaults) {
        if (std::string problem = validate(binding); !problem.empty()) {
            m_defaultProblems.push_back(std::move(problem));
            continue;
        }
        if (const std::optional<size_t> conflict = findConflict(binding)) {
            m_defaultProblems.push_back("Default " + describe(binding) + " for " + binding.operatorId +
                                        " conflicts with " + m_bindings[*conflict].operatorId);
            continue;
        }
        m_bindings.push_back(std::move(binding));
    }
    m_defaults = m_bindings;
}

const std::vector<KeymapBinding>& Keymap::bindings() const {
    return m_bindings;
}

const std::vector<KeymapBinding>& Keymap::defaults() const {
    return m_defaults;
}

const std::vector<std::string>& Keymap::defaultProblems() const {
    return m_defaultProblems;
}

std::optional<size_t> Keymap::findConflict(const KeymapBinding& binding, std::optional<size_t> ignore) const {
    for (size_t index = 0; index < m_bindings.size(); ++index) {
        const KeymapBinding& other = m_bindings[index];
        if (index != ignore && other.chord == binding.chord && other.trigger == binding.trigger &&
            regionsOverlap(other.region, binding.region)) {
            return index;
        }
    }
    return std::nullopt;
}

KeymapResult Keymap::add(KeymapBinding binding) {
    if (std::string problem = validate(binding); !problem.empty()) {
        return {.error = std::move(problem)};
    }
    if (const std::optional<size_t> conflict = findConflict(binding)) {
        return {.error = describe(binding) + " is already assigned to " + m_bindings[*conflict].operatorId};
    }
    m_bindings.push_back(std::move(binding));
    return {.success = true};
}

KeymapResult Keymap::rebind(size_t index, ImGuiKeyChord chord) {
    if (index >= m_bindings.size()) {
        return {.error = "No such key binding"};
    }
    KeymapBinding changed = m_bindings[index];
    changed.chord = chord;
    if (std::string problem = validate(changed); !problem.empty()) {
        return {.error = std::move(problem)};
    }
    if (const std::optional<size_t> conflict = findConflict(changed, index)) {
        return {.error = describe(changed) + " is already assigned to " + m_bindings[*conflict].operatorId};
    }
    m_bindings[index] = std::move(changed);
    return {.success = true};
}

KeymapResult Keymap::setTrigger(size_t index, KeyTrigger trigger) {
    if (index >= m_bindings.size()) {
        return {.error = "No such key binding"};
    }
    KeymapBinding changed = m_bindings[index];
    changed.trigger = trigger;
    if (const std::optional<size_t> conflict = findConflict(changed, index)) {
        return {.error = describe(changed) + " is already assigned to " + m_bindings[*conflict].operatorId};
    }
    m_bindings[index] = std::move(changed);
    return {.success = true};
}

void Keymap::remove(size_t index) {
    if (index < m_bindings.size()) {
        m_bindings.erase(m_bindings.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

KeymapResult Keymap::resetOperator(std::string_view operatorId) {
    std::erase_if(m_bindings, [operatorId](const KeymapBinding& binding) { return binding.operatorId == operatorId; });
    KeymapResult result{.success = true};
    for (const KeymapBinding& binding : m_defaults) {
        if (binding.operatorId != operatorId) {
            continue;
        }
        if (const std::optional<size_t> conflict = findConflict(binding)) {
            result.warnings.push_back("Default " + describe(binding) + " is now used by " +
                                      m_bindings[*conflict].operatorId);
            continue;
        }
        m_bindings.push_back(binding);
    }
    return result;
}

void Keymap::resetToDefaults() {
    m_bindings = m_defaults;
}

std::string Keymap::shortcutLabel(std::string_view operatorId, std::string_view properties) const {
    const std::string normalized = normalizeProperties(properties).value_or(std::string(properties));
    for (const KeymapRegion region : {KeymapRegion::Global, KeymapRegion::Viewport, KeymapRegion::Outliner}) {
        for (const KeymapBinding& binding : m_bindings) {
            if (binding.region == region && binding.trigger == KeyTrigger::Press && binding.operatorId == operatorId &&
                binding.properties == normalized) {
                return formatKeyChord(binding.chord);
            }
        }
    }
    return {};
}

std::vector<ImGuiKeyChord> Keymap::chords(KeymapRegion region) const {
    std::vector<ImGuiKeyChord> result;
    for (const KeymapBinding& binding : m_bindings) {
        if (binding.region == region && !isMouseChord(binding.chord) &&
            std::ranges::find(result, binding.chord) == result.end()) {
            result.push_back(binding.chord);
        }
    }
    return result;
}

std::vector<ImGuiKeyChord> Keymap::mouseChords(KeymapRegion region) const {
    std::vector<ImGuiKeyChord> result;
    for (const KeymapBinding& binding : m_bindings) {
        if (binding.region == region && isMouseChord(binding.chord) &&
            std::ranges::find(result, binding.chord) == result.end()) {
            result.push_back(binding.chord);
        }
    }
    return result;
}

const KeymapBinding* Keymap::find(KeymapRegion region, ImGuiKeyChord chord, KeyTrigger trigger) const {
    const auto found = std::ranges::find_if(m_bindings, [&](const KeymapBinding& binding) {
        return binding.region == region && binding.chord == chord && binding.trigger == trigger;
    });
    return found == m_bindings.end() ? nullptr : &*found;
}

bool Keymap::bindsChord(KeymapRegion region, ImGuiKeyChord chord) const {
    return std::ranges::any_of(
        m_bindings, [&](const KeymapBinding& binding) { return binding.region == region && binding.chord == chord; });
}

bool Keymap::tracksChord(KeymapRegion region, ImGuiKeyChord chord) const {
    return std::ranges::any_of(m_bindings, [&](const KeymapBinding& binding) {
        return binding.region == region && binding.chord == chord && binding.trigger != KeyTrigger::Press &&
               binding.trigger != KeyTrigger::DoubleTap;
    });
}

const KeymapBinding* Keymap::resolve(KeymapRegion region, ImGuiKeyChord chord, double timeSeconds,
                                     double doubleTapSeconds) {
    const KeymapBinding* press = find(region, chord, KeyTrigger::Press);
    const KeymapBinding* doubleTap = find(region, chord, KeyTrigger::DoubleTap);
    const bool repeated = m_lastTap && m_lastTap->region == region && m_lastTap->chord == chord &&
                          timeSeconds - m_lastTap->time <= doubleTapSeconds;
    if (doubleTap != nullptr && repeated) {
        m_lastTap.reset();
        return doubleTap;
    }
    m_lastTap = Tap{.region = region, .chord = chord, .time = timeSeconds};
    return press;
}

std::string Keymap::serializeOverrides() const {
    std::vector<bool> defaultKept(m_defaults.size(), false);
    Json added = Json::array();
    for (const KeymapBinding& binding : m_bindings) {
        bool matched = false;
        for (size_t index = 0; index < m_defaults.size() && !matched; ++index) {
            if (!defaultKept[index] && m_defaults[index] == binding) {
                defaultKept[index] = true;
                matched = true;
            }
        }
        if (!matched) {
            added.push_back(bindingToJson(binding));
        }
    }
    Json removed = Json::array();
    for (size_t index = 0; index < m_defaults.size(); ++index) {
        if (!defaultKept[index]) {
            removed.push_back(bindingToJson(m_defaults[index]));
        }
    }
    if (added.empty() && removed.empty()) {
        return {};
    }
    const Json document{{"version", keymapFileVersion}, {"removed", removed}, {"added", added}};
    return document.dump(2);
}

KeymapResult Keymap::loadOverrides(std::string_view json) {
    resetToDefaults();
    const Json document = Json::parse(json, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return {.error = "The keymap file is not a JSON object"};
    }
    if (!document.contains("version") || document["version"] != keymapFileVersion) {
        return {.error = "Unsupported keymap file version"};
    }
    for (const char* list : {"removed", "added"}) {
        if (document.contains(list) && !document[list].is_array()) {
            return {.error = std::string("Keymap \"") + list + "\" must be an array"};
        }
    }

    KeymapResult result{.success = true};
    for (const Json& entry : document.value("removed", Json::array())) {
        std::string error;
        const std::optional<KeymapBinding> binding = bindingFromJson(entry, error);
        if (!binding) {
            result.warnings.push_back("Skipped removed key binding: " + error);
            continue;
        }
        if (const auto found = std::ranges::find(m_bindings, *binding); found != m_bindings.end()) {
            m_bindings.erase(found);
        }
    }
    for (const Json& entry : document.value("added", Json::array())) {
        std::string error;
        std::optional<KeymapBinding> binding = bindingFromJson(entry, error);
        if (!binding) {
            result.warnings.push_back("Skipped key binding: " + error);
            continue;
        }
        if (KeymapResult added = add(std::move(*binding)); !added) {
            result.warnings.push_back("Skipped key binding: " + added.error);
        }
    }
    return result;
}

KeymapResult Keymap::load(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        resetToDefaults();
        return {.success = true};
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        resetToDefaults();
        return {.error = "Could not read keymap " + path.string()};
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    KeymapResult result = loadOverrides(contents.str());
    if (!result) {
        result.error = "Keymap " + path.string() + ": " + result.error + "; using the default keymap";
    }
    return result;
}

KeymapResult Keymap::save(const std::filesystem::path& path) const {
    const std::string overrides = serializeOverrides();
    std::error_code error;
    if (overrides.empty()) {
        std::filesystem::remove(path, error);
        if (error) {
            return {.error = "Could not remove keymap " + path.string() + ": " + error.message()};
        }
        return {.success = true};
    }
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), error);
    }
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file << overrides << '\n';
        if (!file) {
            return {.error = "Could not write keymap " + temporary.string()};
        }
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return {.error = "Could not replace keymap " + path.string()};
    }
    return {.success = true};
}
