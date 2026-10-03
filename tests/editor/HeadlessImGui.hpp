#pragma once

#include <imgui.h>

// One ImGui frame inside a window, without a renderer or platform backend, so
// editor widgets can be drawn in CPU-only tests. Draw between construction
// and destruction.
class HeadlessFrame {
  public:
    explicit HeadlessFrame(const char* windowTitle = "Headless test") {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = {1280.0f, 720.0f};
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        ImGui::NewFrame();
        ImGui::Begin(windowTitle);
    }

    ~HeadlessFrame() {
        ImGui::End();
        ImGui::Render();
        ImGui::DestroyContext();
    }

    HeadlessFrame(const HeadlessFrame&) = delete;
    HeadlessFrame& operator=(const HeadlessFrame&) = delete;
};
