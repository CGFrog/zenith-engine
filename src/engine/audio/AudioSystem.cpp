#include "AudioSystem.hpp"

#include "engine/diagnostics/OverflowEpisode.hpp"
#include "engine/scene/Scene.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

namespace {
template <typename T> T readLittle(const std::byte* source) {
    T value{};
    std::memcpy(&value, source, sizeof(T));
    return value;
}

std::filesystem::path canonicalPath(const std::filesystem::path& path) {
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    return error ? std::filesystem::absolute(path).lexically_normal() : canonical;
}

struct DecodedAudio {
    std::vector<float> samples;
    uint32_t channels{0};
    uint32_t sampleRate{0};
    std::string error;
};

DecodedAudio decodeWav(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return {.error = "Cannot open WAV file: " + path.string()};
    }
    const auto length = input.tellg();
    if (length < 44) {
        return {.error = "Malformed WAV file: " + path.string()};
    }
    std::vector<std::byte> bytes(static_cast<size_t>(length));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), length);
    if (!input || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        return {.error = "Malformed WAV file: " + path.string()};
    }

    uint16_t format = 0;
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint16_t bits = 0;
    const std::byte* pcm = nullptr;
    size_t pcmBytes = 0;
    for (size_t offset = 12; offset + 8 <= bytes.size();) {
        const uint32_t chunkSize = readLittle<uint32_t>(bytes.data() + offset + 4);
        const size_t body = offset + 8;
        if (body + chunkSize > bytes.size()) {
            return {.error = "Malformed WAV chunk: " + path.string()};
        }
        if (std::memcmp(bytes.data() + offset, "fmt ", 4) == 0 && chunkSize >= 16) {
            format = readLittle<uint16_t>(bytes.data() + body);
            channels = readLittle<uint16_t>(bytes.data() + body + 2);
            sampleRate = readLittle<uint32_t>(bytes.data() + body + 4);
            bits = readLittle<uint16_t>(bytes.data() + body + 14);
        } else if (std::memcmp(bytes.data() + offset, "data", 4) == 0) {
            pcm = bytes.data() + body;
            pcmBytes = chunkSize;
        }
        offset = body + chunkSize + (chunkSize & 1U);
    }
    if (!pcm || sampleRate == 0 || (channels != 1 && channels != 2)) {
        return {.error = "Unsupported WAV channel layout or missing data: " + path.string()};
    }
    const size_t sampleBytes = bits / 8;
    if (!((format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (format == 3 && bits == 32)) ||
        sampleBytes == 0 || pcmBytes % (sampleBytes * channels) != 0) {
        return {.error = "Unsupported or malformed WAV sample data: " + path.string()};
    }
    DecodedAudio decoded;
    decoded.channels = channels;
    decoded.sampleRate = sampleRate;
    decoded.samples.resize(pcmBytes / sampleBytes);
    for (size_t i = 0; i < decoded.samples.size(); ++i) {
        const std::byte* source = pcm + i * sampleBytes;
        if (format == 3) {
            decoded.samples[i] = readLittle<float>(source);
        } else if (bits == 8) {
            decoded.samples[i] = (static_cast<float>(std::to_integer<uint8_t>(*source)) - 128.0f) / 128.0f;
        } else if (bits == 16) {
            decoded.samples[i] = static_cast<float>(readLittle<int16_t>(source)) / 32768.0f;
        } else if (bits == 24) {
            int32_t value = std::to_integer<uint8_t>(source[0]) | (std::to_integer<uint8_t>(source[1]) << 8) |
                            (std::to_integer<uint8_t>(source[2]) << 16);
            if ((value & 0x800000) != 0) {
                value |= ~0xffffff;
            }
            decoded.samples[i] = static_cast<float>(value) / 8388608.0f;
        } else {
            decoded.samples[i] = static_cast<float>(readLittle<int32_t>(source)) / 2147483648.0f;
        }
    }
    return decoded;
}

DecodedAudio decodeOgg(const std::filesystem::path& path) {
    int channels = 0;
    int sampleRate = 0;
    short* source = nullptr;
    const int frames = stb_vorbis_decode_filename(path.string().c_str(), &channels, &sampleRate, &source);
    if (frames < 0 || source == nullptr) {
        return {.error = "Cannot decode Ogg Vorbis file: " + path.string()};
    }
    if (channels != 1 && channels != 2) {
        free(source);
        return {.error = "Unsupported Ogg channel layout: " + path.string()};
    }
    DecodedAudio decoded;
    decoded.channels = static_cast<uint32_t>(channels);
    decoded.sampleRate = static_cast<uint32_t>(sampleRate);
    decoded.samples.resize(static_cast<size_t>(frames) * decoded.channels);
    std::transform(source, source + decoded.samples.size(), decoded.samples.begin(),
                   [](short sample) { return static_cast<float>(sample) / 32768.0f; });
    free(source);
    return decoded;
}

DecodedAudio decode(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (extension == ".wav") {
        return decodeWav(path);
    }
    if (extension == ".ogg") {
        return decodeOgg(path);
    }
    return {.error = "Unsupported audio file extension: " + path.string()};
}

size_t busIndex(AudioBus bus) {
    return static_cast<size_t>(bus);
}
} // namespace

struct AudioSystem::Impl {
    struct Clip {
        std::filesystem::path path;
        std::vector<float> samples;
        uint32_t channels{0};
        uint32_t sampleRate{0};
        uint32_t generation{1};
        bool occupied{true};
    };

    struct Stream {
        std::filesystem::path path;
        uint32_t channels{0};
        uint32_t sampleRate{0};
        uint32_t generation{1};
        bool occupied{true};
    };

