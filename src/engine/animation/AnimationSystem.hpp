#pragma once

#include "engine/assets/Assets.hpp"

#include <vector>

class Scene;
struct AnimationComponent;

struct AnimationNodePose {
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

using AnimationPose = std::vector<AnimationNodePose>;

AnimationPose evaluateAnimationClip(const ModelAsset& model, uint32_t clipIndex, float time, bool looping = true);
AnimationPose blendAnimationPoses(const AnimationPose& first, const AnimationPose& second, float weight);
glm::mat4 animationPoseMatrix(const AnimationNodePose& pose);
bool beginAnimationCrossFade(AnimationComponent& animation, uint32_t targetClip, float duration);
void updateAnimations(Scene& scene, const AssetManager& assets, float deltaSeconds);
