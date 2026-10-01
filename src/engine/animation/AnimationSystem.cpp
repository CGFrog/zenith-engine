#include "AnimationSystem.hpp"

#include "engine/scene/Scene.hpp"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <unordered_map>

namespace {
float playbackTime(float time, float duration, bool looping) {
    if (duration <= 0.0f) {
        return 0.0f;
    }
    if (!looping) {
        return std::clamp(time, 0.0f, duration);
    }
    const float wrapped = std::fmod(time, duration);
    return wrapped < 0.0f ? wrapped + duration : wrapped;
}

glm::quat quaternion(const glm::vec4& value) {
    const glm::quat result(value.w, value.x, value.y, value.z);
    return glm::length(result) > 0.0f ? glm::normalize(result) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
}

glm::vec4 sample(const AnimationChannel& channel, float time) {
    if (channel.times.empty() || channel.values.empty()) {
        return {};
    }
    if (channel.times.size() == 1 || time <= channel.times.front()) {
        return channel.values.front();
    }
    if (time >= channel.times.back()) {
        return channel.values.back();
    }
    const auto upper = std::upper_bound(channel.times.begin(), channel.times.end(), time);
    const size_t next = static_cast<size_t>(upper - channel.times.begin());
    const size_t previous = next - 1;
    const float interval = channel.times[next] - channel.times[previous];
    const float factor = interval > 0.0f ? (time - channel.times[previous]) / interval : 0.0f;
    if (channel.interpolation == AnimationInterpolation::Step) {
        return channel.values[previous];
    }
    if (channel.interpolation == AnimationInterpolation::CubicSpline &&
        channel.outTangents.size() == channel.values.size() && channel.inTangents.size() == channel.values.size()) {
        const float factor2 = factor * factor;
        const float factor3 = factor2 * factor;
        const float h00 = 2.0f * factor3 - 3.0f * factor2 + 1.0f;
        const float h10 = factor3 - 2.0f * factor2 + factor;
        const float h01 = -2.0f * factor3 + 3.0f * factor2;
        const float h11 = factor3 - factor2;
        return h00 * channel.values[previous] + h10 * interval * channel.outTangents[previous] +
               h01 * channel.values[next] + h11 * interval * channel.inTangents[next];
    }
    if (channel.path == AnimationPath::Rotation) {
        const glm::quat interpolated = glm::slerp(quaternion(channel.values[previous]),
                                                  quaternion(channel.values[next]), std::clamp(factor, 0.0f, 1.0f));
        return {interpolated.x, interpolated.y, interpolated.z, interpolated.w};
    }
    return glm::mix(channel.values[previous], channel.values[next], factor);
}

std::vector<float> sampleWeights(const AnimationChannel& channel, float time) {
    if (channel.times.empty() || channel.weightValues.empty()) {
        return {};
    }
    if (channel.times.size() == 1 || time <= channel.times.front()) {
        return channel.weightValues.front();
    }
    if (time >= channel.times.back()) {
        return channel.weightValues.back();
    }
    const size_t next =
        static_cast<size_t>(std::upper_bound(channel.times.begin(), channel.times.end(), time) - channel.times.begin());
    const size_t previous = next - 1;
    if (channel.interpolation == AnimationInterpolation::Step) {
        return channel.weightValues[previous];
    }
    const float interval = channel.times[next] - channel.times[previous];
    const float factor = interval > 0.0f ? (time - channel.times[previous]) / interval : 0.0f;
    const size_t count = std::min(channel.weightValues[previous].size(), channel.weightValues[next].size());
    std::vector<float> result(count);
    for (size_t index = 0; index < count; ++index) {
        if (channel.interpolation == AnimationInterpolation::CubicSpline &&
            previous < channel.weightOutTangents.size() && next < channel.weightInTangents.size() &&
            index < channel.weightOutTangents[previous].size() && index < channel.weightInTangents[next].size()) {
            const float factor2 = factor * factor;
            const float factor3 = factor2 * factor;
            result[index] =
                (2.0f * factor3 - 3.0f * factor2 + 1.0f) * channel.weightValues[previous][index] +
                (factor3 - 2.0f * factor2 + factor) * interval * channel.weightOutTangents[previous][index] +
                (-2.0f * factor3 + 3.0f * factor2) * channel.weightValues[next][index] +
                (factor3 - factor2) * interval * channel.weightInTangents[next][index];
        } else {
            result[index] = glm::mix(channel.weightValues[previous][index], channel.weightValues[next][index], factor);
        }
    }
    return result;
}
} // namespace

