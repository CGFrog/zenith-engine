#include "engine/input/InputProfiles.hpp"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <type_traits>

namespace {
bool sameSource(const InputBinding& first, const InputBinding& second) {
    return first.source == second.source && first.modifiers == second.modifiers;
}

nlohmann::json encodeSource(const InputBindingSource& source) {
    return std::visit(
        [](auto value) {
            using Type = decltype(value);
            const char* kind = std::is_same_v<Type, Key>             ? "key"
                               : std::is_same_v<Type, MouseButton>   ? "mouseButton"
                               : std::is_same_v<Type, MouseAxis>     ? "mouseAxis"
                               : std::is_same_v<Type, GamepadButton> ? "gamepadButton"
                                                                     : "gamepadAxis";
            return nlohmann::json{{"kind", kind}, {"value", static_cast<int>(value)}};
        },
        source);
}

bool decodeSource(const nlohmann::json& value, InputBindingSource& source) {
    const std::string kind = value.value("kind", "");
    const int index = value.value("value", -1);
    if (kind == "key" && index >= 0 && index < static_cast<int>(Key::Count)) {
        source = static_cast<Key>(index);
    } else if (kind == "mouseButton" && index >= 0 && index < static_cast<int>(MouseButton::Count)) {
        source = static_cast<MouseButton>(index);
    } else if (kind == "mouseAxis" && index >= 0 && index <= static_cast<int>(MouseAxis::WheelY)) {
        source = static_cast<MouseAxis>(index);
    } else if (kind == "gamepadButton" && index >= 0 && index < static_cast<int>(GamepadButton::Count)) {
        source = static_cast<GamepadButton>(index);
    } else if (kind == "gamepadAxis" && index >= 0 && index < static_cast<int>(GamepadAxis::Count)) {
        source = static_cast<GamepadAxis>(index);
    } else {
        return false;
    }
    return true;
}
} // namespace

InputProfileResult InputProfileSet::addProfile(InputProfile profile) {
    if (profile.name.empty()) {
        return {.error = "input profile name must not be empty"};
    }
    if (std::ranges::any_of(m_profiles, [&profile](const InputProfile& item) { return item.name == profile.name; })) {
        return {.error = "input profile already exists: " + profile.name};
    }
    m_profiles.push_back(std::move(profile));
    return {.success = true};
}

bool InputProfileSet::select(std::string_view name) {
    for (size_t index = 0; index < m_profiles.size(); ++index) {
        if (m_profiles[index].name == name) {
            m_selected = index;
            return true;
        }
    }
    return false;
}

std::string_view InputProfileSet::selectedName() const {
    return m_profiles.empty() ? std::string_view{} : std::string_view(m_profiles[m_selected].name);
}

const InputProfile* InputProfileSet::selected() const {
    return m_profiles.empty() ? nullptr : &m_profiles[m_selected];
}

InputProfileResult InputProfileSet::bind(std::string action, InputBinding binding, ButtonBehavior behavior,
                                         BindingConflictPolicy policy) {
    if (m_profiles.empty()) {
        return {.error = "no input profile is selected"};
    }
    InputProfile& profile = m_profiles[m_selected];
    for (size_t index = 0; index < profile.bindings.size(); ++index) {
        if (sameSource(profile.bindings[index].binding, binding) && profile.bindings[index].action != action) {
            if (policy == BindingConflictPolicy::Reject) {
                return {.error = "binding conflicts with action '" + profile.bindings[index].action + "'",
                        .conflictingBinding = index};
            }
            if (policy == BindingConflictPolicy::ReplaceExisting) {
                profile.bindings.erase(profile.bindings.begin() + static_cast<std::ptrdiff_t>(index));
            }
            break;
        }
    }
    profile.bindings.push_back({std::move(action), std::move(binding), behavior});
    return {.success = true};
}

std::span<const InputProfile> InputProfileSet::profiles() const {
    return m_profiles;
}

