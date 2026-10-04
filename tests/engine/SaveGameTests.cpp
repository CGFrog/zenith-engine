// Save games: runtime-state fields, saving a scene as changes against its
// baseline document, loading them back, slots, and recovery.

#include "engine/assets/Assets.hpp"
#include "engine/module/Gameplay.hpp"
#include "engine/module/ModuleRegistrar.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/SaveGame.hpp"
#include "engine/scene/Scene.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

// A game component with a designer-authored field and runtime state.
struct Health {
    int32_t maximum{100};
    int32_t current{100};
    bool alive{true};
};

void registerHealth(ModuleRegistrar& registrar) {
    registrar.addComponentType(ComponentTypeBuilder<Health>("health", "Health")
                                   .fields(Fields<Health>()
                                               .add("maximum", &Health::maximum)
                                               .add("current", &Health::current)
                                               .runtimeState()
                                               .add("alive", &Health::alive)
                                               .runtimeState())
                                   .build());
}

Entity findNamed(Scene& scene, std::string_view name) {
    for (const Entity entity : scene.registry().view<NameComponent>()) {
        if (scene.registry().get<NameComponent>(entity).name == name) {
            return entity;
        }
    }
    return entt::null;
}

struct Fixture {
    Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root / "project" / "scenes", error);
        expect(linkGameplay(registerHealth, gameplay).success, "the test gameplay links");
        options = {.assetRoot = root / "project", .componentTypes = gameplay.componentTypes};
    }

    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    // Writes the level: a hero, a crate, and a barrel.
    std::filesystem::path writeLevel() {
        Scene level;
        const Entity hero = level.createEntity("Hero");
        level.registry().emplace<Health>(hero, Health{.maximum = 150, .current = 150});
        level.createEntity("Crate");
        level.createEntity("Barrel");
        const std::filesystem::path path = root / "project" / "scenes" / "level.zenith-scene";
        expect(SceneDocument::save(path, level, assets, options).success, "the level saves");
        return path;
    }

    std::filesystem::path root = std::filesystem::temp_directory_path() / "zenith_save_game_tests";
    Gameplay gameplay;
    AssetManager assets;
    SceneDocumentOptions options;
};

void testRuntimeStateFields() {
    Fixture fixture;
    const std::filesystem::path level = fixture.writeLevel();
    std::ifstream input(level);
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    expect(text.find("\"maximum\"") != std::string::npos && text.find("\"current\"") == std::string::npos &&
               text.find("\"alive\"") == std::string::npos,
           "runtime-state fields are left out of scene documents");

    Scene loaded;
    expect(SceneDocument::load(level, fixture.assets, fixture.options, loaded).success,
           "a scene document without runtime-state fields loads");
    const Entity hero = findNamed(loaded, "Hero");
    expect(hero != entt::null && loaded.registry().get<Health>(hero).maximum == 150 &&
               loaded.registry().get<Health>(hero).current == 100,
           "runtime-state fields keep their defaults when a scene loads");

    SceneDocumentOptions saveOptions = fixture.options;
    saveOptions.purpose = SceneDocumentPurpose::SaveGame;
    std::string saved;
    expect(SceneDocument::serialize(loaded, fixture.assets, saveOptions, saved).success &&
               saved.find("\"current\"") != std::string::npos,
           "a save-game document keeps runtime-state fields");
}