AnimationPose evaluateAnimationClip(const ModelAsset& model, uint32_t clipIndex, float time, bool looping) {
    AnimationPose pose;
    pose.reserve(model.nodes.size());
    for (const ModelNode& node : model.nodes) {
        pose.push_back({node.translation, node.rotation, node.scale, node.morphWeights});
    }
    if (clipIndex >= model.animations.size()) {
        return pose;
    }
    const AnimationClip& clip = model.animations[clipIndex];
    const float sampledTime = playbackTime(time, clip.duration, looping);
    for (const AnimationChannel& channel : clip.channels) {
        if (channel.targetNode >= pose.size()) {
            continue;
        }
        AnimationNodePose& node = pose[channel.targetNode];
        if (channel.path == AnimationPath::Weights) {
            node.morphWeights = sampleWeights(channel, sampledTime);
            continue;
        }
        const glm::vec4 value = sample(channel, sampledTime);
        if (channel.path == AnimationPath::Translation) {
            node.translation = glm::vec3(value);
        } else if (channel.path == AnimationPath::Rotation) {
            node.rotation = quaternion(value);
        } else {
            node.scale = glm::vec3(value);
        }
    }
    return pose;
}

AnimationPose blendAnimationPoses(const AnimationPose& first, const AnimationPose& second, float weight) {
    AnimationPose result = first;
    const float factor = std::clamp(weight, 0.0f, 1.0f);
    const size_t count = std::min(result.size(), second.size());
    for (size_t index = 0; index < count; ++index) {
        result[index].translation = glm::mix(result[index].translation, second[index].translation, factor);
        result[index].rotation = glm::normalize(glm::slerp(result[index].rotation, second[index].rotation, factor));
        result[index].scale = glm::mix(result[index].scale, second[index].scale, factor);
        const size_t morphCount = std::min(result[index].morphWeights.size(), second[index].morphWeights.size());
        for (size_t morphIndex = 0; morphIndex < morphCount; ++morphIndex) {
            result[index].morphWeights[morphIndex] =
                glm::mix(result[index].morphWeights[morphIndex], second[index].morphWeights[morphIndex], factor);
        }
    }
    return result;
}

std::vector<ActiveMorphTarget> selectActiveMorphTargets(const std::vector<float>& weights, uint32_t limit) {
    std::vector<ActiveMorphTarget> active;
    active.reserve(std::min<size_t>(weights.size(), limit));
    for (uint32_t index = 0; index < weights.size(); ++index) {
        if (std::abs(weights[index]) > 1.0e-6f) {
            active.push_back({index, weights[index]});
        }
    }
    std::stable_sort(active.begin(), active.end(), [](const ActiveMorphTarget& first, const ActiveMorphTarget& second) {
        return std::abs(first.weight) > std::abs(second.weight);
    });
    if (active.size() > limit) {
        active.resize(limit);
    }
    std::sort(active.begin(), active.end(), [](const ActiveMorphTarget& first, const ActiveMorphTarget& second) {
        return first.index < second.index;
    });
    return active;
}

glm::mat4 animationPoseMatrix(const AnimationNodePose& pose) {
    return glm::translate(glm::mat4(1.0f), pose.translation) * glm::mat4_cast(pose.rotation) *
           glm::scale(glm::mat4(1.0f), pose.scale);
}

