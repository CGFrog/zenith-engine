#pragma once

#include <imgui.h>
#include <imgui_internal.h>

#include <cstring>
#include <string_view>

// An ImGui context with a built font atlas and no window or renderer, so
// editor widgets can be drawn and driven frame by frame in CPU-only tests.
class HeadlessImGuiContext {
  public:
    HeadlessImGuiContext() {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = {1280.0f, 720.0f};
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }

    ~HeadlessImGuiContext() {
        ImGui::DestroyContext();
    }

    HeadlessImGuiContext(const HeadlessImGuiContext&) = delete;
    HeadlessImGuiContext& operator=(const HeadlessImGuiContext&) = delete;

    // Activates the item with id at the start of the next frame as keyboard
    // navigation does, preferring text input: a text field takes keyboard
    // focus with its contents selected, a drag turns into a text field, and a
    // button or checkbox is pressed.
    static void activateNextFrame(ImGuiID id) {
        ImGuiContext& context = *ImGui::GetCurrentContext();
        context.NavNextActivateId = id;
        context.NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
    }

    // Queues typed characters for the next frame.
    static void typeNextFrame(const char* text) {
        ImGui::GetIO().AddInputCharactersUTF8(text);
    }

    // The window drawn last frame whose name contains nameFragment, for
    // example a child window's "##Content" id; null when there is none.
    static ImGuiWindow* findWindow(std::string_view nameFragment) {
        for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows) {
            if (std::string_view(window->Name).find(nameFragment) != std::string_view::npos) {
                return window;
            }
        }
        return nullptr;
    }

    // The id ImGui gives an item reached by pushing each of path's strings
    // onto window's id stack, as PushID and a final widget label do.
    template <typename... Path> static ImGuiID itemId(const ImGuiWindow& window, Path... path) {
        ImGuiID id = window.ID;
        ((id = ImHashStr(path, 0, id)), ...);
        return id;
    }
};

// One frame drawing into a fixed full-size window named title.
class HeadlessImGuiFrame {
  public:
    explicit HeadlessImGuiFrame(const char* title = "Headless test") {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0.0f, 0.0f});
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin(title);
    }

    ~HeadlessImGuiFrame() {
        ImGui::End();
        ImGui::Render();
    }

    HeadlessImGuiFrame(const HeadlessImGuiFrame&) = delete;
    HeadlessImGuiFrame& operator=(const HeadlessImGuiFrame&) = delete;
};
