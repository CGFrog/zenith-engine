#pragma once

#include <glm/glm.hpp>

class GameUiContext;
struct ImDrawList;

// Draws a game's UI (what its Ui systems recorded into ui, sized to the game
// view) over the game view image whose top-left corner is origin, in draw
// order. The editor shows the game in an ImGui image rather than through the
// engine's GameUi pass, so this stands in for it: panels and text are drawn
// with ImGui (text in the editor's font), and images, whose textures ImGui
// cannot reach, are drawn as their tint.
void drawGameUiOverlay(ImDrawList& drawList, glm::vec2 origin, const GameUiContext& ui);