InputProfileResult applyInputProfile(const InputProfile& profile, InputActionMap& map) {
    for (const ProfileBinding& binding : profile.bindings) {
        if (!map.contains(binding.action)) {
            return {.error = "Input profile '" + profile.name + "' binds unknown action '" + binding.action + "'"};
        }
    }
    std::vector<std::string_view> cleared;
    for (const ProfileBinding& binding : profile.bindings) {
        if (std::ranges::find(cleared, binding.action) == cleared.end()) {
            map.clearBindings(binding.action);
            cleared.push_back(binding.action);
        }
        map.addBinding(binding.action, binding.binding);
    }
    return {.success = true};
}

InputProfileResult saveInputProfiles(const std::filesystem::path& path, const InputProfileSet& profiles) {
    nlohmann::json encodedProfiles = nlohmann::json::array();
    for (const InputProfile& profile : profiles.profiles()) {
        nlohmann::json bindings = nlohmann::json::array();
        for (const ProfileBinding& entry : profile.bindings) {
            bindings.push_back({{"action", entry.action},
                                {"source", encodeSource(entry.binding.source)},
                                {"scale", {entry.binding.scale.x, entry.binding.scale.y}},
                                {"modifiers", entry.binding.modifiers},
                                {"deadZone", entry.binding.deadZone},
                                {"behavior", static_cast<int>(entry.behavior)}});
        }
        encodedProfiles.push_back({{"name", profile.name}, {"bindings", std::move(bindings)}});
    }
    const nlohmann::json document = {
        {"version", 1}, {"selected", profiles.selectedName()}, {"profiles", std::move(encodedProfiles)}};
    const std::string bytes = document.dump(2) + '\n';
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return {.error = "could not create input-profile directory: " + error.message()};
    }
    const std::filesystem::path temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    output << bytes;
    output.close();
    if (!output) {
        return {.error = "could not write input profiles"};
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
    }
    return error ? InputProfileResult{.error = "could not commit input profiles: " + error.message()}
                 : InputProfileResult{.success = true};
}

InputProfileResult loadInputProfiles(const std::filesystem::path& path, InputProfileSet& profiles) {
    std::ifstream input(path);
    if (!input) {
        return {.error = "could not open input profiles: " + path.string()};
    }
    try {
        nlohmann::json document;
        input >> document;
        if (document.value("version", 0) != 1) {
            return {.error = "unsupported input-profile version"};
        }
        InputProfileSet loaded;
        for (const auto& encodedProfile : document.at("profiles")) {
            InputProfile profile;
            profile.name = encodedProfile.at("name").get<std::string>();
            for (const auto& encodedBinding : encodedProfile.at("bindings")) {
                ProfileBinding entry;
                entry.action = encodedBinding.at("action").get<std::string>();
                if (!decodeSource(encodedBinding.at("source"), entry.binding.source)) {
                    return {.error = "input profile contains an invalid binding source"};
                }
                entry.binding.scale = {encodedBinding.at("scale").at(0).get<float>(),
                                       encodedBinding.at("scale").at(1).get<float>()};
                entry.binding.modifiers = encodedBinding.value("modifiers", std::vector<Key>{});
                entry.binding.deadZone = encodedBinding.value("deadZone", 0.15f);
                const int behavior = encodedBinding.value("behavior", 0);
                if (behavior < 0 || behavior > 1 || entry.binding.deadZone < 0.0f || entry.binding.deadZone >= 1.0f) {
                    return {.error = "input profile contains an out-of-range value"};
                }
                entry.behavior = static_cast<ButtonBehavior>(behavior);
                profile.bindings.push_back(std::move(entry));
            }
            if (const InputProfileResult result = loaded.addProfile(std::move(profile)); !result) {
                return result;
            }
        }
        if (!loaded.select(document.value("selected", ""))) {
            return {.error = "selected input profile is missing"};
        }
        profiles = std::move(loaded);
        return {.success = true};
    } catch (const nlohmann::json::exception& error) {
        return {.error = "could not parse input profiles: " + std::string(error.what())};
    }
}
