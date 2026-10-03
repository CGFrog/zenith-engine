#include "engine/audio/AudioSystem.hpp"
#include "engine/scene/Scene.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void write16(std::ofstream& output, uint16_t value) {
    output.put(static_cast<char>(value & 0xff));
    output.put(static_cast<char>(value >> 8));
}

void write32(std::ofstream& output, uint32_t value) {
    write16(output, static_cast<uint16_t>(value & 0xffff));
    write16(output, static_cast<uint16_t>(value >> 16));
}

void writeWav(const std::filesystem::path& path, uint16_t channels, const std::vector<int16_t>& samples) {
    std::ofstream output(path, std::ios::binary);
    output.write("RIFF", 4);
    write32(output, 36 + static_cast<uint32_t>(samples.size() * sizeof(int16_t)));
    output.write("WAVEfmt ", 8);
    write32(output, 16);
    write16(output, 1);
    write16(output, channels);
    write32(output, 48'000);
    write32(output, 48'000 * channels * sizeof(int16_t));
    write16(output, channels * sizeof(int16_t));
    write16(output, 16);
    output.write("data", 4);
    write32(output, static_cast<uint32_t>(samples.size() * sizeof(int16_t)));
    output.write(reinterpret_cast<const char*>(samples.data()),
                 static_cast<std::streamsize>(samples.size() * sizeof(int16_t)));
}

bool near(float lhs, float rhs, float tolerance = 0.001f) {
    return std::abs(lhs - rhs) <= tolerance;
}
} // namespace

int main() {
    const auto directory = std::filesystem::temp_directory_path() / "zenith_audio_tests";
    std::filesystem::create_directories(directory);
    const auto monoPath = directory / "mono.wav";
    const auto stereoPath = directory / "stereo.wav";
    const auto malformedPath = directory / "malformed.wav";
    const auto unsupportedPath = directory / "three-channel.wav";
    writeWav(monoPath, 1, {16'384, -16'384, 8'192, -8'192});
    writeWav(stereoPath, 2, {16'384, 8'192, -16'384, -8'192});
    writeWav(unsupportedPath, 3, {1, 2, 3});
    std::ofstream(malformedPath, std::ios::binary).write("bad", 3);

    AudioSystem audio;
    expect(static_cast<bool>(audio.init(AudioConfig{.maxVoices = 2, .outputEnabled = false})),
           "audio initializes without an output device");
    expect(!audio.hasOutputDevice(), "disabled output uses null-output mode");
    const auto mono = audio.loadClip(monoPath, true);
    const auto cached = audio.loadClip(directory / "." / "mono.wav", true);
    expect(mono && cached && mono.handle == cached.handle, "canonical paths share the clip cache");
    expect(!audio.loadClip(malformedPath), "malformed WAV files are rejected");
    expect(!audio.loadClip(unsupportedPath), "unsupported channel layouts are rejected");
    expect(!audio.loadClip(directory / "missing.wav"), "missing WAV files are rejected");
    const auto oggPath = std::filesystem::path{ZENITH_ASSET_DIR} / "audio" / "ambient.ogg";
    const auto oggClip = audio.loadClip(oggPath);
    expect(oggClip && oggClip.handle == audio.loadClip(oggPath).handle, "Ogg effects decode and share the path cache");
    expect(!audio.play(AudioClipHandle{}), "invalid clip handles are rejected");

    auto first = audio.play(mono.handle, {.gain = 0.5f, .loop = true});
    auto second = audio.play(mono.handle, {.gain = 1.0f, .loop = true});
    expect(first && second, "voices play up to the configured limit");
    expect(!audio.play(mono.handle), "playback reports voice-limit exhaustion");
    std::vector<float> mixed(8);
    audio.mix(mixed.data(), 4);
    expect(near(mixed[0], 0.75f) && near(mixed[1], 0.75f), "2D voices mix into stereo and sum gains");
    expect(static_cast<bool>(audio.setPaused(first.handle, true)), "voice pause succeeds");
    audio.mix(mixed.data(), 1);
    expect(near(mixed[0], 0.5f), "paused voices do not advance or mix");
    audio.setBusGain(AudioBus::Sfx, 0.5f);
    audio.setBusGain(AudioBus::Master, 0.5f);
    audio.mix(mixed.data(), 1);
    expect(near(mixed[0], -0.125f), "master gain applies after child-bus gain");
    audio.setBusMuted(AudioBus::Sfx, true);
    audio.mix(mixed.data(), 1);
    expect(near(mixed[0], 0.0f) && near(mixed[1], 0.0f), "bus mute produces silence");
    audio.setBusMuted(AudioBus::Sfx, false);
    expect(static_cast<bool>(audio.stop(first.handle)), "voice stop succeeds");
    expect(!audio.stop(first.handle), "stale voice generations are rejected");
    audio.stop(second.handle);

    auto scoped = audio.play(mono.handle, {.scope = AudioScope::Scene, .loop = true});
    auto persistent = audio.play(mono.handle, {.scope = AudioScope::Persistent, .loop = true});
    audio.stopSceneVoices();
    expect(!audio.isPlaying(scoped.handle), "scene teardown stops scene-scoped voices");
    expect(audio.isPlaying(persistent.handle), "scene teardown preserves persistent voices");
    audio.stop(persistent.handle);

    audio.setBusGain(AudioBus::Sfx, 1.0f);
    audio.setBusGain(AudioBus::Master, 1.0f);
    auto pitched = audio.play(mono.handle, {.pitch = 2.0f});
    audio.mix(mixed.data(), 2);
    expect(near(mixed[0], 0.5f) && near(mixed[2], 0.25f), "voice pitch controls source-frame advancement");
    audio.stop(pitched.handle);
    auto loudA = audio.play(mono.handle, {.gain = 2.0f});
    auto loudB = audio.play(mono.handle, {.gain = 2.0f});
    audio.mix(mixed.data(), 1);
    expect(near(mixed[0], 1.0f) && near(mixed[1], 1.0f), "mixed output saturates to the float output range");
    audio.stop(loudA.handle);
    audio.stop(loudB.handle);

    Scene scene;
    const Entity listener = scene.createEntity("Listener");
    scene.registry().emplace<AudioListenerComponent>(listener);
    const Entity emitter = scene.createEntity("Emitter");
    scene.registry().get<TransformComponent>(emitter).local[3] = glm::vec4(10.0f, 0.0f, 0.0f, 1.0f);
    scene.registry().emplace<AudioEmitterComponent>(emitter, mono.handle, AudioBus::Sfx, 1.0f, 1.0f, 1.0f, 100.0f, true,
                                                    true);
    audio.setBusGain(AudioBus::Sfx, 1.0f);
    audio.setBusGain(AudioBus::Master, 1.0f);
    scene.updateWorldTransforms();
    audio.syncScene(scene);
    audio.mix(mixed.data(), 1);
    expect(std::abs(mixed[1]) > std::abs(mixed[0]), "right-side emitters pan toward the right channel");
    scene.registry().get<TransformComponent>(listener).local =
        glm::rotate(glm::mat4(1.0f), glm::pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
    scene.markTransformDirty(listener);
    scene.updateWorldTransforms();
    audio.syncScene(scene);
    audio.mix(mixed.data(), 1);
    expect(std::abs(mixed[0]) > std::abs(mixed[1]), "listener rotation changes the stereo pan basis");
    scene.registry().get<AudioEmitterComponent>(emitter).clip = oggClip.handle;
    scene.updateWorldTransforms();
    audio.syncScene(scene);
    auto spareVoice = audio.play(mono.handle);
    expect(static_cast<bool>(spareVoice), "changing an emitter clip replaces rather than leaks its runtime voice");
    audio.stop(spareVoice.handle);
    scene.registry().remove<AudioListenerComponent>(listener);
    scene.updateWorldTransforms();
    audio.syncScene(scene);
    audio.mix(mixed.data(), 1);
    expect(near(mixed[0], 0.0f) && near(mixed[1], 0.0f), "spatial voices are silent without a listener");
    scene.destroyEntity(emitter);
    scene.updateWorldTransforms();
    audio.syncScene(scene);
    audio.stopSceneVoices();

    const auto stream = audio.loadStream(oggPath);
    expect(stream && stream.handle == audio.loadStream(oggPath).handle,
           "Ogg streams validate metadata and share the path cache");
    auto music = audio.playMusic(
        stream.handle,
        {.bus = AudioBus::Music, .scope = AudioScope::Persistent, .gain = 1.0f, .pitch = 1.0f, .loop = true});
    expect(static_cast<bool>(music), "streamed music starts on a decoder worker");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::vector<float> streamed(1024);
    audio.mix(streamed.data(), streamed.size() / 2);
    expect(std::any_of(streamed.begin(), streamed.end(), [](float sample) { return std::abs(sample) > 0.0001f; }),
           "streaming refill recovers from initial underrun and produces samples");
    expect(static_cast<bool>(audio.stop(music.handle)), "streamed music stops explicitly");
    audio.shutdown();

    AudioSystem boundedStream;
    expect(static_cast<bool>(
               boundedStream.init(AudioConfig{.streamingBufferFrames = 64, .maxVoices = 1, .outputEnabled = false})),
           "small bounded streaming buffers initialize");
    const auto boundedAsset = boundedStream.loadStream(oggPath);
    const auto boundedVoice = boundedStream.playMusic(boundedAsset.handle);
    expect(static_cast<bool>(boundedVoice), "streaming supports buffers smaller than a decoder block");
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    // This must join safely even while the decoder is actively refilling.
    boundedStream.shutdown();

    AudioSystem lifetime;
    expect(static_cast<bool>(lifetime.init(AudioConfig{.maxVoices = 1, .outputEnabled = false})),
           "audio lifetime test initializes");
    const std::array<float, 4> lifetimeSamples{0.25f, -0.25f, 0.5f, -0.5f};
    const auto oldClip = lifetime.createClip(lifetimeSamples, 1, 48'000);
    const auto liveVoice = lifetime.play(oldClip.handle, {.loop = true});
    const auto retained = lifetime.collectUnusedAssets(true);
    const auto liveDiagnostics = lifetime.assetDiagnostics();
    expect(retained.clipsEvicted == 0 && lifetime.decodedAudioBytes() == lifetimeSamples.size() * sizeof(float),
           "active voices strongly retain decoded clips during collection");
    expect(liveDiagnostics.size() == 1 && liveDiagnostics.front().generation == oldClip.handle.generation &&
               liveDiagnostics.front().strongReferences == 1,
           "audio diagnostics report generations and active voice ownership");
    lifetime.stop(liveVoice.handle);
    const auto released = lifetime.collectUnusedAssets(true);
    expect(released.clipsEvicted == 1 && released.bytesFreed == lifetimeSamples.size() * sizeof(float) &&
               lifetime.decodedAudioBytes() == 0,
           "stopped voice assets are collected deterministically");
    expect(!lifetime.play(oldClip.handle), "collected audio handles become stale");
    const auto newClip = lifetime.createClip(lifetimeSamples, 1, 48'000);
    expect(newClip.handle.index == oldClip.handle.index && newClip.handle.generation != oldClip.handle.generation,
           "audio slots are recycled with a new generation");
    lifetime.shutdown();

    // The playback rate (the simulation's time scale) holds voices at 0 and
    // plays them faster above 1.
    AudioSystem rated;
    expect(static_cast<bool>(rated.init(AudioConfig{.maxVoices = 2, .outputEnabled = false})),
           "audio playback rate test initializes");
    const std::vector<float> tenthSecond(static_cast<size_t>(rated.config().sampleRate / 10), 0.1f);
    const auto ratedClip = rated.createClip(tenthSecond, 1, rated.config().sampleRate);
    const auto heldVoice = rated.play(ratedClip.handle, {.scope = AudioScope::Persistent});
    rated.setPlaybackRate(0.0f);
    rated.update(1.0f);
    expect(rated.isPlaying(heldVoice.handle), "a zero playback rate holds every voice, persistent ones included");
    rated.setPlaybackRate(2.0f);
    rated.update(0.03f);
    expect(rated.isPlaying(heldVoice.handle), "a held voice resumes where it stopped");
    rated.update(0.03f);
    expect(!rated.isPlaying(heldVoice.handle), "a doubled rate plays a tenth of a second in 0.06 seconds");
    rated.setPlaybackRate(std::numeric_limits<float>::quiet_NaN());
    expect(rated.playbackRate() == 2.0f, "a NaN playback rate is ignored");
    rated.setPlaybackRate(40.0f);
    expect(rated.playbackRate() == 10.0f, "the playback rate is clamped to ten");
    rated.shutdown();

    AudioSystem invalid;
    expect(!invalid.init(AudioConfig{.outputChannels = 1}), "non-stereo output configuration is rejected");
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    return failures == 0 ? 0 : 1;
}
