# Animation (`src/engine/animation/`)

Part of the [repository guide](../REPOSITORY_GUIDE.md).

## `AnimationSystem.*`

Samples glTF clips (`evaluateAnimationClip`: step, linear, and cubic-spline
channels for translation, rotation, scale, and morph weights), blends two
poses (`blendAnimationPoses`), and advances every `AnimationComponent` each
frame (`updateAnimations`, on the frame-task executor). An animation plays one
clip by index; `beginAnimationCrossFade` starts blending toward another clip
over a duration and promotes it when the fade completes. The engine calls
`updateAnimations` once per frame after the Update systems and timers, with
simulated time.

## `Animator.*`

`AnimatorComponent` (`animator`) is a state machine over the entity's
`AnimationComponent`, for an entity that also has a `ModelComponent` whose
clips the states name. Its `AnimatorDefinition` holds:

- parameters: floats, bools, and triggers, set by gameplay with `setFloat`,
  `setBool`, and `setTrigger`;
- states: a clip name, a playback speed, and whether the clip loops;
- transitions: from a state (or any state, when `from` is empty) to another,
  when every condition holds (`Greater`/`Less` a threshold for floats,
  `True`/`False` for bools, `Triggered` for triggers) and, with an exit time,
  once the state has played that many clip lengths; the clips cross-fade over
  `crossFadeSeconds`;
- event markers: a name at a time in a state's clip;
- the entry state.

Scene JSON stores the definition (`parameters`, `states`, `transitions`,
`events`, `entry_state`); code can build an `AnimatorDefinition` directly.
`validateAnimatorDefinition` rejects names that are missing or duplicated,
conditions that do not fit their parameter, and transitions with nothing to
wait for; a scene with an invalid animator fails to load.

`updateAnimator` runs one animator for a frame: it enters the entry state on
its first run, fires the first transition that is ready (consuming the
triggers it used), advances the state time by the state's speed, and reports
the event markers the playback passed (once per loop). The engine's
`zenith.animator` system (Update, Play, ordered after the game's Update
systems) runs `updateAnimators`, which publishes each marker as an
`AnimationEvent` (`zenith.animation_event`: entity, state, and name) to the
session's `EventQueue`, readable in the next frame. A graph editor is later
work.