bool beginAnimationCrossFade(AnimationComponent& animation, uint32_t targetClip, float duration) {
    if (targetClip == animation.clipIndex || duration <= 0.0f) {
        animation.clipIndex = targetClip;
        animation.time = 0.0f;
        animation.blendClipIndex.reset();
        animation.blendTime = 0.0f;
        animation.blendDuration = 0.0f;
        return true;
    }
    animation.blendClipIndex = targetClip;
    animation.blendTime = 0.0f;
    animation.blendDuration = duration;
    return true;
}

namespace {
struct AnimationEvaluation {
    Entity root{entt::null};
    ModelHandle modelHandle;
    SceneEntityId rootId;
    const ModelAsset* model{nullptr};
    uint32_t clipIndex{0};
    float time{0.0f};
    bool looping{true};
    std::optional<uint32_t> blendClipIndex;
    float blendTime{0.0f};
    float blendDuration{0.0f};
    AnimationPose pose;
};

void evaluateAnimation(AnimationEvaluation& evaluation) {
    evaluation.pose =
        evaluateAnimationClip(*evaluation.model, evaluation.clipIndex, evaluation.time, evaluation.looping);
    if (evaluation.blendClipIndex && *evaluation.blendClipIndex < evaluation.model->animations.size()) {
        const AnimationPose target = evaluateAnimationClip(*evaluation.model, *evaluation.blendClipIndex,
                                                           evaluation.blendTime, evaluation.looping);
        const float weight = evaluation.blendDuration > 0.0f ? evaluation.blendTime / evaluation.blendDuration : 1.0f;
        evaluation.pose = blendAnimationPoses(evaluation.pose, target, weight);
    }
}

void applyAnimationEvaluations(Scene& scene, std::span<const AnimationEvaluation> evaluations) {
    entt::registry& registry = scene.registry();
    for (const AnimationEvaluation& evaluation : evaluations) {
        const auto nodes = registry.view<TransformComponent, const ModelNodeComponent>();
        for (Entity entity : nodes) {
            const ModelNodeComponent& node = nodes.get<const ModelNodeComponent>(entity);
            if (node.model == evaluation.modelHandle && node.instanceRoot == evaluation.rootId &&
                node.nodeIndex < evaluation.pose.size()) {
                nodes.get<TransformComponent>(entity).local = animationPoseMatrix(evaluation.pose[node.nodeIndex]);
                if (!evaluation.pose[node.nodeIndex].morphWeights.empty()) {
                    registry.emplace_or_replace<MorphWeightsComponent>(entity,
                                                                       evaluation.pose[node.nodeIndex].morphWeights);
                }
                scene.markTransformDirty(entity);
            }
        }
    }
}

std::vector<AnimationEvaluation> gatherAnimationEvaluations(Scene& scene, const AssetManager& assets,
                                                            float deltaSeconds) {
    entt::registry& registry = scene.registry();
    std::vector<AnimationEvaluation> evaluations;
    const auto animations = registry.view<AnimationComponent, const ModelComponent, const SceneEntityIdComponent>();
    evaluations.reserve(registry.storage<AnimationComponent>().size());
    for (Entity root : animations) {
        AnimationComponent& animation = animations.get<AnimationComponent>(root);
        const ModelHandle modelHandle = animations.get<const ModelComponent>(root).model;
        const ModelAsset* model = assets.model(modelHandle);
        if (!model || model->animations.empty()) {
            continue;
        }
        animation.clipIndex = std::min(animation.clipIndex, static_cast<uint32_t>(model->animations.size() - 1));
        if (animation.playing) {
            animation.time += deltaSeconds * animation.playbackSpeed;
            if (animation.blendClipIndex) {
                animation.blendTime += deltaSeconds * std::abs(animation.playbackSpeed);
            }
        }
        evaluations.push_back({.root = root,
                               .modelHandle = modelHandle,
                               .rootId = animations.get<const SceneEntityIdComponent>(root).id,
                               .model = model,
                               .clipIndex = animation.clipIndex,
                               .time = animation.time,
                               .looping = animation.looping,
                               .blendClipIndex = animation.blendClipIndex,
                               .blendTime = animation.blendTime,
                               .blendDuration = animation.blendDuration});
        if (animation.blendClipIndex && animation.blendDuration > 0.0f &&
            animation.blendTime / animation.blendDuration >= 1.0f) {
            animation.clipIndex = *animation.blendClipIndex;
            animation.time = animation.blendTime;
            animation.blendClipIndex.reset();
            animation.blendTime = 0.0f;
            animation.blendDuration = 0.0f;
        }
    }
    return evaluations;
}

void updateSkinPalettes(Scene& scene, const AssetManager& assets) {
    entt::registry& registry = scene.registry();

    scene.updateWorldTransforms();
    const auto skins = registry.view<SkinComponent, const TransformComponent>();
    const auto instanceNodes = registry.view<const ModelNodeComponent, const TransformComponent>();
    for (Entity entity : skins) {
        SkinComponent& component = skins.get<SkinComponent>(entity);
        const ModelAsset* model = assets.model(component.model);
        if (!model || component.skinIndex >= model->skins.size()) {
            component.jointPalette.clear();
            continue;
        }
        std::unordered_map<uint32_t, glm::mat4> jointWorlds;
        for (Entity nodeEntity : instanceNodes) {
            const ModelNodeComponent& node = instanceNodes.get<const ModelNodeComponent>(nodeEntity);
            if (node.model == component.model && node.instanceRoot == component.instanceRoot) {
                jointWorlds.emplace(node.nodeIndex, instanceNodes.get<const TransformComponent>(nodeEntity).world);
            }
        }
        const SkinAsset& skin = model->skins[component.skinIndex];
        component.jointPalette.assign(skin.joints.size(), glm::mat4(1.0f));
        const glm::mat4 inverseMeshWorld = glm::inverse(skins.get<const TransformComponent>(entity).world);
        for (size_t jointIndex = 0; jointIndex < skin.joints.size(); ++jointIndex) {
            if (const auto found = jointWorlds.find(skin.joints[jointIndex]); found != jointWorlds.end()) {
                component.jointPalette[jointIndex] =
                    inverseMeshWorld * found->second * skin.inverseBindMatrices[jointIndex];
            }
        }
        scene.markSkinDirty(entity);
    }
}
} // namespace

