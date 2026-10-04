// Field keys are the scene document format, so renaming one breaks saved
// scenes unless a schema migration is added.

#include "AudioComponentTypes.hpp"

#include "ComponentTypes.hpp"
#include "engine/audio/AudioSystem.hpp"

#include <string>

ComponentType audioListenerComponentType() {
    using C = AudioListenerComponent;
    return ComponentTypeBuilder<C>("audio_listener", "Audio Listener")
        .category("Audio")
        .fields(Fields<C>()
                    .add("primary", &C::primary)
                    .optional()
                    .tooltip("The first primary listener hears the scene; usually on the player's camera"))
        .build();
}

ComponentType audioEmitterComponentType() {
    using C = AudioEmitterComponent;
    // Every field is optional and defaults as in AudioEmitterComponent; an
    // emitter without a clip asset plays the clip code gives it.
    return ComponentTypeBuilder<C>("audio_emitter", "Audio Emitter")
        .category("Audio")
        .fields(Fields<C>()
                    .add("clip_asset", &C::clipAsset)
                    .optional()
                    .label("Clip")
                    .tooltip("A WAV or Ogg file in the project's assets")
                    .add("bus", &C::bus)
                    .optional()
                    .options({{"master", "Master", static_cast<int64_t>(AudioBus::Master)},
                              {"music", "Music", static_cast<int64_t>(AudioBus::Music)},
                              {"sfx", "Effects", static_cast<int64_t>(AudioBus::Sfx)}})
                    .add("gain", &C::gain)
                    .optional()
                    .drag(0.01f, 0.0, 16.0)
                    .add("pitch", &C::pitch)
                    .optional()
                    .drag(0.01f, 0.01, 4.0)
                    .add("minimum_distance", &C::minimumDistance)
                    .optional()
                    .drag(0.05f, 0.001, 10000.0)
                    .format("%.2f m")
                    .tooltip("Full volume within this distance")
                    .add("maximum_distance", &C::maximumDistance)
                    .optional()
                    .drag(0.1f, 0.001, 100000.0)
                    .format("%.1f m")
                    .tooltip("Silent beyond this distance")
                    .add("loop", &C::loop)
                    .optional()
                    .add("play", &C::play)
                    .optional()
                    .add("occlusion", &C::occlusion)
                    .optional()
                    .tooltip("Muffled while static geometry blocks it from the listener"))
        .assetReference(&C::clipAsset)
        .validate([](const C& emitter) -> std::string {
            if (!(emitter.gain >= 0.0f) || !(emitter.pitch > 0.0f)) {
                return "an audio emitter's gain must not be negative and its pitch must be positive";
            }
            if (!(emitter.minimumDistance > 0.0f) || !(emitter.maximumDistance >= emitter.minimumDistance)) {
                return "an audio emitter's minimum distance must be positive and not above its maximum";
            }
            return {};
        })
        .build();
}
