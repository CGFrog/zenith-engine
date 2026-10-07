// Game modules: the build-hash compatibility check, the system registry and
// scheduler, and loading the test game module from a shadow copy, registering
// its components, systems, and input actions, reloading it, and refusing
// modules that are missing, are not modules, or fail to register.

#include "engine/assets/Assets.hpp"
#include "engine/input/InputActions.hpp"
#include "engine/module/BuiltInSystems.hpp"
#include "engine/module/GameModule.hpp"
#include "engine/module/ModuleApi.hpp"
#include "engine/module/ModuleShadowCopy.hpp"
#include "engine/module/SystemScheduler.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/Scene.hpp"
#include "engine/scene/SceneDocument.hpp"

#include "support/Doctest.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct TestPaths {
    std::filesystem::path root;
    std::filesystem::path shadow;
    // A copy of the test module, so the test can overwrite it while loaded.
    std::filesystem::path module;
};

GameModuleOptions optionsFor(const TestPaths& paths) {
    return {.shadowDirectory = paths.shadow};
}

bool directoryEmpty(const std::filesystem::path& directory) {
    std::error_code error;
    return !std::filesystem::exists(directory, error) || std::filesystem::is_empty(directory, error);
}

int32_t intField(const ComponentType& type, const void* component, std::string_view key) {
    for (const FieldDescriptor& field : type.fields()) {
        if (field.key == key) {
            return field.value<int32_t>(component);
        }
    }
    return -1;
}

float floatField(const ComponentType& type, const void* component, std::string_view key) {
    for (const FieldDescriptor& field : type.fields()) {
        if (field.key == key) {
            return field.value<float>(component);
        }
    }
    return -1.0f;
}

// A copy of the test module in the case's own directory, with an empty
// shadow directory beside it.
struct ModuleFixture {
    explicit ModuleFixture(std::string_view label)
        : root(label) {
        paths.root = root.path();
        paths.shadow = paths.root / "shadow";
        std::error_code error;
        std::filesystem::create_directories(paths.root / "source", error);
        paths.module = paths.root / "source" / std::filesystem::path(ZENITH_TEST_GAME_MODULE).filename();
        std::filesystem::copy_file(ZENITH_TEST_GAME_MODULE, paths.module, error);
        CHECK_MESSAGE(!error, "the test module copies into the test directory");
    }

    testsupport::TempDirectory root;
    TestPaths paths;
};

struct HostCounterComponent {
    int32_t value{0};
};
} // namespace

TEST_SUITE_BEGIN("GameModule");

TEST_CASE("GameModule: compatibility") {
    CHECK_MESSAGE(engineModuleBuildHash() == ZENITH_MODULE_BUILD_HASH,
                  "a module compiled with the engine's toolchain reports the engine's build hash");
    CHECK_MESSAGE(checkModuleCompatibility("same", moduleApiVersion, engineModuleBuildHash()).success,
                  "a module with the engine's API version and build hash is compatible");
    const ModuleCompatibilityResult olderApi = checkModuleCompatibility("older", moduleApiVersion + 1, "x");
    CHECK_MESSAGE((!olderApi && olderApi.error.find("API version") != std::string::npos),
                  "a module targeting another API version is refused with a descriptive error");
    const ModuleCompatibilityResult otherBuild =
        checkModuleCompatibility("other", moduleApiVersion, "api1-othercompiler");
    CHECK_MESSAGE((!otherBuild && otherBuild.error.find("othercompiler") != std::string::npos),
                  "a module built with another toolchain or runtime is refused");
}

