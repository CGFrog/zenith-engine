# Gameplay mechanisms (`src/engine/gameplay/`)

Part of the [repository guide](../REPOSITORY_GUIDE.md).

Reusable gameplay mechanisms built on the engine's subsystems. They hold no
game rules: a game chooses the actions, numbers, and content. The engine's
built-in systems (`module/BuiltInSystems.*`) drive the first-person player in
every host; games draw from the random streams through `GameServices` and
load their content through definition tables.

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

## `Random.*`

Seeded randomness whose results are the same on every platform, compiler,
and build type, so a seed replays the same run.

- `RandomStream` is PCG32 (XSH RR, 64-bit state, selectable sequence) with
  explicit `State` (`state()`, `setState`). Its helpers do not use
  `<random>`'s distributions, which differ between standard libraries:
  - `below(bound)` is unbiased (rejection);
  - `range` for ints (both bounds included) and floats;
  - `unit()` is in [0, 1) in steps of 2^-24;
  - `chance(probability)` always draws once;
  - `weightedIndex(weights)` ignores non-positive and non-finite weights;
  - `shuffle` is Fisher-Yates.

  Integer results are exact everywhere. Float results also need IEEE single
  precision without fused multiply-add contraction.
- `RandomStreams` holds named streams derived from one seed. A stream starts
  the first time its name is asked for, from the seed and the name alone
  (SplitMix64 over the seed and the name's FNV-1a hash), so drawing from
  `loot` never changes `layout`. `states()` lists the used streams for a
  save, and `restore(seed, states)` resumes them.
- `entropySeed()` is a seed from `std::random_device`, for a run whose seed
  the player did not choose.

`GameServices::random(name)` is the session's stream for that name, and
`randomStreams()` is the whole set. The seed starts at 0; a game reseeds it
when a run begins. `tests/engine/RandomTests.cpp` checks PCG32's reference
sequence and pins the named-stream derivation with golden values. Changing
the derivation changes every saved seed's game.

## `DefinitionTable.*`

Data-driven content: `DefinitionTable<Entry>` loads a JSON file holding an
array of objects, one reflected `Entry` each (items, enemy species, loot
tables, floor themes), keyed by a required string id field (`id` by
default). The game defines `Entry` and its `Fields`, including
`Fields::list` for nested lists such as a loot table's drops.

- `load(path)` and `parse(text, source)`, then `find(id)` and `entries()`
  in file order.
- Loading is strict, since the file is written by hand. An unknown key at
  any depth (a typo; scene documents ignore unknown keys instead), a wrong
  type, a missing required field, a missing, empty, or duplicate id, broken
  JSON, and a file that is not an array are errors. Each names the file, the
  entry's index and id, and the field path, for example
  `loot.json[0] 'crypt': unknown field 'drops[1].wieght'`. A failed load
  keeps the table's previous entries.
- `reloadIfChanged()` loads the file again when its write time or size
  changed since the last attempt, so a game picks up edits while playing by
  calling it from a system. A broken edit is reported once and retried only
  after the file changes again.

`tests/engine/DefinitionTableTests.cpp` (`zenith_definition_tables`).
