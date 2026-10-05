// CPU-only coverage of reloading and importing assets: texture reload and
// re-pointing scene users (and material files), texture import settings,
// audio clip reload with emitters and voices, definition table schemas that
// modules register, and OS file drops in the input state.

#include "engine/assets/AssetDatabase.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/assets/MaterialAssetFile.hpp"
#include "engine/assets/TextureImportSettings.hpp"
#include "engine/audio/AudioSystem.hpp"
#include "engine/behaviour/BehaviourRegistry.hpp"
#include "engine/gameplay/DefinitionSchemas.hpp"
#include "engine/input/InputActions.hpp"
#include "engine/input/InputManager.hpp"
#include "engine/module/ModuleRegistrar.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/Scene.hpp"
#include "engine/scene/SceneAssetReferences.hpp"
#include "engine/scene/SceneAssetReplacement.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

const std::filesystem::path decalImages = std::filesystem::path(ZENITH_ASSET_DIR) / "showcase" / "decals";

// A temporary project owned by one test.
struct TemporaryProject {
    std::filesystem::path root;

    explicit TemporaryProject(std::string_view name)
        : root(std::filesystem::temp_directory_path() / name) {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "textures");
        std::filesystem::create_directories(root / "materials");
        std::filesystem::create_directories(root / "sounds");
    }

    ~TemporaryProject() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    TemporaryProject(const TemporaryProject&) = delete;
    TemporaryProject& operator=(const TemporaryProject&) = delete;
};

void replaceFile(const std::filesystem::path& source, const std::filesystem::path& destination) {
    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing);
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
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
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

void testTextureReload() {
    TemporaryProject project("zenith_asset_reload_texture");
    const std::filesystem::path image = project.root / "textures" / "plate.png";
    replaceFile(decalImages / "decal_plate.png", image);
    MaterialFile materialFile;
    materialFile.textures[static_cast<size_t>(MaterialTextureSlot::BaseColor)] = {.path = "../textures/plate.png"};
    const std::filesystem::path materialPath = project.root / "materials" / "plate.zmat";
    expect(static_cast<bool>(writeMaterialFile(materialPath, materialFile)), "the material file should be written");

    AssetManager assets;
    const AssetLoadResult<TextureHandle> color = assets.loadTexture(image);
    const AssetLoadResult<TextureHandle> data = assets.loadTexture(image, TextureUsage::Data);
    const AssetLoadResult<MaterialHandle> material = assets.loadMaterial(materialPath);
    expect(color && data && material && color.handle != data.handle,
           "one image read as color and as data should be two textures");
    expect(material && assets.material(material.handle)->baseColorTexture == color.handle,
           "the material should share the color texture's cache entry");

    Scene scene;
    const Entity decalEntity = scene.createEntity("Decal");
    scene.registry().emplace<DecalComponent>(decalEntity, DecalComponent{.albedoTexture = color.handle});
    const Entity probeEntity = scene.createEntity("Probe");
    scene.registry().emplace<ReflectionProbeComponent>(probeEntity,
                                                       ReflectionProbeComponent{.environmentMap = data.handle});

    // The file changes on disk: another image, another size.
    replaceFile(decalImages / "decal_plate_normal.png", image);
    const TextureReloadResult reloaded = assets.reloadTexture(image);
    expect(reloaded && reloaded.replaced.size() == 2, "both loaded usages of the image should be reloaded");
    expect(reloaded.materialFiles.size() == 1 && reloaded.materialFiles.front().filename() == "plate.zmat",
           "the material file sampling the image should be listed");
    TextureHandle newColor;
    TextureHandle newData;
    for (const TextureReplacement& replacement : reloaded.replaced) {
        expect(replacement.previous != replacement.current, "a reload should produce a new generation");
        (replacement.previous == color.handle ? newColor : newData) = replacement.current;
    }
    expect(assets.texture(newData) != nullptr && assets.texture(newData)->usage == TextureUsage::Data,
           "a reloaded texture should keep its usage");
    expect(assets.loadTexture(image).handle == newColor, "future loads should return the reloaded texture");
    expect(assets.texture(color.handle) != nullptr, "the previous texture should stay valid for its users");

    const size_t changed = replaceSceneTextures(scene, *builtInComponentTypes(), reloaded.replaced);
    expect(changed == 2 && scene.registry().get<DecalComponent>(decalEntity).albedoTexture == newColor &&
               scene.registry().get<ReflectionProbeComponent>(probeEntity).environmentMap == newData,
           "scene components should be re-pointed at the reloaded textures");
    const AssetLoadResult<MaterialHandle> reloadedMaterial = assets.reloadMaterial(materialPath);
    expect(reloadedMaterial && assets.material(reloadedMaterial.handle)->baseColorTexture == newColor,
           "a reloaded material should sample the reloaded texture");

    // Nothing uses the previous generation any more, so a collection frees it.
    const AssetLeaseSet leases = assets.leaseSceneAssets(scene);
    assets.collectUnused(true);
    expect(assets.texture(color.handle) == nullptr && assets.texture(data.handle) == nullptr,
           "the previous textures should be collected once nothing uses them");
    expect(assets.texture(newColor) != nullptr && assets.texture(newData) != nullptr,
           "the leased replacements should survive the collection");

    const TextureReloadResult unloaded = assets.reloadTexture(project.root / "textures" / "other.png");
    expect(unloaded && unloaded.replaced.empty(), "reloading an image nobody loaded should do nothing");
    std::ofstream(image, std::ios::binary | std::ios::trunc) << "not an image";
    const TextureReloadResult broken = assets.reloadTexture(image);
    expect(!broken && assets.loadTexture(image).handle == newColor, "a failed reload should keep the loaded texture");
}

