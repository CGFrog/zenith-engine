#include "FpsGame.hpp"

#include "engine/gameplay/FirstPerson.hpp"
#include "engine/module/GameServices.hpp"
#include "engine/module/ModuleRegistrar.hpp"
#include "engine/physics/CharacterController.hpp"
#include "engine/ui/GameMenus.hpp"
#include "engine/ui/GameUi.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>

namespace {
constexpr float messageSeconds = 2.5f;

void showMessage(FpsGameState& state, std::string text) {
    state.message = std::move(text);
    state.messageSeconds = messageSeconds;
}

// The engine's character and camera systems read the player's input through
// these action names; clearing them stops the player while a menu is open.
void setPlayerInput(Scene& scene, FpsGameState& state, bool enabled) {
    auto& registry = scene.registry();
    for (auto [entity, character] : registry.view<CharacterControllerComponent>().each()) {
        if (enabled) {
            if (character.moveAction.empty()) {
                character.moveAction = state.moveAction;
                character.jumpAction = state.jumpAction;
            }
        } else if (!character.moveAction.empty()) {
            state.moveAction = std::exchange(character.moveAction, {});
            state.jumpAction = std::exchange(character.jumpAction, {});
            character.desiredVelocity = glm::vec3(0.0f);
        }
    }
    for (auto [entity, view] : registry.view<FirstPersonCameraComponent>().each()) {
        if (enabled) {
            if (view.lookAction.empty()) {
                view.lookAction = state.lookAction;
            }
        } else if (!view.lookAction.empty()) {
            state.lookAction = std::exchange(view.lookAction, {});
        }
    }
}

void setScreen(Scene& scene, FpsGameState& state, FpsScreen screen) {
    state.screen = screen;
    setPlayerInput(scene, state, screen == FpsScreen::Playing);
}

FirstPersonViewPreferences& viewPreferences(Scene& scene) {
    auto& context = scene.registry().ctx();
    if (FirstPersonViewPreferences* preferences = context.find<FirstPersonViewPreferences>()) {
        return *preferences;
    }
    return context.emplace<FirstPersonViewPreferences>();
}

void updateScore(const SystemContext& context) {
    if (context.services == nullptr) {
        return;
    }
    FpsGameState& state = fpsGameState(context.scene);
    const EventQueue& events = context.services->events();
    for ([[maybe_unused]] const TargetDestroyed& destroyed : events.read<TargetDestroyed>()) {
        ++state.targetsDestroyed;
        showMessage(state,
                    state.targetsDestroyed == state.targetsTotal ? "All targets down. Find the exit!" : "Target down");
        context.services->log("score").info("Targets destroyed: " + std::to_string(state.targetsDestroyed));
    }
    if (!events.read<GoalReached>().empty() && !state.goalReached) {
        state.goalReached = true;
        showMessage(state, "Course complete: " + std::to_string(state.targetsDestroyed) + " of " +
                               std::to_string(state.targetsTotal) + " targets");
    }
    state.messageSeconds = std::max(0.0f, state.messageSeconds - context.deltaSeconds);

    if (context.actions != nullptr && context.actions->pressed("Pause")) {
        switch (state.screen) {
        case FpsScreen::Playing:
            setScreen(context.scene, state, FpsScreen::Paused);
            break;
        case FpsScreen::Paused:
            setScreen(context.scene, state, FpsScreen::Playing);
            break;
        case FpsScreen::Settings:
            state.screen = FpsScreen::Paused;
            break;
        }
    }
}

void drawCrosshair(GameUiContext& ui) {
    const UiStyle style{.color = {1.0f, 1.0f, 1.0f, 0.85f}};
    ui.panel({{-1.0f, -10.0f}, {2.0f, 20.0f}}, style, UiAnchor::Center);
    ui.panel({{-10.0f, -1.0f}, {20.0f, 2.0f}}, style, UiAnchor::Center);
}

void drawMenus(const SystemContext& context, GameUiContext& ui, FpsGameState& state) {
    if (context.actions != nullptr) {
        const InputActionState& actions = *context.actions;
        ui.setNavigation({.nextPressed = actions.pressed("MenuNext"),
                          .previousPressed = actions.pressed("MenuPrevious"),
                          .activatePressed = actions.pressed("MenuAccept"),
                          .leftPressed = actions.pressed("MenuLeft"),
                          .rightPressed = actions.pressed("MenuRight")});
    }
    if (state.screen == FpsScreen::Paused) {
        // The game runs in the editor too, so the menu does not quit.
        switch (drawPauseMenu(ui, {.showQuit = false})) {
        case MenuAction::Resume:
            setScreen(context.scene, state, FpsScreen::Playing);
            break;
        case MenuAction::OpenSettings:
            state.screen = FpsScreen::Settings;
            break;
        default:
            break;
        }
        return;
    }
    FirstPersonViewPreferences& view = viewPreferences(context.scene);
    const std::array<SettingsEntry, 2> entries{
        SettingsEntry{.id = "sensitivity",
                      .label = "Look sensitivity",
                      .control = SettingsSlider{.minimum = 0.25f,
                                                .maximum = 3.0f,
                                                .step = 0.05f,
                                                .get = [&view] { return view.lookScale; },
                                                .set = [&view](float value) { view.lookScale = value; },
                                                .format =
                                                    [](float value) {
                                                        std::array<char, 16> text{};
                                                        std::snprintf(text.data(), text.size(), "%.2fx", value);
                                                        return std::string(text.data());
                                                    }}},
        SettingsEntry{.id = "invert",
                      .label = "Invert look",
                      .control = SettingsToggle{.get = [&view] { return view.invertLookY; },
                                                .set = [&view](bool value) { view.invertLookY = value; }}},
    };
    if (drawSettingsMenu(ui, entries) == MenuAction::Back) {
        state.screen = FpsScreen::Paused;
    }
}

void drawHud(const SystemContext& context) {
    if (context.ui == nullptr) {
        return;
    }
    GameUiContext& ui = *context.ui;
    FpsGameState& state = fpsGameState(context.scene);
    if (state.screen == FpsScreen::Playing) {
        drawCrosshair(ui);
    }
    ui.text({24.0f, 20.0f},
            "Targets " + std::to_string(state.targetsDestroyed) + " / " + std::to_string(state.targetsTotal), 28.0f);
    if (state.messageSeconds > 0.0f) {
        ui.text({0.0f, 96.0f}, state.message, 32.0f, {1.0f, 0.85f, 0.3f, 1.0f}, UiAnchor::TopCenter);
    }
    if (state.screen != FpsScreen::Playing) {
        drawMenus(context, ui, state);
    }
}
} // namespace

FpsGameState& fpsGameState(Scene& scene) {
    auto& context = scene.registry().ctx();
    if (FpsGameState* state = context.find<FpsGameState>()) {
        return *state;
    }
    return context.emplace<FpsGameState>();
}

void registerFpsGame(ModuleRegistrar& registrar) {
    registrar.addSystem({.name = "{{PROJECT_IDENTIFIER}}.score", .phase = SystemPhase::Update, .run = updateScore});
    registrar.addSystem({.name = "{{PROJECT_IDENTIFIER}}.hud", .phase = SystemPhase::Ui, .run = drawHud});
}
