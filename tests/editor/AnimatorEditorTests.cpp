#include "EditorTestFixture.hpp"
#include "HeadlessImGui.hpp"

#include "editor/interaction/AnimatorEditor.hpp"
#include "editor/operators/AnimationOperators.hpp"

#include "support/Doctest.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <span>
#include <string>
#include <vector>

TEST_SUITE_BEGIN("AnimatorEditor");

// The Animator inspector: renames and removals that keep an animator's
// references whole, parameter retyping, missing-clip reports, an idle frame of
// the inspector, and animation.preview_state with undo.

namespace {

const std::filesystem::path assetRoot{ZENITH_ASSET_DIR};

// Idle <-> Run on a speed parameter, Jump from any state on a trigger, and a
// footstep event in Run.
AnimatorDefinition locomotion() {
    return {.parameters = {{.name = "speed", .type = AnimatorParameterType::Float},
                           {.name = "jump", .type = AnimatorParameterType::Trigger}},
            .states = {{.name = "Idle", .clip = "Idle"},
                       {.name = "Run", .clip = "Run"},
                       {.name = "Jump", .clip = "Jump", .looping = false}},
            .transitions =
                {{.from = "Idle",
                  .to = "Run",
                  .conditions = {{.parameter = "speed", .mode = AnimatorConditionMode::Greater, .threshold = 0.1f}}},
                 {.to = "Jump", .conditions = {{.parameter = "jump", .mode = AnimatorConditionMode::Triggered}}},
                 {.from = "Jump", .to = "Idle", .exitTime = 1.0f}},
            .events = {{.state = "Run", .time = 0.2f, .name = "footstep"}},
            .entryState = "Idle"};
}

TEST_CASE("AnimatorEditor: renames follow references") {
    AnimatorDefinition definition = locomotion();
    AnimatorEditor::renameState(definition, 0, "Rest");
    CHECK_MESSAGE((definition.states[0].name == "Rest" && definition.transitions[0].from == "Rest" &&
                   definition.transitions[2].to == "Rest" && definition.entryState == "Rest"),
                  "renaming a state renames the transitions and the entry state naming it");
    AnimatorEditor::renameState(definition, 1, "Sprint");
    CHECK_MESSAGE(definition.events[0].state == "Sprint", "renaming a state renames its events");

    AnimatorEditor::renameParameter(definition, 0, "velocity");
    CHECK_MESSAGE((definition.parameters[0].name == "velocity" &&
                   definition.transitions[0].conditions[0].parameter == "velocity"),
                  "renaming a parameter renames the conditions testing it");
    CHECK_MESSAGE(validateAnimatorDefinition(definition).empty(), "the renamed animator stays valid");

    CHECK_MESSAGE((AnimatorEditor::unusedStateName(definition, "State") == "State" &&
                   AnimatorEditor::unusedStateName(definition, "Rest") == "Rest 2" &&
                   AnimatorEditor::unusedParameterName(definition, "jump") == "jump 2"),
                  "new names skip the ones in use");
}

TEST_CASE("AnimatorEditor: removals keep the animator valid") {
    AnimatorDefinition definition = locomotion();
    AnimatorEditor::removeState(definition, 0);
    CHECK_MESSAGE((definition.states.size() == 2 && definition.transitions.size() == 1 &&
                   definition.transitions[0].to == "Jump" && definition.entryState.empty()),
                  "removing a state removes its transitions and falls back to the first state");
    CHECK_MESSAGE(validateAnimatorDefinition(definition).empty(), "the animator without the state is valid");

    definition = locomotion();
    AnimatorEditor::removeState(definition, 1);
    CHECK_MESSAGE(definition.events.empty(), "removing a state removes its events");

    definition = locomotion();
    definition.transitions[0].conditions.push_back({.parameter = "jump", .mode = AnimatorConditionMode::Triggered});
    AnimatorEditor::removeParameter(definition, 0);
    CHECK_MESSAGE((definition.parameters.size() == 1 && definition.transitions.size() == 3 &&
                   definition.transitions[0].conditions.size() == 1),
                  "removing a parameter removes only the conditions testing it");
    AnimatorEditor::removeParameter(definition, 0);
    CHECK_MESSAGE((definition.transitions.size() == 1 && definition.transitions[0].from == "Jump"),
                  "a transition left with nothing to wait for is removed");
    CHECK_MESSAGE(validateAnimatorDefinition(definition).empty(), "the animator without parameters is valid");
}

TEST_CASE("AnimatorEditor: retyping a parameter refits its conditions") {
    AnimatorDefinition definition = locomotion();
    definition.parameters[0].value = 3.0f;
    AnimatorEditor::setParameterType(definition, 0, AnimatorParameterType::Bool);
    CHECK_MESSAGE((definition.parameters[0].value == 0.0f &&
                   definition.transitions[0].conditions[0].mode == AnimatorConditionMode::True),
                  "a retyped parameter starts at zero and its conditions take the type's default test");
    CHECK_MESSAGE(validateAnimatorDefinition(definition).empty(), "the retyped animator stays valid");

    using AnimatorEditor::conditionModeFits;
    CHECK_MESSAGE((conditionModeFits(AnimatorConditionMode::Less, AnimatorParameterType::Float) &&
                   !conditionModeFits(AnimatorConditionMode::Less, AnimatorParameterType::Bool) &&
                   conditionModeFits(AnimatorConditionMode::Triggered, AnimatorParameterType::Trigger) &&
                   !conditionModeFits(AnimatorConditionMode::True, AnimatorParameterType::Trigger)),
                  "condition tests fit the parameter types the animator accepts");
}

TEST_CASE("AnimatorEditor: states with missing clips") {
    const AnimatorDefinition definition = locomotion();
    const std::vector<AnimationClip> clips{{.name = "Idle"}, {.name = "Jump"}};
    CHECK_MESSAGE(AnimatorEditor::statesWithMissingClips(definition, clips) == std::vector<std::string>{"Run"},
                  "a state naming a clip the model lacks is reported");
}

TEST_CASE("AnimatorEditor: an idle frame changes nothing") {
    HeadlessImGuiContext imgui;
    AnimatorComponent component{.definition = locomotion()};
    AnimatorEditor::AnimatorEditorState state;
    const std::vector<AnimationClip> clips{{.name = "Idle"}, {.name = "Run"}, {.name = "Jump"}};
    for (const bool withClips : {true, false}) {
        AnimatorEditor::AnimatorEditResult result;
        ImGui::NewFrame();
        ImGui::Begin("Inspector");
        result = AnimatorEditor::drawAnimator(
            component, withClips ? std::span<const AnimationClip>(clips) : std::span<const AnimationClip>{}, state);
        ImGui::End();
        ImGui::Render();
        CHECK_MESSAGE((!result.changed && !result.previewState && state.refusal.empty()),
                      "drawing the inspector without input edits nothing");
    }
    CHECK_MESSAGE(component.definition.states.size() == 3, "the definition is left as it was");
}

struct PreviewFixture : EditorTestFixture {
    SceneEntityId character;