    struct Voice {
        uint32_t generation{1};
        bool active{false};
        AudioPriority priority{AudioPriority::Normal};
        // When the voice started (Impl::nextStartSequence); orders voice stealing.
        uint64_t startSequence{0};
        bool paused{false};
        bool spatial{false};
        bool streaming{false};
        bool loop{false};
        AudioScope scope{AudioScope::Scene};
        AudioBus bus{AudioBus::Sfx};
        uint32_t assetIndex{0};
        double cursor{0.0};
        float gain{1.0f};
        float pitch{1.0f};
        glm::vec3 position{0.0f};
        float minimumDistance{1.0f};
        float maximumDistance{100.0f};
        // The followed entity and the offset from it (AudioSpatialOptions).
        SceneEntityId follow;
        glm::vec3 followOffset{0.0f};
        bool occlusion{false};
        // How blocked the voice is (0..1): the target syncScene sets, and the
        // value the mixer glides toward it per frame, so changes do not click.
        float occlusionTarget{0.0f};
        float occlusionCurrent{0.0f};
        // One-pole low-pass state per channel.
        float lowPassLeft{0.0f};
        float lowPassRight{0.0f};
    };

    struct BusState {
        float gain{1.0f};
        bool muted{false};
    };

    struct EmitterRuntime {
        AudioVoiceHandle voice;
        AudioClipHandle clip;
        uint64_t lastSeenSync{0};
    };

    struct ListenerState {
        std::atomic<float> positionX{0.0f};
        std::atomic<float> positionY{0.0f};
        std::atomic<float> positionZ{0.0f};
        std::atomic<float> rightX{1.0f};
        std::atomic<float> rightY{0.0f};
        std::atomic<float> rightZ{0.0f};
        std::atomic<bool> valid{false};
    };

    AudioConfig config;
    bool initialized{false};
    bool audioSubsystem{false};
    SDL_AudioStream* output{nullptr};
    std::vector<float> callbackScratch;
    std::vector<Clip> clips;
    std::vector<Stream> streams;
    std::vector<Voice> voices;
    std::unordered_map<std::string, AudioClipHandle> clipCache;
    std::unordered_map<std::string, AudioStreamHandle> streamCache;
    std::unordered_map<uint32_t, EmitterRuntime> emitters;
    uint64_t emitterSync{0};
    std::array<BusState, 3> buses;
    // Multiplies every voice's step; 0 pauses all playback. Guarded by mutex.
    float playbackRate{1.0f};
    ListenerState listener;
    // Called on syncScene's thread, outside the mutex.
    AudioOcclusionQuery occlusionQuery;
    mutable std::mutex mutex;
    std::thread decoderThread;
    std::atomic<bool> decoderStop{false};
    std::mutex streamWaitMutex;
    std::condition_variable streamWake;
    std::vector<float> streamRing;
    std::atomic<uint64_t> streamRead{0};
    std::atomic<uint64_t> streamWrite{0};
    AudioVoiceHandle streamingVoice{};
    std::atomic<bool> streamEof{false};
    // Voice stealing: the start order of voices, the voices stolen since init,
    // and the run of plays that found every voice busy (logged once).
    uint64_t nextStartSequence{0};
    uint64_t stolenVoices{0};
    OverflowEpisode voiceLimit;

    bool valid(AudioClipHandle handle) const {
        return handle && handle.index < clips.size() && clips[handle.index].occupied &&
               clips[handle.index].generation == handle.generation;
    }

    bool valid(AudioStreamHandle handle) const {
        return handle && handle.index < streams.size() && streams[handle.index].occupied &&
               streams[handle.index].generation == handle.generation;
    }

    bool valid(AudioVoiceHandle handle) const {
        return handle && handle.index < voices.size() && voices[handle.index].generation == handle.generation &&
               voices[handle.index].active;
    }

    // How loud the voice reaches the listener: its gain through its bus and
    // the master bus, and for a spatial voice the distance attenuation. Pan
    // and occlusion are left out. Requires mutex.
    float audibility(const Voice& voice) const {
        const BusState master = buses[busIndex(AudioBus::Master)];
        const BusState child = voice.bus == AudioBus::Master ? BusState{} : buses[busIndex(voice.bus)];
        const float gain = voice.gain * child.gain * master.gain * (child.muted || master.muted ? 0.0f : 1.0f);
        if (!voice.spatial) {
            return gain;
        }
        if (!listener.valid.load(std::memory_order_acquire)) {
            return 0.0f;
        }
        const glm::vec3 listenerPosition{listener.positionX.load(std::memory_order_relaxed),
                                         listener.positionY.load(std::memory_order_relaxed),
                                         listener.positionZ.load(std::memory_order_relaxed)};
        const float distance = glm::length(voice.position - listenerPosition);
        const float minimum = std::max(voice.minimumDistance, 0.001f);
        const float maximum = std::max(voice.maximumDistance, minimum);
        return distance >= maximum ? 0.0f : gain * std::min(1.0f, minimum / std::max(distance, minimum));
    }

    // A free voice, or the voice a request of the given priority may steal
    // (see AudioSystem::play); voices.end() when there is neither. Requires
    // mutex.
    std::vector<Voice>::iterator acquireVoice(AudioPriority priority, bool& stolen) {
        stolen = false;
        const auto free = std::find_if(voices.begin(), voices.end(), [](const Voice& voice) { return !voice.active; });
        if (free != voices.end()) {
            return free;
        }
        auto victim = voices.end();
        float victimAudibility = 0.0f;
        for (auto candidate = voices.begin(); candidate != voices.end(); ++candidate) {
            if (candidate->streaming || candidate->bus == AudioBus::Music || candidate->priority > priority) {
                continue;
            }
            const float candidateAudibility = audibility(*candidate);
            if (victim == voices.end() || candidate->priority < victim->priority ||
                (candidate->priority == victim->priority &&
                 (candidateAudibility < victimAudibility ||
                  (candidateAudibility == victimAudibility && candidate->startSequence < victim->startSequence)))) {
                victim = candidate;
                victimAudibility = candidateAudibility;
            }
        }
        if (victim != voices.end()) {
            // Invalidate the stolen sound's handle before the slot is reused.
            ++victim->generation;
            ++stolenVoices;
            stolen = true;
        }
        return victim;
    }

    // Resets a voice acquired by acquireVoice for a new sound.
    void startVoice(Voice& voice, AudioPriority priority) {
        const uint32_t generation = voice.generation;
        voice = {};
        voice.generation = generation;
        voice.active = true;
        voice.priority = priority;
        voice.startSequence = ++nextStartSequence;
    }
};

