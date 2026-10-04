// Field keys are the scene document format, so renaming one breaks saved
// scenes unless a schema migration is added. Presentation hints mirror the
// ranges the inspector has always offered.

#include "AnimationComponentTypes.hpp"

#include "ComponentTypeHelpers.hpp"
#include "ComponentTypes.hpp"
#include "SceneAssetReferences.hpp"

#include <nlohmann/json.hpp>

namespace {
using Json = nlohmann::json;
using component_type_helpers::loadModelReference;
using component_type_helpers::modelReference;

constexpr size_t maximumMorphTargets = 64;
} // namespace

ComponentType animationComponentType() {
    return ComponentTypeBuilder<AnimationComponent>("animation", "Animation")
        .category("Animation")
        .codec(
            [](const AnimationComponent& animation, const ComponentSaveContext&, Json& output) {
                Json blendClip = animation.blendClipIndex ? Json(*animation.blendClipIndex) : Json(nullptr);
                output = {{"clip", animation.clipIndex},       {"time", animation.time},
                          {"speed", animation.playbackSpeed},  {"looping", animation.looping},
                          {"playing", animation.playing},      {"blend_clip", std::move(blendClip)},
                          {"blend_time", animation.blendTime}, {"blend_duration", animation.blendDuration}};
                return SceneDocumentResult{.success = true};
            },
            [](const Json& input, AnimationComponent& animation, const ComponentLoadContext&) {
                animation.clipIndex = input.at("clip").get<uint32_t>();
                animation.time = input.at("time").get<float>();
                animation.playbackSpeed = input.at("speed").get<float>();
                animation.looping = input.at("looping").get<bool>();
                animation.playing = input.at("playing").get<bool>();
                if (const auto blend = input.find("blend_clip"); blend != input.end() && !blend->is_null()) {
                    animation.blendClipIndex = blend->get<uint32_t>();
                }
                animation.blendTime = input.value("blend_time", 0.0f);
                animation.blendDuration = input.value("blend_duration", 0.0f);
                return SceneDocumentResult{.success = true};
            })
        .build();
}

ComponentType morphWeightsComponentType() {
    return ComponentTypeBuilder<MorphWeightsComponent>("morph_weights", "Morph Weights")
        .category("Animation")
        .codec(
            [](const MorphWeightsComponent& morph, const ComponentSaveContext&, Json& output) {
                output = {{"weights", morph.weights}};
                return SceneDocumentResult{.success = true};
            },
            [](const Json& input, MorphWeightsComponent& morph, const ComponentLoadContext&) {
                morph.weights = input.at("weights").get<std::vector<float>>();
                return SceneDocumentResult{.success = true};
            })
        .validate([](const MorphWeightsComponent& morph) {
            return morph.weights.size() > maximumMorphTargets
                       ? std::string("Morph-weight component exceeds the 64-target safety limit")
                       : std::string{};
        })
        .build();
}

ComponentType skinComponentType() {
    return ComponentTypeBuilder<SkinComponent>("skin", "Skin")
        .category("Animation")
        .internal()
        .codec(
            [](const SkinComponent& skin, const ComponentSaveContext& context, Json& output) {
                std::string relative;
                if (SceneDocumentResult result =
                        modelReference(context.assets, skin.model, context.options, "skin", relative);
                    !result) {
                    return result;
                }
                output = {{"instance_root", skin.instanceRoot.value}, {"skin", skin.skinIndex}};
                writeAssetReference(relative, context.options, output);
                return SceneDocumentResult{.success = true};
            },
            [](const Json& input, SkinComponent& skin, const ComponentLoadContext& context) {
                if (SceneDocumentResult result = loadModelReference(input, context, skin.model); !result) {
                    return result;
                }
                skin.instanceRoot = SceneEntityId{input.at("instance_root").get<uint64_t>()};
                skin.skinIndex = input.at("skin").get<uint32_t>();
                return SceneDocumentResult{.success = true};
            })
        .build();
}
