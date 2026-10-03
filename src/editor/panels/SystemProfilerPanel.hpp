#pragma once

#include "editor/panels/EditorPanel.hpp"

class EditorPlayMode;
class GameModuleHost;

// Where the frame's CPU and GPU time goes: every game module system with its
// phase, the modes it runs in, and its time this frame, rolling average, and
// maximum; the engine loop's CPU phases; and the GPU render passes. Each
// system that can run in Play has a Simulate checkbox choosing whether
// Simulate runs it. Changes go through operators (play.set_time_scale,
// play.set_simulate_system, profiler.reset_max), so the panel only reads the
// host and play mode it is given, which must outlive it.
class SystemProfilerPanel final : public EditorPanel {
  public:
    SystemProfilerPanel(const GameModuleHost& modules, const EditorPlayMode& playMode);

    const char* title() const override;
    void draw(EditorContext& context) override;

  private:
    void drawRuntimeControls(EditorContext& context) const;
    void drawSystems(EditorContext& context) const;

    const GameModuleHost& m_modules;
    const EditorPlayMode& m_playMode;
};