namespace {
void mixImpl(AudioSystem::Impl& impl, float* output, size_t frameCount);

void SDLCALL fillAudio(void* userdata, SDL_AudioStream* stream, int additionalAmount, int) {
    auto& impl = *static_cast<AudioSystem::Impl*>(userdata);
    size_t remaining = static_cast<size_t>(std::max(additionalAmount, 0)) / (sizeof(float) * 2);
    while (remaining > 0) {
        const size_t frames = std::min(remaining, impl.callbackScratch.size() / 2);
        mixImpl(impl, impl.callbackScratch.data(), frames);
        SDL_PutAudioStreamData(stream, impl.callbackScratch.data(), static_cast<int>(frames * 2 * sizeof(float)));
        remaining -= frames;
    }
}
} // namespace

AudioSystem::AudioSystem()
    : AudioSystem(nullptr) {}

AudioSystem::AudioSystem(Logger* logger)
    : m_impl(std::make_unique<Impl>())
    , m_log(logger, "audio") {}

AudioSystem::~AudioSystem() {
    shutdown();
}

AudioResult AudioSystem::init(const AudioConfig& config) {
    shutdown();
    if (config.sampleRate == 0 || config.outputChannels != 2 || config.bufferFrames == 0 || config.maxVoices == 0 ||
        config.streamingBufferFrames == 0) {
        return {.error = "Audio configuration requires a sample rate, stereo output, and non-zero buffers/voices"};
    }
    m_impl->config = config;
    m_impl->voices.resize(config.maxVoices);
    m_impl->callbackScratch.resize(static_cast<size_t>(config.bufferFrames) * 2);
    m_impl->streamRing.resize(static_cast<size_t>(config.streamingBufferFrames) * 2);
    m_impl->initialized = true;
    if (!config.outputEnabled) {
        return {.success = true};
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        m_log.warning(std::string("Audio output disabled: ") + SDL_GetError());
        return {.success = true};
    }
    m_impl->audioSubsystem = true;
    SDL_AudioSpec specification{SDL_AUDIO_F32, 2, static_cast<int>(config.sampleRate)};
    m_impl->output =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &specification, fillAudio, m_impl.get());
    if (!m_impl->output) {
        m_log.warning(std::string("Audio output disabled: ") + SDL_GetError());
        return {.success = true};
    }
    SDL_ResumeAudioStreamDevice(m_impl->output);
    return {.success = true};
}

void AudioSystem::shutdown() {
    if (!m_impl->initialized) {
        return;
    }
    m_impl->decoderStop = true;
    m_impl->streamWake.notify_all();
    if (m_impl->decoderThread.joinable()) {
        m_impl->decoderThread.join();
    }
    if (m_impl->output) {
        SDL_DestroyAudioStream(m_impl->output);
        m_impl->output = nullptr;
    }
    if (m_impl->audioSubsystem) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        m_impl->audioSubsystem = false;
    }
    std::scoped_lock lock(m_impl->mutex);
    m_impl->clips.clear();
    m_impl->streams.clear();
    m_impl->voices.clear();
    m_impl->clipCache.clear();
    m_impl->streamCache.clear();
    m_impl->emitters.clear();
    m_impl->stolenVoices = 0;
    m_impl->voiceLimit = {};
    m_impl->initialized = false;
}

AudioLoadResult<AudioClipHandle> AudioSystem::loadClip(const std::filesystem::path& path, bool spatial) {
    if (!m_impl->initialized) {
        return {.error = "Audio system is not initialized"};
    }
    const auto resolved = canonicalPath(path);
    const std::string key = resolved.generic_string() + (spatial ? "#mono" : "#native");
    {
        std::scoped_lock lock(m_impl->mutex);
        if (const auto found = m_impl->clipCache.find(key); found != m_impl->clipCache.end()) {
            if (m_impl->valid(found->second)) {
                return {.success = true, .handle = found->second};
            }
            m_impl->clipCache.erase(found);
        }
    }
    DecodedAudio decoded = decode(resolved);
    if (!decoded.error.empty()) {
        return {.error = std::move(decoded.error)};
    }
    if (spatial && decoded.channels == 2) {
        std::vector<float> mono(decoded.samples.size() / 2);
        for (size_t i = 0; i < mono.size(); ++i) {
            mono[i] = (decoded.samples[i * 2] + decoded.samples[i * 2 + 1]) * 0.5f;
        }
        decoded.samples = std::move(mono);
        decoded.channels = 1;
    }
    std::scoped_lock lock(m_impl->mutex);
    auto slot =
        std::find_if(m_impl->clips.begin(), m_impl->clips.end(), [](const Impl::Clip& clip) { return !clip.occupied; });
    if (slot == m_impl->clips.end()) {
        m_impl->clips.emplace_back();
        slot = std::prev(m_impl->clips.end());
    }
    const uint32_t generation = slot->generation;
    *slot = {resolved, std::move(decoded.samples), decoded.channels, decoded.sampleRate, generation, true};
    const uint32_t index = static_cast<uint32_t>(std::distance(m_impl->clips.begin(), slot));
    const AudioClipHandle handle{index, generation};
    m_impl->clipCache.emplace(key, handle);
    return {.success = true, .handle = handle};
}

AudioLoadResult<AudioClipHandle> AudioSystem::createClip(std::span<const float> samples, uint32_t channels,
                                                         uint32_t sampleRate) {
    if (!m_impl->initialized) {
        return {.error = "Audio system is not initialized"};
    }
    if ((channels != 1 && channels != 2) || sampleRate == 0 || samples.empty() || samples.size() % channels != 0) {
        return {.error = "Audio clip PCM must be non-empty mono/stereo data with a valid sample rate"};
    }
    if (!std::all_of(samples.begin(), samples.end(), [](float sample) { return std::isfinite(sample); })) {
        return {.error = "Audio clip PCM contains a non-finite sample"};
    }
    std::scoped_lock lock(m_impl->mutex);
    auto slot =
        std::find_if(m_impl->clips.begin(), m_impl->clips.end(), [](const Impl::Clip& clip) { return !clip.occupied; });
    if (slot == m_impl->clips.end()) {
        m_impl->clips.emplace_back();
        slot = std::prev(m_impl->clips.end());
    }
    const uint32_t generation = slot->generation;
    *slot = {{}, std::vector<float>(samples.begin(), samples.end()), channels, sampleRate, generation, true};
    const uint32_t index = static_cast<uint32_t>(std::distance(m_impl->clips.begin(), slot));
    return {.success = true, .handle = {index, generation}};
}

