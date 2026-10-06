#pragma once

// First-person camera component, character input, look, and spawnFirstPersonPlayer.

#include "engine/physics/CharacterController.hpp"
#include "engine/scene/Scene.hpp"

#include <glm/glm.hpp>
#include <string>

class InputActionState;

// A first-person view: put it on a camera that is a child of a character
// (CharacterControllerComponent). The engine's zenith.first_person_camera
// system turns look input into yaw and pitch and rotates the camera about
// its own local position, so the camera keeps the eye height it was placed
// at. zenith.character_input walks the character relative to the camera's
// yaw. Angles are radians.
struct FirstPersonCameraComponent {
    // Radians of rotation per unit of look input.
    float sensitivity{0.0025f};
    // Look input units per second that a gamepad stick at full deflection
    // gives, so stick look turns at sensitivity * this radians per second.
    float gamepadLookSpeed{800.0f};
    float minimumPitchRadians{-1.5533430f};
    float maximumPitchRadians{1.5533430f};
    // The view's current yaw (about world Y) and pitch; saved as the initial view.
    float yawRadians{0.0f};
    float pitchRadians{0.0f};
    // The 2D input action that turns the view; empty leaves it to code.
    std::string lookAction{"Look"};
};

// The player's look preferences, which scale look input for every
// first-person camera in a scene. A game stores them in the scene's registry
// context (registry().ctx()); without them the defaults apply, as in an
// editor's play mode.
struct FirstPersonViewPreferences {
    float lookScale{1.0f};
    bool invertLookY{false};
};

// The first child of entity that has a CameraComponent, or null.
Entity findCameraChild(const Scene& scene, Entity entity);

// The yaw (radians about world Y) a character walks relative to: its
// first-person camera's, or else its own facing.
float characterHeading(const Scene& scene, Entity character);

// Sets each player-driven character's desired velocity and jump request from
// its move, jump and sprint actions, relative to characterHeading. Characters
// with an empty move action are left alone.
void applyCharacterInput(Scene& scene, const InputActionState& actions);

// Turns each first-person camera by its look action (when actions is not
// null) and writes its yaw and pitch into its local transform.
void updateFirstPersonCameras(Scene& scene, const InputActionState* actions, float deltaSeconds);

// Creates a first-person player at position: a character controller with a
// camera child at eye height that has a first-person view and the audio
// listener. Returns the character.
Entity spawnFirstPersonPlayer(Scene& scene, const glm::vec3& position,
                              const CharacterControllerComponent& controller = {});