TEST_CASE("GameModule: system registry and scheduler") {
    SystemRegistry registry;
    std::vector<std::string> calls;
    const auto record = [&calls](std::string name) {
        return [&calls, name](const SystemContext&) { calls.push_back(name); };
    };
    CHECK_MESSAGE(!registry.add({.name = "", .run = record("unnamed")}), "a system without a name is rejected");
    CHECK_MESSAGE(!registry.add({.name = "no_callback"}), "a system without a callback is rejected");
    CHECK_MESSAGE(registry.add({.name = "late", .order = 10, .run = record("late")}).success,
                  "a named system with a callback registers");
    CHECK_MESSAGE(!registry.add({.name = "late", .run = record("again")}), "duplicate system names are rejected");
    CHECK_MESSAGE((registry.add({.name = "early_a", .order = -3, .run = record("early_a")}).success &&
                   registry.add({.name = "early_b", .order = -3, .run = record("early_b")}).success &&
                   registry.add({.name = "editor_only", .runIn = RunIn::Edit, .run = record("editor_only")}).success &&
                   registry.add({.name = "both", .runIn = RunIn::Edit | RunIn::Play, .run = record("both")}).success &&
                   registry
                       .add({.name = "fixed",
                             .phase = SystemPhase::FixedUpdate,
                             .runIn = RunIn::EditAndPlay,
                             .run = record("fixed")})
                       .success),
                  "systems for every phase and mode register");
    CHECK_MESSAGE((registry.find("both") != nullptr && registry.find("missing") == nullptr),
                  "systems are found by name");

    const SystemScheduler scheduler(std::make_shared<const SystemRegistry>(registry));
    CHECK_MESSAGE(
        (scheduler.schedule(SystemPhase::Update, SimulationMode::Play) ==
         std::vector<std::string>{"early_a", "early_b", "both", "late"}),
        "play updates run by ascending order, keeping registration order for ties, without edit-only systems");
    CHECK_MESSAGE((scheduler.schedule(SystemPhase::Update, SimulationMode::Edit) ==
                   std::vector<std::string>{"editor_only", "both"}),
                  "edit updates run only systems flagged for edit mode");
    CHECK_MESSAGE(scheduler.schedule(SystemPhase::FixedUpdate, SimulationMode::Play) ==
                      std::vector<std::string>{"fixed"},
                  "fixed updates run only fixed-phase systems");

    Scene scene;
    scheduler.run(SystemPhase::Update, {.scene = scene, .mode = SimulationMode::Play, .deltaSeconds = 0.016f});
    CHECK_MESSAGE((calls == std::vector<std::string>{"early_a", "early_b", "both", "late"}),
                  "run invokes the scheduled systems in order");
    CHECK_MESSAGE(SystemScheduler().schedule(SystemPhase::Update, SimulationMode::Play).empty(),
                  "an empty scheduler runs nothing");
}

