#include "InputActionsJson.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <fstream>
#include <iterator>
#include <type_traits>
#include <utility>

namespace {
using Json = nlohmann::json;

constexpr int currentVersion = 1;

constexpr std::array<std::string_view, static_cast<size_t>(Key::Count)> keyNames{
    "W",     "A", "S", "D",     "Q", "E", "LeftShift", "RightShift", "LeftControl", "RightControl",
    "Space", "F", "R", "Escape"};
constexpr std::array<std::string_view, static_cast<size_t>(MouseButton::Count)> mouseButtonNames{"Left", "Middle",
                                                                                                 "Right", "X1", "X2"};
constexpr std::array<std::string_view, 4> mouseAxisNames{"DeltaX", "DeltaY", "WheelX", "WheelY"};
constexpr std::array<std::string_view, static_cast<size_t>(GamepadButton::Count)> gamepadButtonNames{
    "South",      "East",         "West",          "North",  "Back",     "Guide",    "Start",    "LeftStick",
    "RightStick", "LeftShoulder", "RightShoulder", "DpadUp", "DpadDown", "DpadLeft", "DpadRight"};
constexpr std::array<std::string_view, static_cast<size_t>(GamepadAxis::Count)> gamepadAxisNames{
    "LeftX", "LeftY", "RightX", "RightY", "LeftTrigger", "RightTrigger"};

// The kind prefix and value names of one source type.
template <typename Source> struct SourceNames;

template <> struct SourceNames<Key> {
    static constexpr std::string_view kind = "key";
    static constexpr const auto& names = keyNames;
};

template <> struct SourceNames<MouseButton> {
    static constexpr std::string_view kind = "mouseButton";
    static constexpr const auto& names = mouseButtonNames;
};

template <> struct SourceNames<MouseAxis> {
    static constexpr std::string_view kind = "mouseAxis";
    static constexpr const auto& names = mouseAxisNames;
};

template <> struct SourceNames<GamepadButton> {
    static constexpr std::string_view kind = "gamepadButton";
    static constexpr const auto& names = gamepadButtonNames;
};

template <> struct SourceNames<GamepadAxis> {
    static constexpr std::string_view kind = "gamepadAxis";
    static constexpr const auto& names = gamepadAxisNames;
};

template <typename Source>
bool parseSourceOf(std::string_view kind, std::string_view value, std::optional<InputBindingSource>& source) {
    if (kind != SourceNames<Source>::kind) {
        return false;
    }
    const auto& names = SourceNames<Source>::names;
    for (size_t index = 0; index < names.size(); ++index) {
        if (names[index] == value) {
            source = static_cast<Source>(index);
        }
    }
    return true;
}

template <typename Source> void appendSources(std::vector<InputBindingSource>& sources) {
    for (size_t index = 0; index < SourceNames<Source>::names.size(); ++index) {
        sources.push_back(static_cast<Source>(index));
    }
}

bool readVec2(const Json& value, glm::vec2& vector) {
    if (!value.is_array() || value.size() != 2 || !value[0].is_number() || !value[1].is_number()) {
        return false;
    }
    vector = {value[0].get<float>(), value[1].get<float>()};
    return true;
}

InputActionsJsonResult readBinding(const Json& value, const std::string& action, InputBinding& binding) {
    const auto failure = [&action](const std::string& what) {
        return InputActionsJsonResult{.error = "Input action '" + action + "': " + what};
    };
    if (!value.is_object() || !value.contains("source") || !value["source"].is_string()) {
        return failure("each binding needs a \"source\" such as \"key:W\"");
    }
    const std::string sourceName = value["source"].get<std::string>();
    const std::optional<InputBindingSource> source = parseInputSource(sourceName);
    if (!source) {
        return failure("unknown input source \"" + sourceName + "\"");
    }
    binding.source = *source;
    if (value.contains("scale") && !readVec2(value["scale"], binding.scale)) {
        return failure("a binding's \"scale\" must be two numbers");
    }
    if (value.contains("deadZone")) {
        if (!value["deadZone"].is_number()) {
            return failure("a binding's \"deadZone\" must be a number");
        }
        binding.deadZone = value["deadZone"].get<float>();
    }
    if (value.contains("modifiers")) {
        if (!value["modifiers"].is_array()) {
            return failure("a binding's \"modifiers\" must list keys");
        }
        for (const Json& modifier : value["modifiers"]) {
            const std::optional<InputBindingSource> key =
                modifier.is_string() ? parseInputSource(modifier.get<std::string>()) : std::nullopt;
            if (!key || !std::holds_alternative<Key>(*key)) {
                return failure("modifier " + modifier.dump() + " is not a key such as \"key:LeftShift\"");
            }
            binding.modifiers.push_back(std::get<Key>(*key));
        }
    }
    return {.success = true};
}
} // namespace

std::string inputSourceName(const InputBindingSource& source) {
    return std::visit(
        [](auto value) {
            using Names = SourceNames<decltype(value)>;
            const size_t index = static_cast<size_t>(value);
            const std::string_view name = index < Names::names.size() ? Names::names[index] : "?";
            return std::string(Names::kind) + ":" + std::string(name);
        },
        source);
}

