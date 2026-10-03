#pragma once

#include "engine/diagnostics/Logger.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <glm/glm.hpp>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

class Scene;

template <typename Tag> struct AudioHandle {
    uint32_t index{0};
    uint32_t generation{0};

    explicit operator bool() const {
        return generation != 0;
    }

    auto operator<=>(const AudioHandle&) const = default;
};

using AudioClipHandle = AudioHandle<struct AudioClipTag>;
using AudioStreamHandle = AudioHandle<struct AudioStreamTag>;
using AudioVoiceHandle = AudioHandle<struct AudioVoiceTag>;

enum class AudioBus : uint8_t {
    Master,
    Music,
    Sfx
};
enum class AudioScope : uint8_t {
    Scene,
    Persistent
};

struct AudioConfig {
    uint32_t sampleRate{48'000};
    uint32_t outputChannels{2};
    uint32_t bufferFrames{512};
    uint32_t streamingBufferFrames{48'000};
    uint32_t maxVoices{64};
    uint64_t decodedAudioBudgetBytes{std::numeric_limits<uint64_t>::max()};
    bool outputEnabled{true};
};

struct AudioAssetCollectionResult {
    uint32_t clipsEvicted{0};
    uint32_t streamsEvicted{0};
    uint64_t bytesFreed{0};
    uint64_t bytesRetained{0};
    bool budgetSatisfied{true};
};

struct AudioAssetDiagnostic {
    bool streaming{false};
    uint32_t index{0};
    uint32_t generation{0};
    uint32_t strongReferences{0};
    uint64_t decodedBytes{0};
    std::string source;
};

struct AudioResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

template <typename Handle> struct AudioLoadResult {
    bool success{false};
    std::string error;
    Handle handle{};

    explicit operator bool() const {
        return success;
    }
};

struct AudioPlaybackOptions {
    AudioBus bus{AudioBus::Sfx};
    AudioScope scope{AudioScope::Scene};
    float gain{1.0f};
    float pitch{1.0f};
    bool loop{false};
};

struct AudioSpatialOptions : AudioPlaybackOptions {
    glm::vec3 position{0.0f};
    float minimumDistance{1.0f};
    float maximumDistance{100.0f};
};

struct AudioListenerComponent {
    bool primary{true};
};

struct AudioEmitterComponent {
    AudioClipHandle clip{};
    AudioBus bus{AudioBus::Sfx};
    float gain{1.0f};
    float pitch{1.0f};
    float minimumDistance{1.0f};
    float maximumDistance{100.0f};
    bool loop{false};
    bool play{true};
};

class AudioSystem {
  public:
    AudioSystem();
    // logger receives device messages and must outlive the system; null
    // writes them to stderr.
    explicit AudioSystem(Logger* logger);
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    AudioResult init(const AudioConfig& config = {});
    void shutdown();
    AudioLoadResult<AudioClipHandle> loadClip(const std::filesystem::path& path, bool spatial = false);
    AudioLoadResult<AudioClipHandle> createClip(std::span<const float> samples, uint32_t channels, uint32_t sampleRate);
    AudioLoadResult<AudioStreamHandle> loadStream(const std::filesystem::path& path);
    AudioLoadResult<AudioVoiceHandle> play(AudioClipHandle clip, const AudioPlaybackOptions& options = {});
    AudioLoadResult<AudioVoiceHandle> playAt(AudioClipHandle clip, const AudioSpatialOptions& options);
    AudioLoadResult<AudioVoiceHandle> playMusic(AudioStreamHandle stream,
                                                const AudioPlaybackOptions& options = {
                                                    .bus = AudioBus::Music, .scope = AudioScope::Persistent});
    AudioResult stop(AudioVoiceHandle voice);
    AudioResult setPaused(AudioVoiceHandle voice, bool paused);
    bool isPlaying(AudioVoiceHandle voice) const;
    void stopSceneVoices();
    void stopAll();
    AudioResult setBusGain(AudioBus bus, float gain);
    float busGain(AudioBus bus) const;
    void setBusMuted(AudioBus bus, bool muted);
    bool busMuted(AudioBus bus) const;
    // Copies declarative audio state after another frame subsystem has updated
    // Scene world transforms. This function never recomputes the hierarchy.
    void syncScene(const Scene& scene);
    // Advances playback state in null-output mode; device-backed playback is
    // advanced exclusively by the audio callback.
    void update(float deltaSeconds);
    AudioAssetCollectionResult collectUnusedAssets(bool force = false);
    uint64_t decodedAudioBytes() const;
    std::vector<AudioAssetDiagnostic> assetDiagnostics() const;
    bool hasOutputDevice() const;
    const AudioConfig& config() const;
    void mix(float* interleavedStereo, size_t frameCount);

    struct Impl;

  private:
    std::unique_ptr<Impl> m_impl;
    LogChannel m_log;
};