void testTextureImportSettings() {
    TextureImportSettings settings;
    expect(readTextureImportSettings(nlohmann::json::object(), settings).empty() &&
               settings.usage == TextureUsage::Color,
           "missing settings should read as color");
    nlohmann::json json{{"other", 1}};
    writeTextureImportSettings({.usage = TextureUsage::Normal}, json);
    expect(json["usage"] == "normal" && json["other"] == 1, "writing settings should keep other keys");
    expect(readTextureImportSettings(json, settings).empty() && settings.usage == TextureUsage::Normal,
           "texture settings should round trip");
    expect(!readTextureImportSettings({{"usage", "shiny"}}, settings).empty() && settings.usage == TextureUsage::Color,
           "an unknown usage should be an error that keeps the defaults");

    TemporaryProject project("zenith_asset_reload_settings");
    replaceFile(decalImages / "decal_plate.png", project.root / "textures" / "plate.png");
    auto database = std::make_shared<AssetDatabase>(project.root);
    expect(static_cast<bool>(database->rescan()), "the database should scan the project");
    const std::optional<AssetGuid> guid = database->guidForPath("textures/plate.png");
    expect(guid && database->setImportSettings(*guid, {{"usage", "data"}}), "settings should be stored");

    AssetManager assets;
    const SceneDocumentOptions options{.assetRoot = project.root, .assetDatabase = database};
    TextureHandle loaded;
    expect(loadProjectTexture("textures/plate.png", assets, options, loaded) && assets.texture(loaded) != nullptr &&
               assets.texture(loaded)->usage == TextureUsage::Data,
           "a component's texture should load with its import settings' usage");
    expect(textureImportSettingsFor(*database, project.root / "textures" / "plate.png").usage == TextureUsage::Data,
           "the settings should be found by absolute path");
}