AudioLoadResult<AudioStreamHandle> AudioSystem::loadStream(const std::filesystem::path& path) {
    if (!m_impl->initialized) {
        return {.error = "Audio system is not initialized"};
    }
    const auto resolved = canonicalPath(path);
    const std::string key = resolved.generic_string();
    std::scoped_lock lock(m_impl->mutex);
    if (const auto found = m_impl->streamCache.find(key); found != m_impl->streamCache.end()) {
        if (m_impl->valid(found->second)) {
            return {.success = true, .handle = found->second};
        }
        m_impl->streamCache.erase(found);
    }
    int error = 0;
    stb_vorbis* decoder = stb_vorbis_open_filename(resolved.string().c_str(), &error, nullptr);
    if (!decoder) {
        return {.error = "Cannot open Ogg Vorbis stream: " + resolved.string()};
    }
    const stb_vorbis_info info = stb_vorbis_get_info(decoder);
    stb_vorbis_close(decoder);
    if (info.channels != 1 && info.channels != 2) {
        return {.error = "Unsupported Ogg stream channel layout: " + resolved.string()};
    }
    auto slot = std::find_if(m_impl->streams.begin(), m_impl->streams.end(),
                             [](const Impl::Stream& stream) { return !stream.occupied; });
    if (slot == m_impl->streams.end()) {
        m_impl->streams.emplace_back();
        slot = std::prev(m_impl->streams.end());
    }
    const uint32_t generation = slot->generation;
    *slot = {resolved, static_cast<uint32_t>(info.channels), info.sample_rate, generation, true};
    const uint32_t index = static_cast<uint32_t>(std::distance(m_impl->streams.begin(), slot));
    const AudioStreamHandle handle{index, generation};
    m_impl->streamCache.emplace(key, handle);
    return {.success = true, .handle = handle};
}

AudioLoadResult<AudioVoiceHandle> AudioSystem::play(AudioClipHandle clip, const AudioPlaybackOptions& options) {
    AudioLoadResult<AudioVoiceHandle> result;
    bool overflowStarted = false;
    {
        std::scoped_lock lock(m_impl->mutex);
        if (!m_impl->valid(clip)) {
            return {.error = "Cannot play an invalid audio clip handle"};
        }
        bool stolen = false;
        const auto found = m_impl->acquireVoice(options.priority, stolen);
        overflowStarted = m_impl->voiceLimit.update(stolen || found == m_impl->voices.end());
        if (found == m_impl->voices.end()) {
            result.error = "Audio voice limit reached and every voice plays a sound of higher priority";
        } else {
            m_impl->startVoice(*found, options.priority);
            found->assetIndex = clip.index;
            found->scope = options.scope;
            found->bus = options.bus;
            found->gain = std::max(options.gain, 0.0f);
            found->pitch = std::max(options.pitch, 0.01f);
            found->loop = options.loop;
            const uint32_t index = static_cast<uint32_t>(std::distance(m_impl->voices.begin(), found));
            result = {.success = true, .handle = {index, found->generation}};
        }
    }
    // Logged outside the mutex, which the mixer must not wait for.
    if (overflowStarted) {
        m_log.warning("All " + std::to_string(m_impl->config.maxVoices) +
                      " audio voices are busy; new sounds take the voices of lower-priority sounds");
    }
    return result;
}

AudioLoadResult<AudioVoiceHandle> AudioSystem::playAt(AudioClipHandle clip, const AudioSpatialOptions& options) {
    const AudioPlaybackOptions base = options;
    auto result = play(clip, base);
    if (!result) {
        return result;
    }
    std::scoped_lock lock(m_impl->mutex);
    auto& voice = m_impl->voices[result.handle.index];
    voice.spatial = true;
    voice.position = options.position;
    voice.minimumDistance = options.minimumDistance;
    voice.maximumDistance = options.maximumDistance;
    voice.follow = options.follow;
    voice.followOffset = options.position;
    voice.occlusion = options.occlusion;
    return result;
}

