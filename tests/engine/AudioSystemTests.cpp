#include "engine/assets/Assets.hpp"
#include "engine/audio/AudioSystem.hpp"
#include "engine/graphics/DebugDraw.hpp"
#include "engine/module/BuiltInSystems.hpp"
#include "engine/module/GameServices.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/Scene.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <limits>
#include <random>
#include <string>
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

size_t warningsMentioning(const Logger& logger, std::string_view text) {
    return static_cast<size_t>(std::ranges::count_if(logger.entries(), [text](const LogEntry& entry) {
        return entry.level == LogLevel::Warning && entry.message.find(text) != std::string::npos;
    }));
}

// Gameplay's audio: clips by asset path through GameServices, emitters that
// load their authored clip, and the emitter's document form.
void testGameplayAudio(const std::filesystem::path& assetRoot) {
    std::filesystem::create_directories(assetRoot / "sounds");
    writeWav(assetRoot / "sounds" / "hit.wav", 1, {16'384, -16'384, 8'192, -8'192});
    Logger logger;
    AssetManager assets{&logger};
    DebugDraw debugDraw;
    const SceneDocumentOptions documents{.assetRoot = assetRoot, .componentTypes = builtInComponentTypes()};
    GameServices services(assets, documents, debugDraw, &logger);
    expect(services.audio() == nullptr && !services.loadClip("sounds/hit.wav"),
           "a session without audio loads no clips");

    AudioSystem audio;
    expect(static_cast<bool>(audio.init(AudioConfig{.maxVoices = 4, .outputEnabled = false})),
           "gameplay audio test initializes");
    services.setAudio(&audio);
    const auto loaded = services.loadClip("sounds/hit.wav");
    expect(loaded && loaded.handle == audio.loadClip(assetRoot / "sounds" / "hit.wav", true).handle,
           "a clip path is relative to the session's asset root and shares the audio cache");
    expect(!services.loadClip("../hit.wav") && !services.loadClip((assetRoot / "sounds" / "hit.wav").string()),
           "a clip path outside the project's assets is rejected");

    Scene scene;
    const Entity torch = scene.createEntity("Torch");
    auto& torchEmitter = scene.registry().emplace<AudioEmitterComponent>(torch);
    torchEmitter.clipAsset = "sounds/hit.wav";
    torchEmitter.loop = true;
    const Entity broken = scene.createEntity("Broken");
    scene.registry().emplace<AudioEmitterComponent>(broken).clipAsset = "sounds/missing.wav";
    const SystemScheduler systems(builtInSystems());
    const auto lateUpdate = [&] {
        runGameplayPhase(systems, SystemPhase::LateUpdate,
                         {.scene = scene, .mode = SimulationMode::Play, .logger = &logger, .services = &services});
    };
    lateUpdate();
    lateUpdate();
    expect(scene.registry().get<AudioEmitterComponent>(torch).clip == loaded.handle,
           "an emitter loads its authored clip while gameplay runs");
    expect(!scene.registry().get<AudioEmitterComponent>(broken).clip &&
               warningsMentioning(logger, "sounds/missing.wav") == 1,
           "an emitter whose clip cannot load is reported once");
    scene.registry().get<AudioEmitterComponent>(torch).clipAsset.clear();
    lateUpdate();
    expect(!scene.registry().get<AudioEmitterComponent>(torch).clip, "clearing the clip path stops the emitter");

    scene.registry().get<AudioEmitterComponent>(torch).clipAsset = "sounds/hit.wav";
    scene.registry().emplace<AudioListenerComponent>(scene.createEntity("Ears")).primary = false;
    std::string text;
    expect(SceneDocument::serialize(scene, assets, documents, text).success, "audio components serialize");
    Scene restored;
    expect(SceneDocument::deserialize(text, assets, documents, restored).success, "audio components deserialize");
    const Entity restoredTorch = restored.findEntity(scene.id(torch));
    const auto* emitter =
        restoredTorch != entt::null ? restored.registry().try_get<AudioEmitterComponent>(restoredTorch) : nullptr;
    expect(emitter != nullptr && emitter->clipAsset == "sounds/hit.wav" && emitter->loop && !emitter->clip,
           "an emitter keeps its clip path and settings, and loads its clip only while gameplay runs");
    const auto listeners = restored.registry().view<const AudioListenerComponent>();
    expect(listeners.size() == 1 && !listeners.get<const AudioListenerComponent>(listeners.front()).primary,
           "a listener keeps its settings");
    audio.shutdown();
}

bool near(float lhs, float rhs, float tolerance = 0.001f) {
    return std::abs(lhs - rhs) <= tolerance;
}

// The sum of squares of each channel over a mix of frameCount frames.
glm::vec2 energy(AudioSystem& audio, size_t frameCount) {
    std::vector<float> mixed(frameCount * 2);
    audio.mix(mixed.data(), frameCount);
    glm::vec2 total(0.0f);
    for (size_t frame = 0; frame < frameCount; ++frame) {
        total += glm::vec2(mixed[frame * 2] * mixed[frame * 2], mixed[frame * 2 + 1] * mixed[frame * 2 + 1]);
    }
    return total;
}

