#pragma once

#include <imgui.h>

#include <functional>

// Runs ImGui frames without a renderer or window, with docking enabled.
class HeadlessImGui {
  public:
    HeadlessImGui() {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = {1280.0f, 720.0f};
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }

    ~HeadlessImGui() {
        ImGui::DestroyContext();
    }

    HeadlessImGui(const HeadlessImGui&) = delete;
    HeadlessImGui& operator=(const HeadlessImGui&) = delete;

    void frame(const std::function<void()>& draw) {
        ImGui::NewFrame();
        draw();
        ImGui::Render();
    }

    // Presses and releases key (with modifiers) over two frames.
    void press(ImGuiKeyChord chord, const std::function<void()>& draw) {
        ImGuiIO& io = ImGui::GetIO();
        const auto setModifiers = [&io, chord](bool down) {
            io.AddKeyEvent(ImGuiMod_Ctrl, down && (chord & ImGuiMod_Ctrl) != 0);
            io.AddKeyEvent(ImGuiMod_Shift, down && (chord & ImGuiMod_Shift) != 0);
            io.AddKeyEvent(ImGuiMod_Alt, down && (chord & ImGuiMod_Alt) != 0);
        };
        const ImGuiKey key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
        setModifiers(true);
        io.AddKeyEvent(key, true);
        frame(draw);
        io.AddKeyEvent(key, false);
        setModifiers(false);
        frame(draw);
    }
};
