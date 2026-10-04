# Gameplay mechanisms (`src/engine/gameplay/`)

Part of the [repository guide](../REPOSITORY_GUIDE.md).

Reusable gameplay mechanisms built on the engine's subsystems. They hold no
game rules: a game chooses the actions, numbers, and content. The engine's
built-in systems (`module/BuiltInSystems.*`) drive them in every host.

## `FirstPerson.*`

A first-person player is a `CharacterControllerComponent` (see
[Physics](physics.md)) with a camera child that has a `FirstPersonCameraComponent`
(`first_person_camera`): look sensitivity, the gamepad look rate, pitch limits,
the current yaw and pitch, and the look action's name.

- `applyCharacterInput` sets each character's `desiredVelocity` and
  `jumpRequested` from its move, jump, and sprint actions, relative to
  `characterHeading` (its camera's yaw on top of its own). A character with an
  empty move action is left to code, for example an AI.
- `updateFirstPersonCameras` turns each camera by its look action, clamps the
  pitch, and rotates the camera about its own local position, so it keeps the
  eye height it was placed at. Mouse look is a distance per frame; gamepad look
  is a rate, scaled by `gamepadLookSpeed` and the frame time.
- `FirstPersonViewPreferences` in the scene registry's context scales and
  inverts look input for every camera; a game writes it from its settings.
- `spawnFirstPersonPlayer` creates a character with a camera at the top of the
  capsule that has a first-person view and the audio listener.
- `findCameraChild` is the first child with a `CameraComponent`.

The built-in systems are `zenith.character_input` (FixedUpdate) and
`zenith.first_person_camera` (LateUpdate, after physics placed the
characters). Both run in Play only, and only with player input.