// A listener at the origin facing -Z (right is +X).
Entity addListener(Scene& scene) {
    const Entity listener = scene.createEntity("Listener");
    scene.registry().emplace<AudioListenerComponent>(listener);
    return listener;
}

void moveTo(Scene& scene, Entity entity, const glm::vec3& position) {
    scene.registry().get<TransformComponent>(entity).local = glm::translate(glm::mat4(1.0f), position);
    scene.markTransformDirty(entity);
    scene.updateWorldTransforms();
}

void testFollowingVoices() {
    AudioSystem audio;
    audio.init(AudioConfig{.outputEnabled = false});
    const std::vector<float> tone(48'000, 0.5f);
    const AudioClipHandle clip = audio.createClip(tone, 1, 48'000).handle;
    Scene scene;
    addListener(scene);
    const Entity car = scene.createEntity("Car");
    moveTo(scene, car, {5.0f, 0.0f, 0.0f});

    const auto voice = audio.playAt(clip, {.follow = scene.id(car)});
    expect(voice.success, "a voice that follows an entity plays");
    audio.syncScene(scene);
    const glm::vec2 right = energy(audio, 64);
    expect(right.y > right.x * 4.0f, "a following voice starts at its entity");

    moveTo(scene, car, {-5.0f, 0.0f, 0.0f});
    audio.syncScene(scene);
    const glm::vec2 left = energy(audio, 64);
    expect(left.x > left.y * 4.0f, "a following voice moves with its entity");

    scene.destroyEntity(car);
    audio.syncScene(scene);
    expect(!audio.isPlaying(voice.handle), "a following voice stops when its entity is destroyed");

    const Entity drone = scene.createEntity("Drone");
    moveTo(scene, drone, {0.0f, 0.0f, 0.0f});
    const auto offset = audio.playAt(clip, {.position = {3.0f, 0.0f, 0.0f}, .follow = scene.id(drone)});
    audio.syncScene(scene);
    const glm::vec2 offsetEnergy = energy(audio, 64);
    expect(offset.success && offsetEnergy.y > offsetEnergy.x * 4.0f,
           "a following voice keeps its position as an offset from the entity");
}

void testOcclusion() {
    // Alternating samples are all high frequency, which a low-pass removes.
    std::vector<float> buzz(48'000);
    for (size_t index = 0; index < buzz.size(); ++index) {
        buzz[index] = index % 2 == 0 ? 0.5f : -0.5f;
    }
    const auto play = [&](bool occluded, float blocked) {
        AudioSystem audio;
        audio.init(AudioConfig{.outputEnabled = false});
        const AudioClipHandle clip = audio.createClip(buzz, 1, 48'000).handle;
        audio.setOcclusionQuery([blocked](const glm::vec3&, const glm::vec3&) { return blocked; });
        Scene scene;
        addListener(scene);
        audio.playAt(clip, {.position = {0.0f, 0.0f, -2.0f}, .occlusion = occluded});
        audio.syncScene(scene);
        energy(audio, 9'600);
        return energy(audio, 4'800).x;
    };
    const float clear = play(false, 1.0f);
    const float unblocked = play(true, 0.0f);
    const float blocked = play(true, 1.0f);
    expect(clear > 0.0f && std::abs(unblocked - clear) < clear * 0.01f,
           "an occludable voice with a clear path sounds unchanged");
    expect(blocked < clear * 0.05f, "a blocked voice is quieter and muffled");

    // The occlusion glides in rather than switching at once.
    AudioSystem audio;
    audio.init(AudioConfig{.outputEnabled = false});
    const AudioClipHandle clip = audio.createClip(buzz, 1, 48'000).handle;
    audio.setOcclusionQuery([](const glm::vec3&, const glm::vec3&) { return 1.0f; });
    Scene scene;
    addListener(scene);
    audio.playAt(clip, {.position = {0.0f, 0.0f, -2.0f}, .occlusion = true});
    audio.syncScene(scene);
    const float first = energy(audio, 480).x;
    const float later = energy(audio, 480).x;
    expect(first > later * 1.5f, "occlusion glides in over tens of milliseconds");
}
} // namespace

// With every voice busy, a new sound takes the voice of the lowest-priority
// sound, then the quietest, then the oldest, and never streamed music or the
// Music bus; a sound of lower priority than every playing one fails.
void testVoiceStealing() {
    Logger logger(LoggerConfig{.minimumLevel = LogLevel::Debug, .echoToStderr = false});
    AudioSystem audio(&logger);
    expect(static_cast<bool>(audio.init(AudioConfig{.maxVoices = 3, .outputEnabled = false})),
           "voice stealing test initializes");
    const std::vector<float> tone(480, 0.5f);
    const AudioClipHandle clip = audio.createClip(tone, 1, 48'000).handle;
    const auto low = audio.play(clip, {.loop = true, .priority = AudioPriority::Low});
    const auto quiet = audio.play(clip, {.gain = 0.2f, .loop = true});
    const auto loud = audio.play(clip, {.gain = 1.0f, .loop = true});
    expect(low && quiet && loud && audio.stolenVoiceCount() == 0, "free voices are used before any is stolen");

    const auto high = audio.play(clip, {.loop = true, .priority = AudioPriority::High});
    expect(high && !audio.isPlaying(low.handle) && audio.isPlaying(quiet.handle) && audio.isPlaying(loud.handle),
           "a full system steals the lowest-priority voice");
    const auto louder = audio.play(clip, {.gain = 1.0f, .loop = true});
    expect(louder && !audio.isPlaying(quiet.handle) && audio.isPlaying(loud.handle),
           "among equal priorities the quietest voice is stolen");
    const auto newest = audio.play(clip, {.gain = 1.0f, .loop = true});
    expect(newest && !audio.isPlaying(loud.handle) && audio.isPlaying(louder.handle) && audio.isPlaying(high.handle),
           "among equally loud voices the oldest is stolen");
    expect(!audio.play(clip, {.priority = AudioPriority::Low}) && audio.isPlaying(newest.handle),
           "a sound below every playing voice's priority steals nothing");
    expect(audio.stolenVoiceCount() == 3, "stolen voices are counted");
    expect(warningsMentioning(logger, "audio voices are busy") == 1, "a run of full voices is logged once");

    audio.stopAll();
    const auto stream = audio.loadStream(std::filesystem::path{ZENITH_ASSET_DIR} / "audio" / "ambient.ogg");
    const auto music = audio.playMusic(stream.handle);
    const auto musicClip = audio.play(clip, {.bus = AudioBus::Music, .loop = true, .priority = AudioPriority::Lowest});
    const auto effect = audio.play(clip, {.loop = true, .priority = AudioPriority::Highest});
    const auto urgent = audio.play(clip, {.loop = true, .priority = AudioPriority::Highest});
    expect(music && musicClip && effect && urgent && !audio.isPlaying(effect.handle) && audio.isPlaying(music.handle) &&
               audio.isPlaying(musicClip.handle),
           "streamed music and Music-bus voices are never stolen");
    audio.stop(urgent.handle);
    audio.stop(musicClip.handle);
    audio.play(clip, {.bus = AudioBus::Music, .loop = true});
    audio.play(clip, {.bus = AudioBus::Music, .loop = true});
    expect(!audio.play(clip, {.priority = AudioPriority::Highest}),
           "a system holding only music fails a new sound instead of stealing");
    audio.shutdown();
}

int main() {
    testFollowingVoices();
    testVoiceStealing();
    testOcclusion();
    // Unique per process, so concurrent direct runs of this executable (a
    // stress loop) do not delete each other's files.
    const auto directory =
        std::filesystem::temp_directory_path() / ("zenith_audio_tests_" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(directory);
    testGameplayAudio(directory / "project");
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
    expect(!audio.play(mono.handle, {.priority = AudioPriority::Low}),
           "playback below every playing voice's priority reports voice-limit exhaustion");
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
    // The decoder worker opens and decodes the file on its own schedule, so a
    // fixed sleep is not enough on a loaded machine (it failed ~15% of runs
    // with 16 instances on busy cores). Mix as the device callback would: an
    // underrun mixes silence without consuming anything, and the voice must
    // produce samples once the worker has refilled the ring. The deadline only
    // bounds a decoder that never delivers.
    std::vector<float> streamed(1024);
    const auto streamingDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool streamedSamples = false;
    while (!streamedSamples && std::chrono::steady_clock::now() < streamingDeadline) {
        audio.mix(streamed.data(), streamed.size() / 2);
        streamedSamples =
            std::any_of(streamed.begin(), streamed.end(), [](float sample) { return std::abs(sample) > 0.0001f; });
        if (!streamedSamples) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    expect(streamedSamples, "streaming refill recovers from initial underrun and produces samples");
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

    // The playback rate (the simulation's time scale) holds the scene's
    // voices at 0 and plays them faster above 1; persistent voices (music,
    // menu sounds) play in real time.
    AudioSystem rated;
    expect(static_cast<bool>(rated.init(AudioConfig{.maxVoices = 2, .outputEnabled = false})),
           "audio playback rate test initializes");
    const std::vector<float> tenthSecond(static_cast<size_t>(rated.config().sampleRate / 10), 0.1f);
    const auto ratedClip = rated.createClip(tenthSecond, 1, rated.config().sampleRate);
    const auto heldVoice = rated.play(ratedClip.handle);
    const auto menuVoice = rated.play(ratedClip.handle, {.scope = AudioScope::Persistent});
    rated.setPlaybackRate(0.0f);
    rated.update(1.0f);
    expect(rated.isPlaying(heldVoice.handle), "a zero playback rate holds the scene's voices");
    expect(!rated.isPlaying(menuVoice.handle), "a persistent voice plays on while the scene is held");
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