AudioLoadResult<AudioVoiceHandle> AudioSystem::playMusic(AudioStreamHandle stream,
                                                         const AudioPlaybackOptions& options) {
    std::unique_lock lock(m_impl->mutex);
    if (!m_impl->valid(stream)) {
        return {.error = "Cannot play an invalid audio stream handle"};
    }
    if (m_impl->streamingVoice && m_impl->valid(m_impl->streamingVoice)) {
        return {.error = "Only one streamed music voice is supported"};
    }
    if (m_impl->decoderThread.joinable()) {
        m_impl->decoderStop = true;
        m_impl->streamWake.notify_all();
        lock.unlock();
        m_impl->decoderThread.join();
        lock.lock();
    }
    bool stolen = false;
    const auto found = m_impl->acquireVoice(options.priority, stolen);
    if (found == m_impl->voices.end()) {
        return {.error = "Audio voice limit reached and every voice plays a sound of higher priority"};
    }
    m_impl->startVoice(*found, options.priority);
    const uint32_t generation = found->generation;
    found->streaming = true;
    found->assetIndex = stream.index;
    found->scope = options.scope;
    found->bus = options.bus;
    found->gain = std::max(options.gain, 0.0f);
    found->loop = options.loop;
    const uint32_t index = static_cast<uint32_t>(std::distance(m_impl->voices.begin(), found));
    const AudioVoiceHandle voice{index, generation};
    m_impl->streamingVoice = voice;
    m_impl->decoderStop = false;
    m_impl->streamEof = false;
    m_impl->streamRead.store(0, std::memory_order_relaxed);
    m_impl->streamWrite.store(0, std::memory_order_relaxed);
    const auto path = m_impl->streams[stream.index].path;
    const uint32_t channels = m_impl->streams[stream.index].channels;
    const bool shouldLoop = options.loop;
    m_impl->decoderThread = std::thread([impl = m_impl.get(), path, channels, shouldLoop] {
        int error = 0;
        stb_vorbis* decoder = stb_vorbis_open_filename(path.string().c_str(), &error, nullptr);
        if (!decoder) {
            return;
        }
        std::array<float, 2048> source{};
        size_t refillThreshold = std::min(source.size(), impl->streamRing.size());
        refillThreshold -= refillThreshold % channels;
        refillThreshold = std::max(refillThreshold, static_cast<size_t>(channels));
        while (!impl->decoderStop) {
            uint64_t read = impl->streamRead.load(std::memory_order_acquire);
            uint64_t write = impl->streamWrite.load(std::memory_order_relaxed);
            size_t free = impl->streamRing.size() - static_cast<size_t>(write - read);
            if (free < refillThreshold) {
                std::unique_lock waitLock(impl->streamWaitMutex);
                impl->streamWake.wait(waitLock, [&] {
                    if (impl->decoderStop) {
                        return true;
                    }
                    const uint64_t currentRead = impl->streamRead.load(std::memory_order_acquire);
                    const uint64_t currentWrite = impl->streamWrite.load(std::memory_order_relaxed);
                    return impl->streamRing.size() - static_cast<size_t>(currentWrite - currentRead) >= refillThreshold;
                });
                if (impl->decoderStop) {
                    break;
                }
                read = impl->streamRead.load(std::memory_order_acquire);
                write = impl->streamWrite.load(std::memory_order_relaxed);
                free = impl->streamRing.size() - static_cast<size_t>(write - read);
            }
            size_t requestedSamples = std::min(source.size(), free);
            requestedSamples -= requestedSamples % channels;
            if (requestedSamples == 0) {
                continue;
            }
            const int frames = stb_vorbis_get_samples_float_interleaved(
                decoder, static_cast<int>(channels), source.data(), static_cast<int>(requestedSamples));
            if (frames == 0) {
                if (shouldLoop) {
                    stb_vorbis_seek_start(decoder);
                    continue;
                }
                break;
            }
            const size_t count = static_cast<size_t>(frames) * channels;
            for (size_t i = 0; i < count; ++i) {
                impl->streamRing[(write + i) % impl->streamRing.size()] = source[i];
            }
            impl->streamWrite.store(write + count, std::memory_order_release);
        }
        stb_vorbis_close(decoder);
        impl->streamEof = true;
    });
    return {.success = true, .handle = voice};
}

AudioResult AudioSystem::stop(AudioVoiceHandle voice) {
    std::scoped_lock lock(m_impl->mutex);
    if (!m_impl->valid(voice)) {
        return {.error = "Cannot stop an invalid audio voice handle"};
    }
    m_impl->voices[voice.index].active = false;
    ++m_impl->voices[voice.index].generation;
    if (voice == m_impl->streamingVoice) {
        m_impl->decoderStop = true;
        m_impl->streamWake.notify_all();
        m_impl->streamingVoice = {};
    }
    return {.success = true};
}

AudioResult AudioSystem::setPaused(AudioVoiceHandle voice, bool paused) {
    std::scoped_lock lock(m_impl->mutex);
    if (!m_impl->valid(voice)) {
        return {.error = "Cannot pause an invalid audio voice handle"};
    }
    m_impl->voices[voice.index].paused = paused;
    return {.success = true};
}

bool AudioSystem::isPlaying(AudioVoiceHandle voice) const {
    std::scoped_lock lock(m_impl->mutex);
    return m_impl->valid(voice) && !m_impl->voices[voice.index].paused;
}

void AudioSystem::stopSceneVoices() {
    std::scoped_lock lock(m_impl->mutex);
    for (auto& voice : m_impl->voices) {
        if (voice.active && voice.scope == AudioScope::Scene) {
            if (voice.streaming) {
                m_impl->decoderStop = true;
                m_impl->streamWake.notify_all();
                m_impl->streamingVoice = {};
            }
            voice.active = false;
            ++voice.generation;
        }
    }
    m_impl->emitters.clear();
}

void AudioSystem::stopAll() {
    std::scoped_lock lock(m_impl->mutex);
    for (auto& voice : m_impl->voices) {
        if (voice.active) {
            voice.active = false;
            ++voice.generation;
        }
    }
    m_impl->decoderStop = true;
    m_impl->streamWake.notify_all();
    m_impl->streamingVoice = {};
    m_impl->emitters.clear();
}

AudioResult AudioSystem::setBusGain(AudioBus bus, float gain) {
    if (!std::isfinite(gain) || gain < 0.0f) {
        return {.error = "Audio bus gain must be finite and non-negative"};
    }
    std::scoped_lock lock(m_impl->mutex);
    m_impl->buses[busIndex(bus)].gain = gain;
    return {.success = true};
}

float AudioSystem::busGain(AudioBus bus) const {
    std::scoped_lock lock(m_impl->mutex);
    return m_impl->buses[busIndex(bus)].gain;
}

void AudioSystem::setBusMuted(AudioBus bus, bool muted) {
    std::scoped_lock lock(m_impl->mutex);
    m_impl->buses[busIndex(bus)].muted = muted;
}

bool AudioSystem::busMuted(AudioBus bus) const {
    std::scoped_lock lock(m_impl->mutex);
    return m_impl->buses[busIndex(bus)].muted;
}

