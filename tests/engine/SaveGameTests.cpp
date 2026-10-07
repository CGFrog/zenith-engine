// Save games: runtime-state fields, saving a scene as changes against its
// baseline document, loading them back, slots, recovery, a game's own data
// in slots (a roguelike run) with versions, and profiles kept across runs.

#include "engine/assets/Assets.hpp"
#include "engine/gameplay/Random.hpp"
#include "engine/module/Gameplay.hpp"
#include "engine/module/ModuleRegistrar.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/SaveGame.hpp"
#include "engine/scene/Scene.hpp"
#include "engine/scene/SceneDocument.hpp"

#include "support/Doctest.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

TEST_SUITE_BEGIN("SaveGame");

namespace {

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
        std::filesystem::create_directories(root / "project" / "scenes");
        CHECK_MESSAGE(linkGameplay(registerHealth, gameplay).success, "the test gameplay links");
        options = {.assetRoot = root / "project", .componentTypes = gameplay.componentTypes};
    }

    // Writes the level: a hero, a crate, and a barrel.
    std::filesystem::path writeLevel() {
        Scene level;
        const Entity hero = level.createEntity("Hero");
        level.registry().emplace<Health>(hero, Health{.maximum = 150, .current = 150});
        level.createEntity("Crate");
        level.createEntity("Barrel");
        const std::filesystem::path path = root / "project" / "scenes" / "level.zenith-scene";
        CHECK_MESSAGE(SceneDocument::save(path, level, assets, options).success, "the level saves");
        return path;
    }

    const testsupport::TempDirectory directory{"save-game"};
    const std::filesystem::path root = directory.path();
    Gameplay gameplay;
    AssetManager assets;
    SceneDocumentOptions options;
};

TEST_CASE("SaveGame: runtime state fields") {
    Fixture fixture;
    const std::filesystem::path level = fixture.writeLevel();
    std::ifstream input(level);
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    CHECK_MESSAGE((text.find("\"maximum\"") != std::string::npos && text.find("\"current\"") == std::string::npos &&
                   text.find("\"alive\"") == std::string::npos),
                  "runtime-state fields are left out of scene documents");

    Scene loaded;
    CHECK_MESSAGE(SceneDocument::load(level, fixture.assets, fixture.options, loaded).success,
                  "a scene document without runtime-state fields loads");
    const Entity hero = findNamed(loaded, "Hero");
    CHECK_MESSAGE((hero != entt::null && loaded.registry().get<Health>(hero).maximum == 150 &&
                   loaded.registry().get<Health>(hero).current == 100),
                  "runtime-state fields keep their defaults when a scene loads");

    SceneDocumentOptions saveOptions = fixture.options;
    saveOptions.purpose = SceneDocumentPurpose::SaveGame;
    std::string saved;
    CHECK_MESSAGE((SceneDocument::serialize(loaded, fixture.assets, saveOptions, saved).success &&
                   saved.find("\"current\"") != std::string::npos),
                  "a save-game document keeps runtime-state fields");
}

