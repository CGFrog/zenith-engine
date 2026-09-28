# 5. Add HUD, audio, and game flow

## HUD

Zenith exposes Dear ImGui through `ApplicationScene::drawUi`. It is suitable
for a prototype HUD and debugging tools:

```cpp
void TrainingScene::drawUi(ApplicationContext&) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowBgAlpha(0.0f);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                                       ImGuiWindowFlags_NoInputs |
                                       ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("##Crosshair", nullptr, flags)) {
        ImGui::TextUnformatted("+");
    }
    ImGui::End();
}
```

Add a second anchored window for health, ammunition, score, and a restart hint.
Read component values; do not make the HUD own authoritative gameplay state.

## Sound effects

Load immutable audio resources once in `FpsApplication::onStart`, then pass
their handles into `TrainingScene`. This mirrors `DemoApplication` and keeps
application-lifetime assets outside individual scene reloads:

```cpp
const auto shot = context.audio.loadClip(
    std::filesystem::path{ZENITH_ASSET_DIR} / "audio" / "shot.wav", false);
if (!shot) {
    return {.error = shot.error};
}
m_shotSound = shot.handle;
```

For a first-person shot, play a non-spatial SFX voice. For an impact or enemy
sound, use `playAt` with its world position:

```cpp
AudioSpatialOptions options;
options.position = hit.position;
options.gain = 0.8f;
context.audio.playAt(m_impactSound, options);
```

The camera's `AudioListenerComponent` supplies the listener transform. Scene
voices stop on scene transitions; persistent music should be started by the
application with `AudioScope::Persistent` and stopped in `onStop`.

## Restart and game states

Scene changes are deferred to a frame boundary. Restart the range without
mutating lifecycle state inside a callback:

```cpp
if (restartRequested) {
    context.control.requestSceneChange(fpsSceneKey(FpsSceneId::Training));
}
```

For a larger game, add `Menu`, `Training`, and `Results` to `FpsSceneId`, and
construct a distinct `ApplicationScene` for each. Shared profile or settings
state belongs to `FpsApplication` or another explicit game-owned object passed
to scenes—not to globals and not to `zenith_engine`.

## Cleanup

`ApplicationSession` stops scene-scoped audio, clears physics, and clears ECS
state after `unload`. `TrainingScene::unload` can remain empty unless it owns
additional resources outside those systems. The application must stop any
persistent music in `onStop`.

## Authoring versus runtime Play mode

Use `zenith_editor` to construct and save the static level. The editor's Play
mode protects the authored scene by running a clone, but it does not replace
your FPS executable's `TrainingScene` systems. Run `zenith_fps_game` to test the
actual controller, weapons, scoring, and application transitions.
