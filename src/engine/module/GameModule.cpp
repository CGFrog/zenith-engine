#include "GameModule.hpp"

#include "ModuleApi.hpp"
#include "ModuleRegistrar.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include <iostream>
#include <system_error>
#include <utility>

namespace {
template <typename Function> Function findEntryPoint(const SharedLibrary& library, const char* name) {
    return reinterpret_cast<Function>(library.symbol(name));
}

std::string moduleString(const SharedLibrary& library, const char* symbol) {
    const auto function = findEntryPoint<ZenithModuleStringFunction>(library, symbol);
    const char* value = function != nullptr ? function() : nullptr;
    return value != nullptr ? std::string(value) : std::string();
}

std::filesystem::path defaultShadowDirectory() {
    std::error_code error;
    const std::filesystem::path temporary = std::filesystem::temp_directory_path(error);
    return (error ? std::filesystem::current_path() : temporary) / "zenith_game_modules";
}
} // namespace

GameModule::~GameModule() {
    if (registriesShared()) {
        // Unloading now would leave the other owners holding unmapped code.
        std::cerr << "Game module '" << m_name
                  << "' is still referenced at unload; its library stays loaded until the process exits\n";
        m_library.leak();
        m_shadowCopy.keep();
    }
    // Release the module's functions while its code is still mapped.
    m_systems.reset();
    m_componentTypes.reset();
}

GameModuleResult GameModule::load(const std::filesystem::path& path, const GameModuleOptions& options,
                                  std::unique_ptr<GameModule>& loaded) {
    std::error_code error;
    const std::filesystem::path sourcePath = std::filesystem::absolute(path, error).lexically_normal();
    if (error || !std::filesystem::is_regular_file(sourcePath, error)) {
        return {.error = "Game module not found: " + path.string()};
    }

    std::unique_ptr<GameModule> module(new GameModule());
    module->m_sourcePath = sourcePath;
    const std::filesystem::path shadowDirectory =
        options.shadowDirectory.empty() ? defaultShadowDirectory() : options.shadowDirectory;
    if (ModuleShadowCopyResult result = ModuleShadowCopy::create(
            sourcePath, std::filesystem::absolute(shadowDirectory).lexically_normal(), module->m_shadowCopy);
        !result) {
        return {.error = result.error};
    }
    // The copy's dependencies are found beside the original library.
    if (SharedLibraryResult result = module->m_library.open(module->m_shadowCopy.library(), sourcePath.parent_path());
        !result) {
        return {.error = "Game module '" + sourcePath.string() + "' could not be loaded: " + result.error};
    }

    const auto apiVersion = findEntryPoint<ZenithModuleApiVersionFunction>(module->m_library, moduleApiVersionSymbol);
    const auto buildHash = findEntryPoint<ZenithModuleStringFunction>(module->m_library, moduleBuildHashSymbol);
    const auto registerModule = findEntryPoint<ZenithModuleRegisterFunction>(module->m_library, moduleRegisterSymbol);
    for (const auto& [present, symbol] : {std::pair{apiVersion != nullptr, moduleApiVersionSymbol},
                                          std::pair{buildHash != nullptr, moduleBuildHashSymbol},
                                          std::pair{registerModule != nullptr, moduleRegisterSymbol}}) {
        if (!present) {
            return {.error = "Game module '" + sourcePath.string() + "' does not export " + symbol +
                             "; define the module with ZENITH_GAME_MODULE"};
        }
    }

    module->m_name = moduleString(module->m_library, moduleNameSymbol);
    if (module->m_name.empty()) {
        module->m_name = sourcePath.stem().string();
    }
    module->m_version = moduleString(module->m_library, moduleVersionSymbol);
    const char* reportedBuildHash = buildHash();
    if (ModuleCompatibilityResult result = checkModuleCompatibility(
            module->m_name, apiVersion(), reportedBuildHash != nullptr ? reportedBuildHash : "");
        !result) {
        return {.error = result.error};
    }

    auto componentTypes = std::make_shared<ComponentTypeRegistry>(
        options.baseComponentTypes ? *options.baseComponentTypes : *builtInComponentTypes());
    auto systems = std::make_shared<SystemRegistry>();
    // Owned by the module before registration runs, so a failure releases the
    // module's functions before the library unloads.
    module->m_componentTypes = componentTypes;
    module->m_systems = systems;
    ModuleRegistrar registrar(*componentTypes, *systems);
    registerModule(registrar);
    componentTypes.reset();
    systems.reset();
    if (!registrar.firstError().empty()) {
        return {.error = "Game module '" + module->m_name + "' failed to register: " + registrar.firstError()};
    }
    module->m_moduleComponentKeys = registrar.componentKeys();

    loaded = std::move(module);
    return {.success = true};
}

const std::string& GameModule::name() const {
    return m_name;
}

const std::string& GameModule::version() const {
    return m_version;
}

const std::filesystem::path& GameModule::sourcePath() const {
    return m_sourcePath;
}

const std::filesystem::path& GameModule::loadedPath() const {
    return m_shadowCopy.library();
}

std::shared_ptr<const ComponentTypeRegistry> GameModule::componentTypes() const {
    return m_componentTypes;
}

std::span<const std::string> GameModule::moduleComponentKeys() const {
    return m_moduleComponentKeys;
}

std::shared_ptr<const SystemRegistry> GameModule::systems() const {
    return m_systems;
}

bool GameModule::registriesShared() const {
    return m_componentTypes.use_count() > 1 || m_systems.use_count() > 1;
}