TEST_CASE("SaveGame: save and load") {
    Fixture fixture;
    const std::filesystem::path level = fixture.writeLevel();
    Scene playing;
    CHECK_MESSAGE(SceneDocument::load(level, fixture.assets, fixture.options, playing).success,
                  "the level loads to play");
    const Entity hero = findNamed(playing, "Hero");
    playing.registry().get<Health>(hero) = {.maximum = 150, .current = 40, .alive = true};
    playing.destroyEntity(findNamed(playing, "Crate"));
    const Entity loot = playing.createEntity("Loot");
    const SceneEntityId lootId = playing.id(loot);

    const SaveGameStore store(fixture.root / "saves", fixture.root / "project");
    CHECK_MESSAGE((!store.save("../escape", playing, fixture.assets, fixture.options, level).success &&
                   !store.save("", playing, fixture.assets, fixture.options, level).success),
                  "slot names cannot leave the save directory");
    const SaveGameResult saved = store.save("slot-1", playing, fixture.assets, fixture.options, level, "Chapter 1");
    CHECK_MESSAGE(saved.success, "a running scene saves into a slot");
    if (!saved) {
        std::cerr << saved.error << '\n';
    }

    std::ifstream input(store.pathOf("slot-1"));
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    CHECK_MESSAGE(
        (text.find("Barrel") == std::string::npos && text.find("scenes/level.zenith-scene") != std::string::npos),
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
    CHECK_MESSAGE(loaded.success, "a slot loads");
    if (!loaded) {
        std::cerr << loaded.error << '\n';
        return;
    }
    const Entity restoredHero = findNamed(restored, "Hero");
    CHECK_MESSAGE((restoredHero != entt::null && restored.registry().get<Health>(restoredHero).current == 40),
                  "runtime state comes back from a save");
    CHECK_MESSAGE((findNamed(restored, "Crate") == entt::null && restored.findEntity(lootId) != entt::null &&
                   findNamed(restored, "Leftover") == entt::null),
                  "destroyed and spawned entities come back, replacing the destination's contents");
    CHECK_MESSAGE((findNamed(restored, "Renamed Barrel") != entt::null),
                  "entities the save did not change follow the level");
    CHECK_MESSAGE((info.slot == "slot-1" && info.title == "Chapter 1" && info.savedAt > 0 &&
                   info.scene == std::filesystem::path("scenes/level.zenith-scene")),
                  "a load reports the slot's metadata");
}

TEST_CASE("SaveGame: slots and recovery") {
    Fixture fixture;
    const std::filesystem::path level = fixture.writeLevel();
    Scene playing;
    SceneDocument::load(level, fixture.assets, fixture.options, playing);
    const SaveGameStore store(fixture.root / "saves", fixture.root / "project");
    CHECK_MESSAGE(store.list().empty(), "a new store has no slots");
    CHECK_MESSAGE((store.save("first", playing, fixture.assets, fixture.options, level, "First").success &&
                   store.save("second", playing, fixture.assets, fixture.options, level, "Second").success),
                  "slots save");
    const std::vector<SaveGameInfo> slots = store.list();
    CHECK_MESSAGE((slots.size() == 2 && (slots[0].slot == "first" || slots[0].slot == "second") &&
                   slots[0].savedAt >= slots[1].savedAt),
                  "slots are listed newest first");

    // A save interrupted before it replaced the slot leaves the backup.
    playing.registry().get<Health>(findNamed(playing, "Hero")).current = 7;
    CHECK_MESSAGE(store.save("first", playing, fixture.assets, fixture.options, level, "First again").success,
                  "a slot is overwritten");
    {
        std::ofstream corrupt(store.pathOf("first"), std::ios::trunc);
        corrupt << "{ not json";
    }
    Scene recovered;
    SaveGameInfo info;
    CHECK_MESSAGE(
        (store.load("first", fixture.assets, fixture.options, recovered, &info).success && info.title == "First"),
        "a corrupted slot loads its previous save from the backup");
    std::filesystem::remove(store.pathOf("first"));
    CHECK_MESSAGE(
        (store.load("first", fixture.assets, fixture.options, recovered, &info).success && info.title == "First"),
        "a missing slot is recovered from its backup");

    CHECK_MESSAGE((store.remove("second").success && store.list().size() == 1), "a slot can be removed");
    CHECK_MESSAGE(!store.remove("second").success, "removing a missing slot fails");
    Scene untouched;
    untouched.createEntity("Kept");
    CHECK_MESSAGE((!store.load("second", fixture.assets, fixture.options, untouched).success &&
                   findNamed(untouched, "Kept") != entt::null),
                  "a failed load leaves the destination unchanged");
}

// A roguelike run as a game saves it instead of its generated floor.
struct ItemStack {
    std::string item;
    int32_t count{1};
};

struct Run {
    uint32_t floor{1};
    int32_t health{100};
    std::vector<ItemStack> inventory;
    RandomStreamsSnapshot random;
};

Fields<Run> runFields(std::string healthKey = "health") {
    return Fields<Run>()
        .add("floor", &Run::floor)
        .add(std::move(healthKey), &Run::health)
        .list("inventory", &Run::inventory,
              Fields<ItemStack>().add("item", &ItemStack::item).add("count", &ItemStack::count))
        .structure("random", &Run::random, randomStreamsSnapshotFields());
}

const SaveDataFormat runFormat{.kind = "run", .version = 1};

TEST_CASE("SaveGame: run data") {
    Fixture fixture;
    const SaveGameStore store(fixture.root / "saves", fixture.root / "project");
    RandomStreams streams(42U);
    for (int roll = 0; roll < 3; ++roll) {
        streams.stream("loot").nextUInt32();
    }
    const Run run{.floor = 3,
                  .health = 61,
                  .inventory = {{.item = "sword"}, {.item = "potion", .count = 2}},
                  .random = streams.snapshot()};
    CHECK_MESSAGE(store.saveData("run", run, runFields(), runFormat, "Floor 3").success, "a run saves as game data");
    const std::vector<SaveGameInfo> slots = store.list();
    CHECK_MESSAGE((slots.size() == 1 && slots[0].kind == "run" && slots[0].title == "Floor 3"),
                  "a data slot is listed with its kind");

    Run loaded;
    SaveGameInfo info;
    CHECK_MESSAGE((store.loadData("run", loaded, runFields(), runFormat, &info).success && info.kind == "run"),
                  "a run loads back");
    CHECK_MESSAGE((loaded.floor == 3 && loaded.health == 61 && loaded.inventory.size() == 2 &&
                   loaded.inventory[1].item == "potion" && loaded.inventory[1].count == 2),
                  "a run keeps its floor, stats, and inventory");
    RandomStreams resumed;
    resumed.restore(loaded.random);
    CHECK_MESSAGE((resumed.stream("loot").nextUInt32() == streams.stream("loot").nextUInt32() &&
                   resumed.stream("layout").nextUInt32() == streams.stream("layout").nextUInt32()),
                  "a loaded run's random streams continue exactly where they were saved");

    Scene scene;
    CHECK_MESSAGE(!store.load("run", fixture.assets, fixture.options, scene).success,
                  "a data slot does not load as a scene");
    const std::filesystem::path level = fixture.writeLevel();
    Scene playing;
    SceneDocument::load(level, fixture.assets, fixture.options, playing);
    CHECK_MESSAGE(store.save("scene", playing, fixture.assets, fixture.options, level).success,
                  "a scene slot saves beside it");
    Run untouched{.floor = 9};
    CHECK_MESSAGE((!store.loadData("scene", untouched, runFields(), runFormat).success && untouched.floor == 9),
                  "a scene slot does not load as data, and a failed load leaves the data unchanged");
    CHECK_MESSAGE(!store.loadData("run", untouched, runFields(), {.kind = "profile"}).success,
                  "data of another kind does not load");
    CHECK_MESSAGE((!store.saveData("bad", run, runFields(), {.kind = "scene"}).success &&
                   !store.saveData("bad", run, runFields(), {}).success),
                  "data needs a kind of its own");
}

TEST_CASE("SaveGame: data versions") {
    Fixture fixture;
    const SaveGameStore store(fixture.root / "saves");
    CHECK_MESSAGE(store.saveData("run", Run{.health = 40}, runFields(), runFormat).success, "a version 1 run saves");

    // Version 2 renamed "health" to "hit_points".
    Run upgraded;
    CHECK_MESSAGE(!store.loadData("run", upgraded, runFields("hit_points"), {.kind = "run", .version = 2}).success,
                  "an older version without a migration is refused");
    const SaveDataFormat version2{.kind = "run", .version = 2, .migrate = [](nlohmann::json& data, uint32_t from) {
                                      if (from == 1) {
                                          data["hit_points"] = data["health"];
                                          data.erase("health");
                                      }
                                      return std::string{};
                                  }};
    CHECK_MESSAGE((store.loadData("run", upgraded, runFields("hit_points"), version2).success && upgraded.health == 40),
                  "a migration upgrades older data before it is read");

    CHECK_MESSAGE(store.saveData("newer", Run{}, runFields("hit_points"), version2).success, "a version 2 run saves");
    Run older;
    const SaveGameResult refused = store.loadData("newer", older, runFields(), runFormat);
    CHECK_MESSAGE((!refused.success && refused.error.find("newer version") != std::string::npos),
                  "data from a newer version of the game is refused");
}

struct Profile {
    uint32_t runs{0};
    uint32_t deepestFloor{0};
    std::vector<ItemStack> unlocked;
};

Fields<Profile> profileFields() {
    return Fields<Profile>()
        .add("runs", &Profile::runs)
        .add("deepest_floor", &Profile::deepestFloor)
        .list("unlocked", &Profile::unlocked,
              Fields<ItemStack>().add("item", &ItemStack::item).add("count", &ItemStack::count));
}

TEST_CASE("SaveGame: profiles") {
    Fixture fixture;
    const SaveGameStore store(fixture.root / "saves");
    const SaveDataFormat format{.kind = "profile", .version = 1};
    Profile profile{.runs = 99};
    bool found = true;
    CHECK_MESSAGE(
        (store.loadProfile("player", profile, profileFields(), format, &found).success && !found && profile.runs == 99),
        "a missing profile loads as success and keeps the defaults");

    profile = {.runs = 4, .deepestFloor = 7, .unlocked = {{.item = "lantern"}}};
    CHECK_MESSAGE(store.saveProfile("player", profile, profileFields(), format).success, "a profile saves");
    CHECK_MESSAGE(store.saveData("run", Run{}, runFields(), runFormat).success, "a run saves beside the profile");
    CHECK_MESSAGE(store.list().size() == 1, "profiles are not listed as save slots");
    CHECK_MESSAGE(store.remove("run").success, "the run is deleted on death");

    Profile loaded;
    CHECK_MESSAGE((store.loadProfile("player", loaded, profileFields(), format, &found).success && found &&
                   loaded.runs == 4 && loaded.deepestFloor == 7 && loaded.unlocked.size() == 1 &&
                   loaded.unlocked[0].item == "lantern"),
                  "deleting a run keeps the profile");
    CHECK_MESSAGE(!store.loadProfile("player", loaded, profileFields(), {.kind = "run"}).success,
                  "a profile of another kind does not load");
}
} // namespace

TEST_SUITE_END();