    PreviewFixture()
        : EditorTestFixture(EditorTestFixtureOptions{.documentRoot = assetRoot}) {
        CHECK_MESSAGE(registerAnimationOperators(operators).success, "the animation operators should register");
        const ModelHandle model = assets.loadModel(assetRoot / "morph_triangle.gltf").handle;
        const Entity entity = scene.createEntity("Character");
        CHECK_MESSAGE(scene.instantiateModel(entity, model, assets).success, "the model should instantiate");
        scene.registry().get_or_emplace<AnimationComponent>(entity).playing = false;
        scene.registry().emplace<AnimatorComponent>(
            entity, AnimatorComponent{.definition = {.states = {{.name = "Lift", .clip = "Raise", .speed = 0.5f}}}});
        character = scene.id(entity);
    }

    OperatorResult preview(const std::string& state) {
        return invoke("animation.preview_state", nlohmann::json{{"entity", character.value}, {"state", state}}.dump());
    }

    const AnimationComponent& animation() {
        return scene.registry().get<AnimationComponent>(scene.findEntity(character));
    }
};

TEST_CASE("AnimatorEditor: preview a state with undo") {
    PreviewFixture fixture;
    const OperatorResult result = fixture.preview("Lift");
    CHECK_MESSAGE(result.status == OperatorStatus::Finished, ("previewing a state should finish: " + result.message));
    CHECK_MESSAGE((fixture.animation().playing && fixture.animation().playbackSpeed == 0.5f),
                  "the object plays the state's clip at the state's speed");
    CHECK_MESSAGE(fixture.history.size() == 1, "the preview is one undo step");

    fixture.editing().undo();
    CHECK_MESSAGE((!fixture.animation().playing && fixture.animation().playbackSpeed == 1.0f),
                  "undo stops the preview");

    CHECK_MESSAGE(fixture.preview("Nowhere").status == OperatorStatus::Failed, "a missing state is refused");
}
} // namespace

TEST_SUITE_END();