TEST_CASE("GameModule: load register and unload") {
    const ModuleFixture fixture("game-module-load");
    const TestPaths& paths = fixture.paths;
    std::error_code error;
#if defined(_WIN32)
    // A copy left behind by a process that exited without unloading.
    std::filesystem::create_directories(paths.shadow, error);
    const std::filesystem::path staleLibrary =
        paths.shadow / (paths.module.stem().string() + "-0123456789abcdef" + paths.module.extension().string());
    std::filesystem::path staleSymbols = staleLibrary;
    staleSymbols.replace_extension(".pdb");
    std::ofstream(staleLibrary) << "stale";
    std::ofstream(staleSymbols) << "stale";
#endif

    // Another owner's copy, written but not loaded yet (another process
    // creating its shadow copy at the same moment).
    ModuleShadowCopy pending;
    CHECK_MESSAGE(static_cast<bool>(ModuleShadowCopy::create(paths.module, paths.shadow, pending)),
                  "a shadow copy is created without loading it");

    std::unique_ptr<GameModule> module;
    const GameModuleResult loaded = GameModule::load(paths.module, optionsFor(paths), module);
#if defined(_WIN32)
    CHECK_MESSAGE((!std::filesystem::exists(staleLibrary) && !std::filesystem::exists(staleSymbols)),
                  "loading a module removes stale shadow copies of it");
#endif
    CHECK_MESSAGE(std::filesystem::exists(pending.library()),
                  "stale-copy cleanup never deletes a copy its owner has written but not loaded yet");
    const std::filesystem::path pendingLibrary = pending.library();
    pending = ModuleShadowCopy();
    CHECK_MESSAGE(!std::filesystem::exists(pendingLibrary), "a shadow copy deletes its files when released");
    CHECK_MESSAGE((loaded.success && module != nullptr), "the test game module loads");
    if (!loaded) {
        MESSAGE(loaded.error);
        return;
    }

    CHECK_MESSAGE((module->name() == "zenith_test_game" && module->version() == "1.0.0"),
                  "the module reports its name and version");
    const std::filesystem::path shadowPath = module->loadedPath();
    CHECK_MESSAGE((module->sourcePath() == std::filesystem::absolute(paths.module).lexically_normal() &&
                   shadowPath != module->sourcePath() && shadowPath.parent_path() == paths.shadow &&
                   std::filesystem::exists(shadowPath)),
                  "the module is loaded from a shadow copy in the shadow directory");
#if defined(ZENITH_TEST_GAME_MODULE_PDB)
    if (std::filesystem::exists(ZENITH_TEST_GAME_MODULE_PDB)) {
        std::filesystem::path shadowSymbols = shadowPath;
        shadowSymbols.replace_extension(".pdb");
        CHECK_MESSAGE(std::filesystem::exists(shadowSymbols),
                      "the module's program database is copied beside the shadow under a unique name");
        CHECK_MESSAGE(testsupport::readText(shadowPath).find(shadowSymbols.filename().string()) != std::string::npos,
                      "the shadow copy names its own program database");
    }
#endif

    std::filesystem::copy_file(paths.module, paths.root / "overwrite-source.bin",
                               std::filesystem::copy_options::overwrite_existing, error);
    std::filesystem::copy_file(paths.root / "overwrite-source.bin", paths.module,
                               std::filesystem::copy_options::overwrite_existing, error);
    CHECK_MESSAGE(!error, "the original module file can be replaced while the module is loaded");

    std::shared_ptr<const ComponentTypeRegistry> types = module->componentTypes();
    CHECK_MESSAGE((module->moduleComponentKeys().size() == 1 && module->moduleComponentKeys()[0] == "test_counter"),
                  "the module reports the component keys it added");
    CHECK_MESSAGE(
        (types->types().size() == builtInComponentTypes()->types().size() + 1 && types->find("point_light") != nullptr),
        "module component types extend a copy of the built-in types");
    CHECK_MESSAGE(builtInComponentTypes()->find("test_counter") == nullptr,
                  "loading a module leaves the built-in registry unchanged");
    const ComponentType* counterType = types->find("test_counter");
    CHECK_MESSAGE((counterType != nullptr && counterType->fields().size() == 10 && counterType->category() == "Test"),
                  "the module's component type carries its reflected fields");
    if (counterType == nullptr) {
        return;
    }

    std::string serialized;
    {
        Scene scene;
        const Entity entity = scene.createEntity("Counter");
        counterType->addDefault(scene, entity);
        const SystemScheduler scheduler(module->systems());
        CHECK_MESSAGE(module->registriesShared(), "a scheduler shares the module's systems");
        CHECK_MESSAGE((scheduler.schedule(SystemPhase::Update, SimulationMode::Play) ==
                       std::vector<std::string>{"add_one", std::string(EngineSystems::Behaviours),
                                                std::string(EngineSystems::NavAgents), "double", "context_probe",
                                                "fire", std::string(EngineSystems::Animator)}),
                      "module systems are ordered by their order, not registration");
        CHECK_MESSAGE((scheduler.schedule(SystemPhase::Update, SimulationMode::Edit) ==
                       std::vector<std::string>{std::string(EngineSystems::Behaviours), "edit_tick", "context_probe"}),
                      "edit mode runs only the module's edit systems");
        // The engine's own systems come first.
        CHECK_MESSAGE((scheduler.schedule(SystemPhase::LateUpdate, SimulationMode::Play) ==
                           std::vector<std::string>{std::string(EngineSystems::FirstPersonCamera),
                                                    std::string(EngineSystems::LateBehaviours),
                                                    std::string(EngineSystems::AudioEmitters), "late_tick"} &&
                       scheduler.schedule(SystemPhase::Ui, SimulationMode::Play) == std::vector<std::string>{"hud"}),
                      "module systems register in the LateUpdate and Ui phases");
        CHECK_MESSAGE((module->inputActions() != nullptr &&
                       module->inputActions()->actionNames() == std::vector<std::string>{"Fire"}),
                      "the module's declared input actions reach the host");

        scheduler.run(SystemPhase::Update, {.scene = scene, .mode = SimulationMode::Play, .deltaSeconds = 0.5f});
        scheduler.run(SystemPhase::Update, {.scene = scene, .mode = SimulationMode::Edit, .deltaSeconds = 0.5f});
        scheduler.run(SystemPhase::FixedUpdate,
                      {.scene = scene, .mode = SimulationMode::Edit, .deltaSeconds = 1.0f / 60.0f});
        const void* counter = counterType->find(scene, entity);
        CHECK_MESSAGE((intField(*counterType, counter, "value") == 2 &&
                       floatField(*counterType, counter, "elapsed_seconds") == 0.5f),
                      "module play systems run in order and receive the frame time");
        CHECK_MESSAGE(
            (intField(*counterType, counter, "edit_ticks") == 1 && intField(*counterType, counter, "fixed_ticks") == 1),
            "module edit and fixed systems run only in their mode and phase");

        AssetManager assets;
        const SceneDocumentOptions documentOptions{.componentTypes = types};
        CHECK_MESSAGE((SceneDocument::serialize(scene, assets, documentOptions, serialized).success &&
                       serialized.find("test_counter") != std::string::npos),
                      "scenes serialize module components through the module's registry");
        Scene reloaded;
        CHECK_MESSAGE(SceneDocument::deserialize(serialized, assets, documentOptions, reloaded).success,
                      "scenes with module components load through the module's registry");
        bool roundTripped = false;
        for (const Entity candidate : reloaded.registry().view<NameComponent>()) {
            if (const void* loadedCounter = counterType->find(reloaded, candidate); loadedCounter != nullptr) {
                roundTripped = intField(*counterType, loadedCounter, "value") == 2;
            }
        }
        CHECK_MESSAGE(roundTripped, "module component values survive a document round trip");
    }
    CHECK_MESSAGE(module->registriesShared(), "an outside registry reference keeps the module's registries shared");
    // Every scene, scheduler, and registry reference is released before unload.
    types.reset();
    CHECK_MESSAGE(!module->registriesShared(), "the module's registries are unshared once outside references are gone");

    module.reset();
    CHECK_MESSAGE(!std::filesystem::exists(shadowPath), "unloading the module deletes its shadow copy");
    CHECK_MESSAGE(directoryEmpty(paths.shadow), "unloading the module leaves no shadow files behind");
}