void testSaveAndLoad() {
    Fixture fixture;
    const std::filesystem::path level = fixture.writeLevel();
    Scene playing;
    expect(SceneDocument::load(level, fixture.assets, fixture.options, playing).success, "the level loads to play");
    const Entity hero = findNamed(playing, "Hero");
    playing.registry().get<Health>(hero) = {.maximum = 150, .current = 40, .alive = true};
    playing.destroyEntity(findNamed(playing, "Crate"));
    const Entity loot = playing.createEntity("Loot");
    const SceneEntityId lootId = playing.id(loot);

    const SaveGameStore store(fixture.root / "saves", fixture.root / "project");
    expect(!store.save("../escape", playing, fixture.assets, fixture.options, level).success &&
               !store.save("", playing, fixture.assets, fixture.options, level).success,
           "slot names cannot leave the save directory");
    const SaveGameResult saved = store.save("slot-1", playing, fixture.assets, fixture.options, level, "Chapter 1");
    expect(saved.success, "a running scene saves into a slot");
    if (!saved) {
        std::cerr << saved.error << '\n';
    }

    std::ifstream input(store.pathOf("slot-1"));
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    expect(text.find("Barrel") == std::string::npos && text.find("scenes/level.zenith-scene") != std::string::npos,
           "a save holds only the changed entities and its baseline relative to the content root");

    // The level is edited after the save: untouched entities follow it.
    {
        Scene edited;
        SceneDocument::load(level, fixture.assets, fixture.options, edited);
        edited.registry().get<NameComponent>(findNamed(edited, "Barrel")).name = "Renamed Barrel";
        SceneDocument::save(level, edited, fixture.assets, fixture.options);
    }

    Scene restored;
    restored.createEntity("Leftover");
    SaveGameInfo info;
    const SaveGameResult loaded = store.load("slot-1", fixture.assets, fixture.options, restored, &info);
    expect(loaded.success, "a slot loads");
    if (!loaded) {
        std::cerr << loaded.error << '\n';
        return;
    }
    const Entity restoredHero = findNamed(restored, "Hero");
    expect(restoredHero != entt::null && restored.registry().get<Health>(restoredHero).current == 40,
           "runtime state comes back from a save");
    expect(findNamed(restored, "Crate") == entt::null && restored.findEntity(lootId) != entt::null &&
               findNamed(restored, "Leftover") == entt::null,
           "destroyed and spawned entities come back, replacing the destination's contents");
    expect(findNamed(restored, "Renamed Barrel") != entt::null, "entities the save did not change follow the level");
    expect(info.slot == "slot-1" && info.title == "Chapter 1" && info.savedAt > 0 &&
               info.scene == std::filesystem::path("scenes/level.zenith-scene"),
           "a load reports the slot's metadata");
}

void testSlotsAndRecovery() {
    Fixture fixture;
    const std::filesystem::path level = fixture.writeLevel();
    Scene playing;
    SceneDocument::load(level, fixture.assets, fixture.options, playing);
    const SaveGameStore store(fixture.root / "saves", fixture.root / "project");
    expect(store.list().empty(), "a new store has no slots");
    expect(store.save("first", playing, fixture.assets, fixture.options, level, "First").success &&
               store.save("second", playing, fixture.assets, fixture.options, level, "Second").success,
           "slots save");
    const std::vector<SaveGameInfo> slots = store.list();
    expect(slots.size() == 2 && (slots[0].slot == "first" || slots[0].slot == "second") &&
               slots[0].savedAt >= slots[1].savedAt,
           "slots are listed newest first");

    // A save interrupted before it replaced the slot leaves the backup.
    playing.registry().get<Health>(findNamed(playing, "Hero")).current = 7;
    expect(store.save("first", playing, fixture.assets, fixture.options, level, "First again").success,
           "a slot is overwritten");
    {
        std::ofstream corrupt(store.pathOf("first"), std::ios::trunc);
        corrupt << "{ not json";
    }
    Scene recovered;
    SaveGameInfo info;
    expect(store.load("first", fixture.assets, fixture.options, recovered, &info).success && info.title == "First",
           "a corrupted slot loads its previous save from the backup");
    std::filesystem::remove(store.pathOf("first"));
    expect(store.load("first", fixture.assets, fixture.options, recovered, &info).success && info.title == "First",
           "a missing slot is recovered from its backup");

    expect(store.remove("second").success && store.list().size() == 1, "a slot can be removed");
    expect(!store.remove("second").success, "removing a missing slot fails");
    Scene untouched;
    untouched.createEntity("Kept");
    expect(!store.load("second", fixture.assets, fixture.options, untouched).success &&
               findNamed(untouched, "Kept") != entt::null,
           "a failed load leaves the destination unchanged");
}
} // namespace

int main() {
    testRuntimeStateFields();
    testSaveAndLoad();
    testSlotsAndRecovery();
    return failures == 0 ? 0 : 1;
}
