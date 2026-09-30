#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

struct AccessibilitySettings {
    float uiScale{1.0f};
    float textScale{1.0f};
    bool highContrast{false};
    bool reducedMotion{false};
    bool captionsEnabled{true};
    bool colorIndependentIndicators{true};
};

struct CaptionCue {
    double startSeconds{0.0};
    double endSeconds{0.0};
    std::string messageId;
    std::string speaker;
};

class CaptionTrack {
  public:
    bool setCues(std::vector<CaptionCue> cues, std::string& error);
    std::vector<const CaptionCue*> active(double timeSeconds) const;

  private:
    std::vector<CaptionCue> m_cues;
};

enum class IndicatorShape : uint8_t {
    Circle,
    Triangle,
    Square,
    Diamond
};

struct AccessibleIndicator {
    uint32_t category{0};
    IndicatorShape shape{IndicatorShape::Circle};
    std::string label;
};

AccessibleIndicator makeAccessibleIndicator(uint32_t category, std::string label);