TEST_CASE("GameModule: concurrent loads and reload") {
    const ModuleFixture fixture("game-module-concurrent");
    const TestPaths& paths = fixture.paths;
    std::unique_ptr<GameModule> first;
    std::unique_ptr<GameModule> second;
    CHECK_MESSAGE((GameModule::load(paths.module, optionsFor(paths), first).success &&
                   GameModule::load(paths.module, optionsFor(paths), second).success),
                  "the same module loads twice from independent shadow copies");
    if (!first || !second) {
        return;
    }
    CHECK_MESSAGE(first->loadedPath() != second->loadedPath(), "each load uses a uniquely named shadow copy");
    CHECK_MESSAGE(first->componentTypes() != second->componentTypes(), "each load owns its own registries");

    first.reset();
    {
        Scene scene;
        const ComponentType* counterType = second->componentTypes()->find("test_counter");
        const Entity entity = scene.createEntity();
        counterType->addDefault(scene, entity);
        const SystemScheduler scheduler(second->systems());
        scheduler.run(SystemPhase::Update, {.scene = scene, .mode = SimulationMode::Play, .deltaSeconds = 0.1f});
        CHECK_MESSAGE(intField(*counterType, counterType->find(scene, entity), "value") == 2,
                      "unloading one instance leaves the other loaded and working");
    }
    second.reset();

    std::unique_ptr<GameModule> reloaded;
    CHECK_MESSAGE(
        (GameModule::load(paths.module, optionsFor(paths), reloaded).success && reloaded->name() == "zenith_test_game"),
        "the module reloads after being unloaded");
    reloaded.reset();
    CHECK_MESSAGE(directoryEmpty(paths.shadow), "every unloaded instance removes its shadow copy");
}

TEST_CASE("GameModule: load failures") {
    const ModuleFixture fixture("game-module-failures");
    const TestPaths& paths = fixture.paths;
    std::unique_ptr<GameModule> module;
    const GameModuleResult missing = GameModule::load(paths.root / "missing_module.dll", optionsFor(paths), module);
    CHECK_MESSAGE((!missing && missing.error.find("not found") != std::string::npos && module == nullptr),
                  "a missing module file fails with a descriptive error");

    const GameModuleResult notAModule = GameModule::load(ZENITH_TEST_NOT_A_MODULE, optionsFor(paths), module);
    CHECK_MESSAGE(
        (!notAModule && notAModule.error.find(moduleApiVersionSymbol) != std::string::npos && module == nullptr),
        "a library without module entry points is refused");

    const GameModuleResult mismatched = GameModule::load(ZENITH_TEST_MISMATCHED_MODULE, optionsFor(paths), module);
    CHECK_MESSAGE((!mismatched && mismatched.error.find("API version 999") != std::string::npos && module == nullptr),
                  "a module built for another API version is refused before it registers");

    auto conflicting = std::make_shared<ComponentTypeRegistry>(*builtInComponentTypes());
    CHECK_MESSAGE(
        conflicting->add(ComponentTypeBuilder<HostCounterComponent>("test_counter", "Host Counter").build()).success,
        "the host registers a type whose key the module also uses");
    const GameModuleResult duplicate =
        GameModule::load(paths.module, {.baseComponentTypes = conflicting, .shadowDirectory = paths.shadow}, module);
    CHECK_MESSAGE((!duplicate && duplicate.error.find("failed to register") != std::string::npos && module == nullptr),
                  "a module whose registration fails is unloaded with the registration error");

    CHECK_MESSAGE(directoryEmpty(paths.shadow), "failed loads leave no shadow files behind");
}

TEST_SUITE_END();