void testAudioReload() {
    TemporaryProject project("zenith_asset_reload_audio");
    const std::filesystem::path clip = project.root / "sounds" / "hit.wav";
    writeWav(clip, 2, {16'384, -16'384, 8'192, -8'192});
    AudioSystem audio;
    expect(static_cast<bool>(audio.init(AudioConfig{.outputEnabled = false})), "audio should start without output");
    const AudioLoadResult<AudioClipHandle> native = audio.loadClip(clip);
    const AudioLoadResult<AudioClipHandle> mono = audio.loadClip(clip, true);
    expect(native && mono, "the clip should load as is and mixed to mono");
    const AudioLoadResult<AudioVoiceHandle> voice = audio.play(native.handle, {.loop = true});
    expect(voice && audio.isPlaying(voice.handle), "the clip should play");

    Scene scene;
    const Entity emitter = scene.createEntity("Emitter");
    scene.registry().emplace<AudioEmitterComponent>(emitter, AudioEmitterComponent{.clip = mono.handle});

    writeWav(clip, 1, {1'000, 2'000, 3'000, 4'000, 5'000, 6'000});
    const AudioClipReloadResult reloaded = audio.reloadClip(clip);
    expect(reloaded && reloaded.replaced.size() == 2, "both loaded forms of the clip should be reloaded");
    expect(!audio.isPlaying(voice.handle), "voices of the previous clip should stop");
    expect(replaceAudioEmitterClips(scene, reloaded.replaced) == 1, "the emitter should be re-pointed");
    const AudioClipHandle current = scene.registry().get<AudioEmitterComponent>(emitter).clip;
    expect(current != mono.handle && audio.loadClip(clip, true).handle == current,
           "the emitter and future loads should use the reloaded clip");
    expect(static_cast<bool>(audio.play(current)), "the reloaded clip should play");
    const AudioClipReloadResult unloaded = audio.reloadClip(project.root / "sounds" / "missing.wav");
    expect(unloaded && unloaded.replaced.empty(), "reloading a clip nobody loaded should do nothing");
    audio.shutdown();
}

struct EnemyDef {
    std::string id;
    int health{0};
};

Fields<EnemyDef> enemyFields() {
    return Fields<EnemyDef>().add("id", &EnemyDef::id).add("health", &EnemyDef::health);
}

void testDefinitionSchemas() {
    ComponentTypeRegistry types;
    SystemRegistry systems;
    InputActionMap actions;
    BehaviourRegistry behaviours;
    DefinitionSchemaRegistry schemas;
    {
        ModuleRegistrar registrar(types, systems, actions, behaviours, &schemas);
        expect(registrar.addDefinitionTable<EnemyDef>("enemies", enemyFields()), "a schema should register");
        expect(!registrar.addDefinitionTable<EnemyDef>("enemies", enemyFields()) &&
                   registrar.firstError().find("twice") != std::string::npos,
               "a schema name registered twice should fail the registration");
    }
    const DefinitionSchema* enemies = schemas.find("enemies");
    expect(enemies != nullptr && enemies->fields.size() == 2 && enemies->fields[1].key == "health",
           "the schema should describe the entry fields");
    expect(enemies != nullptr && enemies->validate(R"([{"id": "rat", "health": 3}])", "enemies.json"),
           "valid entries should pass");
    const DefinitionTableResult typo = enemies != nullptr
                                           ? enemies->validate(R"([{"id": "rat", "helth": 3}])", "enemies.json")
                                           : DefinitionTableResult{};
    expect(!typo && typo.error.find("enemies.json[0] 'rat': unknown field 'helth'") != std::string::npos,
           "a typo should be reported with the file and entry");
    expect(!schemas.add({.name = "broken", .fields = {}, .validate = enemies->validate}),
           "a schema without an id field should be rejected");

    ModuleRegistrar packaged(types, systems, actions, behaviours);
    expect(packaged.addDefinitionTable<EnemyDef>("enemies", enemyFields()) && packaged.firstError().empty(),
           "a host without a schema registry should accept and drop schemas");
}

void testDroppedFiles() {
    InputState state;
    state.droppedFiles.push_back({.path = "C:/models/crate.glb", .position = {10.0f, 20.0f}});
    InputEdgeBuffer edges;
    edges.accumulate(state);
    expect(edges.apply(state).droppedFiles.empty(), "fixed-step input should not repeat the frame's drops");
    expect(state.droppedFiles.size() == 1, "the frame's input should keep its drops");
}
} // namespace

int main() {
    testTextureReload();
    testTextureImportSettings();
    testAudioReload();
    testDefinitionSchemas();
    testDroppedFiles();
    if (failures != 0) {
        std::cerr << failures << " asset reload checks failed\n";
        return 1;
    }
    std::cout << "Asset reload tests passed\n";
    return 0;
}