void AudioSystem::syncScene(const Scene& scene) {
    Entity chosen = entt::null;
    auto listeners = scene.registry().view<const AudioListenerComponent, const TransformComponent>();
    for (Entity entity : listeners) {
        const bool primary = listeners.get<const AudioListenerComponent>(entity).primary;
        if (chosen == entt::null || (primary && !listeners.get<const AudioListenerComponent>(chosen).primary) ||
            (primary == listeners.get<const AudioListenerComponent>(chosen).primary &&
             entt::to_integral(entity) < entt::to_integral(chosen))) {
            chosen = entity;
        }
    }
    if (chosen != entt::null) {
        const glm::mat4 world = listeners.get<const TransformComponent>(chosen).world;
        const glm::vec3 position = glm::vec3(world[3]);
        const glm::vec3 rightAxis = glm::vec3(world[0]);
        const glm::vec3 right =
            glm::dot(rightAxis, rightAxis) > 0.000001f ? glm::normalize(rightAxis) : glm::vec3(1.0f, 0.0f, 0.0f);
        m_impl->listener.positionX.store(position.x, std::memory_order_relaxed);
        m_impl->listener.positionY.store(position.y, std::memory_order_relaxed);
        m_impl->listener.positionZ.store(position.z, std::memory_order_relaxed);
        m_impl->listener.rightX.store(right.x, std::memory_order_relaxed);
        m_impl->listener.rightY.store(right.y, std::memory_order_relaxed);
        m_impl->listener.rightZ.store(right.z, std::memory_order_relaxed);
        m_impl->listener.valid.store(true, std::memory_order_release);
    } else {
        m_impl->listener.valid.store(false, std::memory_order_release);
    }

    followEntities(scene);
    syncEmitters(scene);
    updateOcclusion();
}

void AudioSystem::setOcclusionQuery(AudioOcclusionQuery query) {
    m_impl->occlusionQuery = std::move(query);
}

void AudioSystem::followEntities(const Scene& scene) {
    std::scoped_lock lock(m_impl->mutex);
    for (auto& voice : m_impl->voices) {
        if (!voice.active || !voice.follow) {
            continue;
        }
        const Entity entity = scene.findEntity(voice.follow);
        if (entity == entt::null || !scene.registry().all_of<TransformComponent>(entity)) {
            voice.active = false;
            ++voice.generation;
            continue;
        }
        voice.position = glm::vec3(scene.registry().get<TransformComponent>(entity).world[3]) + voice.followOffset;
    }
}

void AudioSystem::updateOcclusion() {
    if (!m_impl->occlusionQuery || !m_impl->listener.valid.load(std::memory_order_acquire)) {
        return;
    }
    const glm::vec3 listener{m_impl->listener.positionX.load(std::memory_order_relaxed),
                             m_impl->listener.positionY.load(std::memory_order_relaxed),
                             m_impl->listener.positionZ.load(std::memory_order_relaxed)};

    // The queries run outside the mutex, so the mixer never waits on them.
    struct Pending {
        AudioVoiceHandle voice;
        glm::vec3 position;
        float occlusion{0.0f};
    };

    std::vector<Pending> pending;
    {
        std::scoped_lock lock(m_impl->mutex);
        for (uint32_t index = 0; index < m_impl->voices.size(); ++index) {
            const auto& voice = m_impl->voices[index];
            if (voice.active && voice.spatial && voice.occlusion) {
                pending.push_back({{index, voice.generation}, voice.position});
            }
        }
    }
    for (Pending& query : pending) {
        query.occlusion = std::clamp(m_impl->occlusionQuery(listener, query.position), 0.0f, 1.0f);
    }
    std::scoped_lock lock(m_impl->mutex);
    for (const Pending& query : pending) {
        if (m_impl->valid(query.voice)) {
            m_impl->voices[query.voice.index].occlusionTarget = query.occlusion;
        }
    }
}

void AudioSystem::syncEmitters(const Scene& scene) {
    auto emitters = scene.registry().view<const AudioEmitterComponent, const TransformComponent>();
    if (emitters.begin() == emitters.end() && m_impl->emitters.empty()) {
        return;
    }

    std::scoped_lock lock(m_impl->mutex);
    const uint64_t sync = ++m_impl->emitterSync;
    for (Entity entity : emitters) {
        const uint32_t id = entt::to_integral(entity);
        const auto& component = emitters.get<const AudioEmitterComponent>(entity);
        auto runtime = m_impl->emitters.find(id);
        if (!component.play || !m_impl->valid(component.clip)) {
            if (runtime != m_impl->emitters.end() && m_impl->valid(runtime->second.voice)) {
                auto& voice = m_impl->voices[runtime->second.voice.index];
                voice.active = false;
                ++voice.generation;
            }
            m_impl->emitters.erase(id);
            continue;
        }
        if (runtime == m_impl->emitters.end() || runtime->second.clip != component.clip ||
            !m_impl->valid(runtime->second.voice)) {
            if (runtime != m_impl->emitters.end()) {
                if (m_impl->valid(runtime->second.voice)) {
                    auto& previousVoice = m_impl->voices[runtime->second.voice.index];
                    previousVoice.active = false;
                    ++previousVoice.generation;
                }
                m_impl->emitters.erase(runtime);
            }
            auto slot = std::find_if(m_impl->voices.begin(), m_impl->voices.end(),
                                     [](const Impl::Voice& voice) { return !voice.active; });
            if (slot == m_impl->voices.end()) {
                continue;
            }
            m_impl->startVoice(*slot, AudioPriority::Normal);
            const uint32_t generation = slot->generation;
            slot->spatial = true;
            slot->scope = AudioScope::Scene;
            slot->assetIndex = component.clip.index;
            const uint32_t index = static_cast<uint32_t>(std::distance(m_impl->voices.begin(), slot));
            runtime =
                m_impl->emitters.emplace(id, Impl::EmitterRuntime{{index, generation}, component.clip, sync}).first;
        }
        runtime->second.lastSeenSync = sync;
        auto& voice = m_impl->voices[runtime->second.voice.index];
        voice.position = glm::vec3(emitters.get<const TransformComponent>(entity).world[3]);
        voice.bus = component.bus;
        voice.gain = std::max(component.gain, 0.0f);
        voice.pitch = std::max(component.pitch, 0.01f);
        voice.minimumDistance = component.minimumDistance;
        voice.maximumDistance = component.maximumDistance;
        voice.loop = component.loop;
        voice.occlusion = component.occlusion;
    }
    for (auto it = m_impl->emitters.begin(); it != m_impl->emitters.end();) {
        if (it->second.lastSeenSync != sync) {
            if (m_impl->valid(it->second.voice)) {
                auto& voice = m_impl->voices[it->second.voice.index];
                voice.active = false;
                ++voice.generation;
            }
            it = m_impl->emitters.erase(it);
        } else {
            ++it;
        }
    }
}

