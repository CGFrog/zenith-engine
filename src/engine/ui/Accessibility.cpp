#include "engine/ui/Accessibility.hpp"

#include <algorithm>

bool CaptionTrack::setCues(std::vector<CaptionCue> cues, std::string& error) {
    for (const CaptionCue& cue : cues) {
        if (cue.startSeconds < 0.0 || cue.endSeconds <= cue.startSeconds || cue.messageId.empty()) {
            error = "caption cues require a stable message ID and a positive time range";
            return false;
        }
    }
    std::stable_sort(cues.begin(), cues.end(), [](const CaptionCue& first, const CaptionCue& second) {
        return first.startSeconds < second.startSeconds;
    });
    m_cues = std::move(cues);
    error.clear();
    return true;
}

std::vector<const CaptionCue*> CaptionTrack::active(double timeSeconds) const {
    std::vector<const CaptionCue*> result;
    for (const CaptionCue& cue : m_cues) {
        if (cue.startSeconds <= timeSeconds && timeSeconds < cue.endSeconds) {
            result.push_back(&cue);
        }
        if (cue.startSeconds > timeSeconds) {
            break;
        }
    }
    return result;
}

AccessibleIndicator makeAccessibleIndicator(uint32_t category, std::string label) {
    return {.category = category, .shape = static_cast<IndicatorShape>(category % 4), .label = std::move(label)};
}
