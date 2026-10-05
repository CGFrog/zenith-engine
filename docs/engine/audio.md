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

The playback rate (`setPlaybackRate`, which the engine sets from the
application's `SimulationClock`) applies to `AudioScope::Scene` voices only:
a paused or slowed game holds or slows its world's sounds, while
`Persistent` voices (music, menu sounds) play on in real time.

When all `AudioConfig::maxVoices` voices are busy, `play`, `playAt`, and
`playMusic` steal one by priority (`AudioPlaybackOptions::priority`, an
`AudioPriority` from `Lowest` to `Highest`, `Normal` by default): the voice
with the lowest priority, then the quietest at the listener (gain through its
bus and the master bus, and distance attenuation), then the oldest. A request
steals only a voice whose priority is at most its own, and never the streamed
music voice or a voice on the Music bus; otherwise it fails as before. The
stolen sound's handle stops being valid. `AudioEmitterComponent` voices play
at `Normal` priority and only take free voices. A run of plays that find every
voice busy is logged once, and `stolenVoiceCount()` counts the steals.

`reloadClip(path)` decodes a changed file again for every way it is loaded (as
is, and mixed to mono for spatial use) into new clip generations and updates
the cache, so later `loadClip` calls return the new clips. Voices playing a
previous clip stop under the mixer's lock (their handles become invalid); the
previous clips stay valid for code still holding them until
`collectUnusedAssets` evicts them. `replaceAudioEmitterClips(scene,
replacements)` points `AudioEmitterComponent`s at the new clips, and the next
`syncScene` restarts their voices. A file nobody loaded reloads nothing; one
that no longer decodes changes nothing and fails. The editor calls it when a
sound changes on disk or is reimported (`project/AssetReload`).

## Audio in scenes and gameplay

`audio_listener` and `audio_emitter` are serialized component types
(`scene/AudioComponentTypes.cpp`, category Audio). An emitter's sound is
`clip_asset`, a path relative to the project's asset root saved as an asset
reference (with its GUID); the runtime `clip` handle is not saved. While
gameplay runs, the built-in `zenith.audio_emitters` system (LateUpdate, Play)
loads each emitter's clip when its path changes, through
`GameServices::loadClip`, and logs a clip that cannot load once. Code may
instead set `clip` and leave `clip_asset` empty.

Gameplay plays its own sounds through `GameServices::audio()` (null without
audio, as in tests): `play`, `playAt` (with `follow` to move with an entity),
and `stop` by voice handle. `GameServices::loadClip(assetPath)` resolves a
path against the session's asset root and rejects paths outside it. Both
hosts give their services the application's audio (`setAudio`).
`tests/engine/AudioSystemTests.cpp` (`testGameplayAudio`) covers them.