void AudioSystem::setPlaybackRate(float rate) {
    if (std::isnan(rate)) {
        return;
    }
    std::scoped_lock lock(m_impl->mutex);
    m_impl->playbackRate = std::clamp(rate, 0.0f, 10.0f);
}

float AudioSystem::playbackRate() const {
    std::scoped_lock lock(m_impl->mutex);
    return m_impl->playbackRate;
}

void AudioSystem::update(float deltaSeconds) {
    if (m_impl->output || !m_impl->initialized || !std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f) {
        return;
    }
    size_t remaining = static_cast<size_t>(deltaSeconds * static_cast<float>(m_impl->config.sampleRate));
    while (remaining > 0) {
        const size_t frames = std::min(remaining, m_impl->callbackScratch.size() / 2);
        mix(m_impl->callbackScratch.data(), frames);
        remaining -= frames;
    }
}

AudioAssetCollectionResult AudioSystem::collectUnusedAssets(bool force) {
    std::scoped_lock lock(m_impl->mutex);
    AudioAssetCollectionResult result;
    for (const Impl::Clip& clip : m_impl->clips) {
        if (clip.occupied) {
            result.bytesRetained += clip.samples.size() * sizeof(float);
        }
    }

    std::vector<bool> liveClips(m_impl->clips.size(), false);
    std::vector<bool> liveStreams(m_impl->streams.size(), false);
    for (const Impl::Voice& voice : m_impl->voices) {
        if (!voice.active) {
            continue;
        }
        if (voice.streaming) {
            if (voice.assetIndex < liveStreams.size()) {
                liveStreams[voice.assetIndex] = true;
            }
        } else if (voice.assetIndex < liveClips.size()) {
            liveClips[voice.assetIndex] = true;
        }
    }

    for (uint32_t index = 0; index < m_impl->clips.size(); ++index) {
        Impl::Clip& clip = m_impl->clips[index];
        if (!clip.occupied || liveClips[index] ||
            (!force && result.bytesRetained <= m_impl->config.decodedAudioBudgetBytes)) {
            continue;
        }
        const uint64_t bytes = clip.samples.size() * sizeof(float);
        const uint32_t generation = clip.generation;
        std::erase_if(m_impl->clipCache, [index, generation](const auto& entry) {
            return entry.second.index == index && entry.second.generation == generation;
        });
        clip = {};
        clip.occupied = false;
        clip.generation = generation + 1;
        if (clip.generation == 0) {
            clip.generation = 1;
        }
        result.bytesRetained -= bytes;
        result.bytesFreed += bytes;
        ++result.clipsEvicted;
    }

    if (force) {
        for (uint32_t index = 0; index < m_impl->streams.size(); ++index) {
            Impl::Stream& stream = m_impl->streams[index];
            if (!stream.occupied || liveStreams[index]) {
                continue;
            }
            const uint32_t generation = stream.generation;
            std::erase_if(m_impl->streamCache, [index, generation](const auto& entry) {
                return entry.second.index == index && entry.second.generation == generation;
            });
            stream = {};
            stream.occupied = false;
            stream.generation = generation + 1;
            if (stream.generation == 0) {
                stream.generation = 1;
            }
            ++result.streamsEvicted;
        }
    }
    result.budgetSatisfied = result.bytesRetained <= m_impl->config.decodedAudioBudgetBytes;
    return result;
}

uint64_t AudioSystem::stolenVoiceCount() const {
    std::scoped_lock lock(m_impl->mutex);
    return m_impl->stolenVoices;
}

uint64_t AudioSystem::decodedAudioBytes() const {
    std::scoped_lock lock(m_impl->mutex);
    uint64_t bytes = 0;
    for (const Impl::Clip& clip : m_impl->clips) {
        if (clip.occupied) {
            bytes += clip.samples.size() * sizeof(float);
        }
    }
    return bytes;
}

std::vector<AudioAssetDiagnostic> AudioSystem::assetDiagnostics() const {
    std::scoped_lock lock(m_impl->mutex);
    std::vector<uint32_t> clipReferences(m_impl->clips.size(), 0);
    std::vector<uint32_t> streamReferences(m_impl->streams.size(), 0);
    for (const Impl::Voice& voice : m_impl->voices) {
        if (!voice.active) {
            continue;
        }
        if (voice.streaming && voice.assetIndex < streamReferences.size()) {
            ++streamReferences[voice.assetIndex];
        } else if (!voice.streaming && voice.assetIndex < clipReferences.size()) {
            ++clipReferences[voice.assetIndex];
        }
    }
    std::vector<AudioAssetDiagnostic> result;
    for (uint32_t index = 0; index < m_impl->clips.size(); ++index) {
        const Impl::Clip& clip = m_impl->clips[index];
        if (clip.occupied) {
            result.push_back({false, index, clip.generation, clipReferences[index], clip.samples.size() * sizeof(float),
                              clip.path.generic_string()});
        }
    }
    for (uint32_t index = 0; index < m_impl->streams.size(); ++index) {
        const Impl::Stream& stream = m_impl->streams[index];
        if (stream.occupied) {
            result.push_back(
                {true, index, stream.generation, streamReferences[index], 0, stream.path.generic_string()});
        }
    }
    return result;
}

bool AudioSystem::hasOutputDevice() const {
    return m_impl->output != nullptr;
}

const AudioConfig& AudioSystem::config() const {
    return m_impl->config;
}

void AudioSystem::mix(float* output, size_t frameCount) {
    mixImpl(*m_impl, output, frameCount);
}

