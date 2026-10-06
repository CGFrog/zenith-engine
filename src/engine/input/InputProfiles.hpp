#pragma once

// Named player rebinding profiles with conflict and hold/toggle policies.

#include "engine/input/InputActions.hpp"

#include <filesystem>
#include <optional>
#include <span>

enum class ButtonBehavior : uint8_t {
    Hold,
    Toggle
};
enum class BindingConflictPolicy : uint8_t {
    Reject,
    ReplaceExisting,
    Allow
};

struct ProfileBinding {
    std::string action;
    InputBinding binding;
    ButtonBehavior behavior{ButtonBehavior::Hold};
};

struct InputProfile {
    std::string name;
    std::vector<ProfileBinding> bindings;
};

struct InputProfileResult {
    bool success{false};
    std::string error;
    std::optional<size_t> conflictingBinding;

    explicit operator bool() const {
        return success;
    }
};

class InputProfileSet {
  public:
    InputProfileResult addProfile(InputProfile profile);
    bool select(std::string_view name);
    std::string_view selectedName() const;
    const InputProfile* selected() const;
    InputProfileResult bind(std::string action, InputBinding binding, ButtonBehavior behavior,
                            BindingConflictPolicy policy);
    std::span<const InputProfile> profiles() const;

  private:
    std::vector<InputProfile> m_profiles;
    size_t m_selected{0};
};

// Replaces the bindings of every action profile names with the profile's, so
// a player's saved profile overrides a game's default bindings (from its game
// module's action map). Actions the profile does not name keep theirs. Fails,
// changing nothing, when the profile names an action map lacks. Hold and
// Toggle behaviours are not evaluated; every binding acts as Hold.
InputProfileResult applyInputProfile(const InputProfile& profile, InputActionMap& map);

InputProfileResult saveInputProfiles(const std::filesystem::path& path, const InputProfileSet& profiles);
InputProfileResult loadInputProfiles(const std::filesystem::path& path, InputProfileSet& profiles);