void updateAnimations(Scene& scene, const AssetManager& assets, float deltaSeconds) {
    std::vector<AnimationEvaluation> evaluations = gatherAnimationEvaluations(scene, assets, deltaSeconds);
    for (AnimationEvaluation& evaluation : evaluations) {
        evaluateAnimation(evaluation);
    }
    applyAnimationEvaluations(scene, evaluations);
    updateSkinPalettes(scene, assets);
}

AnimationTaskResult updateAnimations(Scene& scene, const AssetManager& assets, float deltaSeconds,
                                     FrameTaskExecutor& executor, uint64_t frameGeneration) {
    std::vector<AnimationEvaluation> evaluations = gatherAnimationEvaluations(scene, assets, deltaSeconds);
    FrameTaskGraph graph(frameGeneration, {.minimumGrainSize = 1});
    const FrameResourceUse resources[]{{"scene.animation_snapshot", FrameResourceAccess::Read},
                                       {"frame.animation_poses", FrameResourceAccess::Write}};
    graph.add(
        "animation-evaluation", resources,
        [&](const FrameTaskContext& context) {
            for (size_t index = context.rangeBegin; index < context.rangeEnd; ++index) {
                evaluateAnimation(evaluations[index]);
            }
        },
        evaluations.size());
    FrameTaskResult result = executor.execute(graph);
    if (!result) {
        return {.error = result.error, .stats = result.stats};
    }
    applyAnimationEvaluations(scene, evaluations);
    updateSkinPalettes(scene, assets);
    return {.success = true, .stats = result.stats};
}
