// CPU-only coverage of animation sampling: linear, step and cubic spline
// channels, looping and clamping, pose blending, and cross-fade playback state.

#include "engine/animation/AnimationSystem.hpp"
#include "engine/scene/Scene.hpp"

#include "support/Doctest.hpp"

#include <cmath>

TEST_SUITE_BEGIN("Animation");

namespace {

AnimationChannel channel(AnimationInterpolation interpolation, AnimationPath path, std::vector<glm::vec4> values) {
    AnimationChannel result;
    result.targetNode = 0;
    result.path = path;
    result.interpolation = interpolation;
    result.times = {0.0f, 1.0f};
    result.values = std::move(values);
    return result;
}

// One node with three one-second clips: 0 linear translation, 1 step scale,
// 2 cubic spline translation.
ModelAsset makeAnimatedModel() {
    ModelAsset model;
    model.nodes.push_back({.translation = {3.0f, 4.0f, 5.0f}});

    AnimationClip linear{.name = "Linear", .duration = 1.0f};
    linear.channels.push_back(channel(AnimationInterpolation::Linear, AnimationPath::Translation,
                                      {{0.0f, 0.0f, 0.0f, 0.0f}, {2.0f, 4.0f, 6.0f, 0.0f}}));
    model.animations.push_back(linear);

    AnimationClip step{.name = "Step", .duration = 1.0f};
    step.channels.push_back(channel(AnimationInterpolation::Step, AnimationPath::Scale,
                                    {{1.0f, 1.0f, 1.0f, 0.0f}, {3.0f, 3.0f, 3.0f, 0.0f}}));
    model.animations.push_back(step);

    AnimationClip cubic{.name = "Cubic", .duration = 1.0f};
    AnimationChannel cubicChannel = channel(AnimationInterpolation::CubicSpline, AnimationPath::Translation,
                                            {{0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f}});
    cubicChannel.inTangents = {{0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}};
    cubicChannel.outTangents = cubicChannel.inTangents;
    cubic.channels.push_back(cubicChannel);
    model.animations.push_back(cubic);
    return model;
}

} // namespace

TEST_CASE("Animation: linear clips interpolate and loop or clamp at their duration") {
    const ModelAsset model = makeAnimatedModel();

    const AnimationPose linearPose = evaluateAnimationClip(model, 0, 0.5f);
    CHECK_MESSAGE((linearPose.size() == 1 && std::abs(linearPose[0].translation.x - 1.0f) < 0.001f &&
                   std::abs(linearPose[0].translation.y - 2.0f) < 0.001f),
                  "linear channels interpolate vector values");
    const AnimationPose loopedPose = evaluateAnimationClip(model, 0, 1.5f);
    CHECK_MESSAGE(std::abs(loopedPose[0].translation.x - 1.0f) < 0.001f, "looping wraps clip time");
    const AnimationPose clampedPose = evaluateAnimationClip(model, 0, 1.5f, false);
    CHECK_MESSAGE(std::abs(clampedPose[0].translation.x - 2.0f) < 0.001f, "non-looping clips clamp at their duration");
}

TEST_CASE("Animation: step and cubic spline channels sample their keys") {
    const ModelAsset model = makeAnimatedModel();

    const AnimationPose stepPose = evaluateAnimationClip(model, 1, 0.75f);
    CHECK_MESSAGE(std::abs(stepPose[0].scale.x - 1.0f) < 0.001f, "step channels retain the preceding key");
    const AnimationPose cubicPose = evaluateAnimationClip(model, 2, 0.5f);
    CHECK_MESSAGE(std::abs(cubicPose[0].translation.x - 0.5f) < 0.001f, "cubic spline channels use Hermite sampling");
}

TEST_CASE("Animation: two animation poses cross-fade") {
    const ModelAsset model = makeAnimatedModel();
    const AnimationPose linearPose = evaluateAnimationClip(model, 0, 0.5f);
    const AnimationPose clampedPose = evaluateAnimationClip(model, 0, 1.5f, false);

    const AnimationPose blended = blendAnimationPoses(linearPose, clampedPose, 0.25f);
    CHECK_MESSAGE(std::abs(blended[0].translation.x - 1.25f) < 0.001f, "two animation poses cross-fade");
}

TEST_CASE("Animation: cross-fade playback state records the target or switches immediately") {
    AnimationComponent playback;
    playback.clipIndex = 0;
    CHECK_MESSAGE((beginAnimationCrossFade(playback, 1, 0.4f) && playback.blendClipIndex == 1u &&
                   std::abs(playback.blendDuration - 0.4f) < 0.001f),
                  "cross-fade state records the target clip and duration");
    beginAnimationCrossFade(playback, 2, 0.0f);
    CHECK_MESSAGE((playback.clipIndex == 2 && !playback.blendClipIndex),
                  "zero-duration transitions switch immediately");
}

TEST_SUITE_END();
