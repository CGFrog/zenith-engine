#pragma once

#include <filesystem>
#include <string>

struct ImGuiIO;
struct ImGuiStyle;

struct EditorFontResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// Dark, flat editor styling modeled on Blender's default theme.
void applyEditorTheme(ImGuiStyle& style);

// Loads the editor UI font and makes it the ImGui default. Must run before the
// first UI frame so the backend uploads the atlas with this font included.
// pixelSize is in logical pixels.
EditorFontResult loadEditorFont(ImGuiIO& io, const std::filesystem::path& fontPath, float pixelSize);
