#pragma once

// AudioSystem: device output, clip cache, streaming music, buses, spatial voices, scene sync.

#include "engine/diagnostics/Logger.hpp"
#include "engine/scene/SceneEntityId.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
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

// How much a voice matters when every voice is busy: a new sound takes the
// voice of the lowest-priority sound playing when its own priority is at least
// as high (see AudioSystem::play). Any value of the underlying type is valid;
// the named ones are conventional steps.
enum class AudioPriority : uint8_t {
    Lowest = 0,
    Low = 64,
    Normal = 128,
    High = 192,
    Highest = 255,
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
    // Last, so the members before keep their aggregate order.
    AudioPriority priority{AudioPriority::Normal};
};

struct AudioSpatialOptions : AudioPlaybackOptions {
    // World position, or, with follow, the offset from the followed entity.
    glm::vec3 position{0.0f};
    float minimumDistance{1.0f};
    float maximumDistance{100.0f};
    // An entity the voice moves with: every syncScene places it at the
    // entity's world position plus position, and stops it once the entity is
    // gone. Zero plays at position.
    SceneEntityId follow;
    // Muffles and lowers the voice while geometry blocks it from the
    // listener, as the occlusion query reports (setOcclusionQuery).
    bool occlusion{false};
};

struct AudioListenerComponent {
    bool primary{true};
};

// A looping or one-shot sound placed in the scene. clipAsset is the authored
// clip, a path relative to the project's asset root; the engine's
// zenith.audio_emitters system loads it into clip while gameplay runs. Code
// may also set clip directly and leave clipAsset empty.
struct AudioEmitterComponent {
    AudioClipHandle clip{};
    AudioBus bus{AudioBus::Sfx};
    float gain{1.0f};
    float pitch{1.0f};
    float minimumDistance{1.0f};
    float maximumDistance{100.0f};
    bool loop{false};
    bool play{true};
    // See AudioSpatialOptions::occlusion.
    bool occlusion{false};
    // Last, so the members before keep their aggregate order.
    std::string clipAsset;
    // The clipAsset that clip was loaded from (or failed to load from), so a
    // changed path loads again; runtime only.
    std::string loadedClipAsset;
};

// A clip decoded again from its file: users of previous should move to
// current (see AudioSystem::reloadClip).
struct AudioClipReplacement {
    AudioClipHandle previous;
    AudioClipHandle current;
};

struct AudioClipReloadResult {
    bool success{false};
    std::string error;
    // One entry per way the file was loaded (as is, and mixed to mono for
    // spatial use).
    std::vector<AudioClipReplacement> replaced;

    explicit operator bool() const {
        return success;
    }
};

// Points every AudioEmitterComponent of scene playing a replaced clip at its
// replacement; the emitter's voice restarts on the next syncScene. Returns the
// number of emitters changed.
size_t replaceAudioEmitterClips(Scene& scene, std::span<const AudioClipReplacement> replacements);

// How blocked the path from the listener to a source is, from 0 (clear) to 1
// (fully blocked). The engine answers it with a physics raycast; audio itself
// knows nothing of physics.
using AudioOcclusionQuery = std::function<float(const glm::vec3& listener, const glm::vec3& source)>;

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
    // Decodes the file again for every way it is loaded into fresh clips and
    // updates future lookups. Voices playing a previous clip stop (their
    // handles become invalid); the previous clips stay valid for code still
    // holding them until collectUnusedAssets evicts them. Point emitters at
    // the replacements with replaceAudioEmitterClips. A file that is not
    // loaded (or an uninitialized system) reloads nothing and succeeds; a file
    // that no longer decodes changes nothing and fails.
    AudioClipReloadResult reloadClip(const std::filesystem::path& path);
    AudioLoadResult<AudioClipHandle> createClip(std::span<const float> samples, uint32_t channels, uint32_t sampleRate);
    AudioLoadResult<AudioStreamHandle> loadStream(const std::filesystem::path& path);
    // Plays clip on a free voice. When every voice is busy, the new sound
    // steals the voice of the least important sound playing: the lowest
    // priority, then the quietest (gain, bus gain, and distance attenuation
    // at the listener), then the oldest. It steals only a voice whose
    // priority is at most options.priority, and never the streamed music
    // voice or a voice on the Music bus; otherwise it fails. A stolen
    // voice's handle stops being valid. Scene emitters never steal.
    AudioLoadResult<AudioVoiceHandle> play(AudioClipHandle clip, const AudioPlaybackOptions& options = {});
    // As play, at a position in the world.
    AudioLoadResult<AudioVoiceHandle> playAt(AudioClipHandle clip, const AudioSpatialOptions& options);
    // As play, for the one streamed music voice.
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
    // Plays every Scene-scope voice (emitters included) at rate times its
    // pitch, like a tape running faster or slower (pitch changes with speed);
    // 0 holds them where they are until the rate rises again. Persistent
    // voices (music, menu sounds) keep playing in real time, so a paused game
    // still has its menu sounds. Clamped to [0, 10]; NaN is ignored. The
    // engine sets it from the application's SimulationClock each frame.
    void setPlaybackRate(float rate);
    float playbackRate() const;
    // Copies declarative audio state after another frame subsystem has updated
    // Scene world transforms: the listener, the emitters, the positions of
    // voices that follow entities, and the occlusion of voices that want it.
    // This function never recomputes the hierarchy.
    void syncScene(const Scene& scene);
    // Answers occlusion for voices and emitters that ask for it; empty turns
    // occlusion off. Called from syncScene on the calling thread.
    void setOcclusionQuery(AudioOcclusionQuery query);
    // Advances playback state in null-output mode; device-backed playback is
    // advanced exclusively by the audio callback.
    void update(float deltaSeconds);
    AudioAssetCollectionResult collectUnusedAssets(bool force = false);
    uint64_t decodedAudioBytes() const;
    // Voices taken from playing sounds because every voice was busy, since init.
    uint64_t stolenVoiceCount() const;
    std::vector<AudioAssetDiagnostic> assetDiagnostics() const;
    bool hasOutputDevice() const;
    const AudioConfig& config() const;
    void mix(float* interleavedStereo, size_t frameCount);

    struct Impl;

  private:
    // The parts of syncScene: voices that follow entities, the scene's
    // emitters, then occlusion.
    void followEntities(const Scene& scene);
    void syncEmitters(const Scene& scene);
    void updateOcclusion();

    std::unique_ptr<Impl> m_impl;
    LogChannel m_log;
};