std::optional<InputBindingSource> parseInputSource(std::string_view name) {
    const size_t colon = name.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view kind = name.substr(0, colon);
    const std::string_view value = name.substr(colon + 1);
    std::optional<InputBindingSource> source;
    if (parseSourceOf<Key>(kind, value, source) || parseSourceOf<MouseButton>(kind, value, source) ||
        parseSourceOf<MouseAxis>(kind, value, source) || parseSourceOf<GamepadButton>(kind, value, source) ||
        parseSourceOf<GamepadAxis>(kind, value, source)) {
        return source;
    }
    return std::nullopt;
}

std::vector<InputBindingSource> allInputSources() {
    std::vector<InputBindingSource> sources;
    appendSources<Key>(sources);
    appendSources<MouseButton>(sources);
    appendSources<MouseAxis>(sources);
    appendSources<GamepadButton>(sources);
    appendSources<GamepadAxis>(sources);
    return sources;
}

std::string_view inputActionTypeName(InputActionValueType type) {
    switch (type) {
    case InputActionValueType::Button:
        return "button";
    case InputActionValueType::Axis1D:
        return "axis1d";
    case InputActionValueType::Axis2D:
        return "axis2d";
    }
    return "button";
}

std::optional<InputActionValueType> parseInputActionType(std::string_view name) {
    for (const InputActionValueType type :
         {InputActionValueType::Button, InputActionValueType::Axis1D, InputActionValueType::Axis2D}) {
        if (inputActionTypeName(type) == name) {
            return type;
        }
    }
    return std::nullopt;
}

std::string serializeInputActionMap(const InputActionMap& actions) {
    Json list = Json::array();
    for (const std::string& name : actions.actionNames()) {
        Json bindings = Json::array();
        for (const InputBinding& binding : actions.bindings(name)) {
            Json entry{{"source", inputSourceName(binding.source)},
                       {"scale", {binding.scale.x, binding.scale.y}},
                       {"deadZone", binding.deadZone}};
            if (!binding.modifiers.empty()) {
                Json modifiers = Json::array();
                for (const Key modifier : binding.modifiers) {
                    modifiers.push_back(inputSourceName(modifier));
                }
                entry["modifiers"] = std::move(modifiers);
            }
            bindings.push_back(std::move(entry));
        }
        list.push_back({{"name", name},
                        {"type", inputActionTypeName(actions.actionType(name).value_or(InputActionValueType::Button))},
                        {"clamp", actions.clampsValue(name)},
                        {"bindings", std::move(bindings)}});
    }
    return Json{{"version", currentVersion}, {"actions", std::move(list)}}.dump(2);
}

InputActionsJsonResult deserializeInputActionMap(std::string_view text, InputActionMap& actions) {
    const Json document = Json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return {.error = "The input actions are not a JSON object"};
    }
    const int version = document.value("version", 0);
    if (version < 1 || version > currentVersion) {
        return {.error = "Unsupported input actions version " + std::to_string(version)};
    }
    const auto list = document.find("actions");
    if (list == document.end() || !list->is_array()) {
        return {.error = "The input actions need an \"actions\" array"};
    }
    InputActionMap loaded;
    for (const Json& action : *list) {
        if (!action.is_object() || !action.contains("name") || !action["name"].is_string()) {
            return {.error = "Each input action needs a \"name\""};
        }
        const std::string name = action["name"].get<std::string>();
        const std::optional<InputActionValueType> type =
            parseInputActionType(action.value("type", std::string("button")));
        if (!type) {
            return {.error = "Input action '" + name + "' has an unknown type; use button, axis1d, or axis2d"};
        }
        const bool clamp = action.value("clamp", true);
        if (!loaded.addAction(name, *type, clamp)) {
            return {.error = "Input action '" + name + "' is empty or declared twice"};
        }
        if (action.contains("bindings") && !action["bindings"].is_array()) {
            return {.error = "Input action '" + name + "' needs \"bindings\" to be an array"};
        }
        for (const Json& value : action.value("bindings", Json::array())) {
            InputBinding binding;
            if (InputActionsJsonResult result = readBinding(value, name, binding); !result) {
                return result;
            }
            loaded.addBinding(name, std::move(binding));
        }
    }
    actions = std::move(loaded);
    return {.success = true};
}

InputActionsJsonResult saveInputActionMap(const std::filesystem::path& path, const InputActionMap& actions) {
    std::error_code error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), error);
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << serializeInputActionMap(actions) << '\n';
    if (!output) {
        return {.error = "Could not write the input actions: " + path.string()};
    }
    return {.success = true};
}

InputActionsJsonResult loadInputActionMap(const std::filesystem::path& path, InputActionMap& actions) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {.error = "Could not read the input actions: " + path.string()};
    }
    const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (InputActionsJsonResult result = deserializeInputActionMap(text, actions); !result) {
        return {.error = result.error + " (" + path.string() + ")"};
    }
    return {.success = true};
}
