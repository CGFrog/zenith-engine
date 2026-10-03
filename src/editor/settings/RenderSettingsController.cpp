#include "RenderSettingsController.hpp"

#include <utility>

RenderSettingsController::RenderSettingsController(EditorRenderSettings settings)
    : m_settings(std::move(settings)) {}

EditorRenderSettingsResult RenderSettingsController::load(RenderRuntime& rendering) {
    EditorRenderSettingsResult result = m_settings.load();
    apply(rendering);
    return result;
}

EditorRenderSettingsResult RenderSettingsController::save() {
    return m_settings.save();
}

void RenderSettingsController::request(const RenderSettings& settings, RenderRuntime& rendering) {
    m_settings.request(settings);
    apply(rendering);
}

void RenderSettingsController::apply(RenderRuntime& rendering) {
    m_fallbacks = m_settings.apply(rendering);
}