namespace {
void mixImpl(AudioSystem::Impl& impl, float* output, size_t frameCount) {
    std::fill(output, output + frameCount * 2, 0.0f);
    std::unique_lock lock(impl.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return;
    }
    const auto master = impl.buses[busIndex(AudioBus::Master)];
    if (master.muted) {
        return;
    }
    for (auto& voice : impl.voices) {
        // The simulation's rate holds or speeds up the scene's voices only.
        const float rate = voice.scope == AudioScope::Scene ? impl.playbackRate : 1.0f;
        if (!voice.active || voice.paused || rate <= 0.0f) {
            continue;
        }
        const auto child =
            voice.bus == AudioBus::Master ? AudioSystem::Impl::BusState{} : impl.buses[busIndex(voice.bus)];
        const float baseGain = voice.gain * child.gain * master.gain * (child.muted ? 0.0f : 1.0f);
        float leftGain = baseGain;
        float rightGain = baseGain;
        if (voice.spatial) {
            if (!impl.listener.valid.load(std::memory_order_acquire)) {
                leftGain = rightGain = 0.0f;
            } else {
                const glm::vec3 listenerPosition{
                    impl.listener.positionX.load(std::memory_order_relaxed),
                    impl.listener.positionY.load(std::memory_order_relaxed),
                    impl.listener.positionZ.load(std::memory_order_relaxed),
                };
                const glm::vec3 listenerRight{
                    impl.listener.rightX.load(std::memory_order_relaxed),
                    impl.listener.rightY.load(std::memory_order_relaxed),
                    impl.listener.rightZ.load(std::memory_order_relaxed),
                };
                const glm::vec3 offset = voice.position - listenerPosition;
                const float distance = glm::length(offset);
                const float pan =
                    distance > 0.0001f ? glm::clamp(glm::dot(offset / distance, listenerRight), -1.0f, 1.0f) : 0.0f;
                const float minimum = std::max(voice.minimumDistance, 0.001f);
                const float maximum = std::max(voice.maximumDistance, minimum);
                const float attenuation =
                    distance >= maximum ? 0.0f : std::min(1.0f, minimum / std::max(distance, minimum));
                constexpr float halfPi = 1.57079632679f;
                leftGain *= std::cos((pan + 1.0f) * halfPi * 0.5f) * attenuation;
                rightGain *= std::sin((pan + 1.0f) * halfPi * 0.5f) * attenuation;
            }
        }
        if (voice.streaming) {
            const auto& stream = impl.streams[voice.assetIndex];
            const uint32_t channels = stream.channels;
            const double step = static_cast<double>(stream.sampleRate) / impl.config.sampleRate * voice.pitch * rate;
            uint64_t read = impl.streamRead.load(std::memory_order_relaxed);
            const uint64_t write = impl.streamWrite.load(std::memory_order_acquire);
            size_t available = static_cast<size_t>(write - read);
            const size_t availableBeforeMix = available;
            for (size_t frame = 0; frame < frameCount; ++frame) {
                const size_t sourceOffset = static_cast<size_t>(voice.cursor);
                if (available < (sourceOffset + 1) * channels) {
                    break;
                }
                size_t sampleIndex = (read + sourceOffset * channels) % impl.streamRing.size();
                const float left = impl.streamRing[sampleIndex];
                float right = left;
                if (channels == 2) {
                    sampleIndex = (sampleIndex + 1) % impl.streamRing.size();
                    right = impl.streamRing[sampleIndex];
                }
                output[frame * 2] += left * leftGain;
                output[frame * 2 + 1] += right * rightGain;
                voice.cursor += step;
                const size_t advanceFrames = std::min(static_cast<size_t>(voice.cursor), available / channels);
                read += advanceFrames * channels;
                available -= advanceFrames * channels;
                voice.cursor -= static_cast<double>(advanceFrames);
            }
            impl.streamRead.store(read, std::memory_order_release);
            const size_t refillThreshold = std::min<size_t>(2048, impl.streamRing.size());
            const size_t freeBeforeMix = impl.streamRing.size() - availableBeforeMix;
            const size_t freeAfterMix = impl.streamRing.size() - available;
            if (freeBeforeMix < refillThreshold && freeAfterMix >= refillThreshold) {
                impl.streamWake.notify_one();
            }
            if (impl.streamEof && available == 0) {
                voice.active = false;
                ++voice.generation;
                impl.streamingVoice = {};
            }
            continue;
        }
        const auto& clip = impl.clips[voice.assetIndex];
        const size_t sourceFrames = clip.samples.size() / clip.channels;
        const double step = static_cast<double>(clip.sampleRate) / impl.config.sampleRate * voice.pitch * rate;
        // Occlusion glides with a 50 ms time constant; fully occluded, a voice
        // keeps occludedGain of its level and a low-pass near 1 kHz.
        constexpr float occludedGain = 0.35f;
        constexpr float occludedLowPass = 0.12f;
        const bool occludable = voice.spatial && (voice.occlusion || voice.occlusionCurrent > 0.0f);
        const float glide = 1.0f - std::exp(-1.0f / (0.05f * static_cast<float>(impl.config.sampleRate)));
        for (size_t frame = 0; frame < frameCount; ++frame) {
            size_t sourceFrame = static_cast<size_t>(voice.cursor);
            if (sourceFrame >= sourceFrames) {
                if (!voice.loop) {
                    voice.active = false;
                    ++voice.generation;
                    break;
                }
                voice.cursor = std::fmod(voice.cursor, static_cast<double>(sourceFrames));
                sourceFrame = static_cast<size_t>(voice.cursor);
            }
            float left = clip.samples[sourceFrame * clip.channels];
            float right = clip.channels == 2 ? clip.samples[sourceFrame * 2 + 1] : left;
            if (occludable) {
                const float target = voice.occlusion ? voice.occlusionTarget : 0.0f;
                voice.occlusionCurrent += (target - voice.occlusionCurrent) * glide;
                const float smoothing = 1.0f + (occludedLowPass - 1.0f) * voice.occlusionCurrent;
                voice.lowPassLeft += (left - voice.lowPassLeft) * smoothing;
                voice.lowPassRight += (right - voice.lowPassRight) * smoothing;
                const float gain = 1.0f + (occludedGain - 1.0f) * voice.occlusionCurrent;
                left = voice.lowPassLeft * gain;
                right = voice.lowPassRight * gain;
            }
            output[frame * 2] += left * leftGain;
            output[frame * 2 + 1] += right * rightGain;
            voice.cursor += step;
        }
    }
    for (size_t i = 0; i < frameCount * 2; ++i) {
        output[i] = std::clamp(output[i], -1.0f, 1.0f);
    }
}
} // namespace
