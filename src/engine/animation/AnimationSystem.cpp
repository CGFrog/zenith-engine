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
} // namespace

AnimationPose evaluateAnimationClip(const ModelAsset& model, uint32_t clipIndex, float time, bool looping) {
    AnimationPose pose;
    pose.reserve(model.nodes.size());
    for (const ModelNode& node : model.nodes) {
        pose.push_back({node.translation, node.rotation, node.scale});
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
        const glm::vec4 value = sample(channel, sampledTime);
        AnimationNodePose& node = pose[channel.targetNode];
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
    }
    return result;
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

void updateAnimations(Scene& scene, const AssetManager& assets, float deltaSeconds) {
    entt::registry& registry = scene.registry();
    const auto animations = registry.view<AnimationComponent, const ModelComponent, const SceneEntityIdComponent>();
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
        AnimationPose pose = evaluateAnimationClip(*model, animation.clipIndex, animation.time, animation.looping);
        if (animation.blendClipIndex && *animation.blendClipIndex < model->animations.size()) {
            const AnimationPose target =
                evaluateAnimationClip(*model, *animation.blendClipIndex, animation.blendTime, animation.looping);
            const float weight = animation.blendDuration > 0.0f ? animation.blendTime / animation.blendDuration : 1.0f;
            pose = blendAnimationPoses(pose, target, weight);
            if (weight >= 1.0f) {
                animation.clipIndex = *animation.blendClipIndex;
                animation.time = animation.blendTime;
                animation.blendClipIndex.reset();
                animation.blendTime = 0.0f;
                animation.blendDuration = 0.0f;
            }
        }

        const SceneEntityId rootId = animations.get<const SceneEntityIdComponent>(root).id;
        const auto nodes = registry.view<TransformComponent, const ModelNodeComponent>();
        for (Entity entity : nodes) {
            const ModelNodeComponent& node = nodes.get<const ModelNodeComponent>(entity);
            if (node.model == modelHandle && node.instanceRoot == rootId && node.nodeIndex < pose.size()) {
                nodes.get<TransformComponent>(entity).local = animationPoseMatrix(pose[node.nodeIndex]);
                scene.markTransformDirty(entity);
            }
        }
    }

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
    }
}
