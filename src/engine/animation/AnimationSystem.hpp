#pragma once

// Clip sampling, pose blending, cross-fades, and morph weights; updateAnimations.

#include "engine/assets/Assets.hpp"
#include "engine/jobs/FrameTaskGraph.hpp"

#include <vector>

class Scene;
struct AnimationComponent;

struct AnimationNodePose {
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
    std::vector<float> morphWeights;
};

constexpr uint32_t maximumActiveMorphTargets = 8;

struct ActiveMorphTarget {
    uint32_t index{0};
    float weight{0.0f};
};

using AnimationPose = std::vector<AnimationNodePose>;

AnimationPose evaluateAnimationClip(const ModelAsset& model, uint32_t clipIndex, float time, bool looping = true);
AnimationPose blendAnimationPoses(const AnimationPose& first, const AnimationPose& second, float weight);
glm::mat4 animationPoseMatrix(const AnimationNodePose& pose);
bool beginAnimationCrossFade(AnimationComponent& animation, uint32_t targetClip, float duration);
void updateAnimations(Scene& scene, const AssetManager& assets, float deltaSeconds);

struct AnimationTaskResult {
    bool success{false};
    std::string error;
    FrameTaskGraphStats stats;

    explicit operator bool() const {
        return success;
    }
};

AnimationTaskResult updateAnimations(Scene& scene, const AssetManager& assets, float deltaSeconds,
                                     FrameTaskExecutor& executor, uint64_t frameGeneration);
std::vector<ActiveMorphTarget> selectActiveMorphTargets(const std::vector<float>& weights,
                                                        uint32_t limit = maximumActiveMorphTargets);
