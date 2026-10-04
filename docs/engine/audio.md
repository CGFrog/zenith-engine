# Audio

Part of the [repository guide](../REPOSITORY_GUIDE.md).

## `src/engine/audio/AudioSystem.*` and `StbVorbis.cpp`

Owns SDL output, clip decoding and caching, streamed Ogg decoding, voice
lifetime, bus gain/muting, and scene spatialization. Public clip, stream, and
voice handles are generational; ECS audio components remain declarative.
`StbVorbis.cpp` contains the isolated third-party decoder implementation.

`syncScene`, which the engine calls once per frame after the transforms are
final, places the listener and the `AudioEmitterComponent` voices, and:

- moves voices that follow an entity (`AudioSpatialOptions::follow`, with
  `position` as the offset from it) and stops them once the entity is gone,
  so a one-shot sound can travel with what made it;
- asks the occlusion query (`setOcclusionQuery`) how blocked each voice and
  emitter that wants occlusion is, outside the mixer's lock. The mixer glides
  toward that value with a 50 ms time constant and, fully blocked, keeps 35%
  of the level behind a one-pole low-pass near 1 kHz.

Audio knows nothing of physics: `Engine` answers the query with a
`PhysicsWorld::raycast` from the listener to the source against the Static
layer, so a source behind a static wall is occluded.
