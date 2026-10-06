#pragma once

// Chooses the internal render extent and sample jitter for each frame and decides when temporal history must be
// discarded.

#include "Rendering.hpp"

#include <glm/glm.hpp>

#include <span>

struct TemporalFrameState {
    // Offset of this frame's samples from internal texel centers, in internal
    // pixels; each component lies in [-0.5, 0.5].
    glm::vec2 jitterPixels{0.0f};
    // The same offset in NDC units of the internal render extent.
    glm::vec2 jitterNdc{0.0f};
    uint32_t internalWidth{1};
    uint32_t internalHeight{1};
    float resolutionScale{1.0f};
    bool historyValid{false};
    bool historyInvalidated{false};
    bool cameraCut{false};
};

// Chooses the internal render extent and sample jitter for each frame and
// decides when temporal history must be discarded. History lives at output
// resolution, so dynamic-resolution changes keep it; output resizes, camera
// cuts, explicit invalidation and toggling temporal anti-aliasing discard it.
class TemporalQualityController {
  public:
    TemporalFrameState beginFrame(uint32_t outputWidth, uint32_t outputHeight, double gpuFrameMilliseconds,
                                  const DynamicResolutionSettings& resolution, bool temporalEnabled, bool cameraCut);
    void invalidateHistory();

    float smoothedGpuMilliseconds() const {
        return m_smoothedGpuMilliseconds;
    }

  private:
    float m_scale{1.0f};
    float m_smoothedGpuMilliseconds{0.0f};
    uint32_t m_width{0};
    uint32_t m_height{0};
    uint32_t m_outputWidth{0};
    uint32_t m_outputHeight{0};
    uint32_t m_jitterIndex{0};
    uint32_t m_framesSinceScaleChange{0};
    bool m_historyValid{false};
    bool m_forceInvalidation{true};
};

// Adds a sub-pixel jitter to a perspective projection. A point that projects to
// NDC p without jitter lands at p - jitterNdc, so the internal texel centered at
// c samples the scene at c + jitter.
glm::mat4 applyProjectionJitter(glm::mat4 projection, const glm::vec2& jitterNdc);

// CPU mirror of the motion vector written by mesh.frag: the unjittered UV-space
// displacement from the previous frame to the current one. Clip positions come
// from jittered matrices; jitterNdc holds the current (xy) and previous (zw)
// frame jitter that applyProjectionJitter applied.
glm::vec2 unjitteredMotionUv(const glm::vec4& currentClip, const glm::vec4& previousClip, const glm::vec4& jitterNdc);

// The following mirror shaders/taa_resolve.comp; keep both in sync.
glm::vec3 rgbToYCoCg(const glm::vec3& rgb);
glm::vec3 yCoCgToRgb(const glm::vec3& yCoCg);

// Weight of an internal sample at the given offset (internal pixels) from the
// output pixel center; a Gaussian fit of the Blackman-Harris window.
float temporalReconstructionWeight(const glm::vec2& offsetInternalPixels);

// YCoCg statistics of the internal-resolution neighborhood around a pixel.
struct TemporalNeighborhood {
    glm::vec3 mean{0.0f};
    glm::vec3 standardDeviation{0.0f};
};

TemporalNeighborhood measureTemporalNeighborhood(std::span<const glm::vec3> rgbSamples);

// Clip box half-size in standard deviations; tighter under motion.
float temporalClipGamma(float motionPixels);

// Pulls reprojected history toward the neighborhood mean until it lies inside
// the variance box (mean +/- gamma * standard deviation, in YCoCg).
glm::vec3 clipTemporalHistory(const glm::vec3& historyRgb, const TemporalNeighborhood& neighborhood, float gamma);

// Fraction of the output taken from history before luma weighting.
// sampleConfidence is the peak reconstruction weight of the nearest internal
// sample, motionPixels the motion length in output pixels.
float temporalHistoryWeight(float sampleConfidence, float motionPixels, float reactive);

struct TemporalResolveInputs {
    // Current-frame color reconstructed at the output pixel (linear HDR).
    glm::vec3 current{0.0f};
    // Reprojected history (linear HDR), before clipping.
    glm::vec3 history{0.0f};
    TemporalNeighborhood neighborhood;
    float sampleConfidence{1.0f};
    float motionPixels{0.0f};
    float reactive{0.0f};
    // False when history is invalid, off screen, or disoccluded.
    bool historyAvailable{true};
};

struct TemporalResolveSample {
    glm::vec3 color{0.0f};
    float historyWeight{0.0f};
};

TemporalResolveSample resolveTemporalSample(const TemporalResolveInputs& inputs);
