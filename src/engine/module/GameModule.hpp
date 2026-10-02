#pragma once

#include "ModuleShadowCopy.hpp"
#include "SharedLibrary.hpp"
#include "SystemScheduler.hpp"

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

class ComponentTypeRegistry;

struct GameModuleResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

struct GameModuleOptions {
    // The component types the module extends; a copy is made, so the
    // original is never modified. Null means the engine's built-in types.
    std::shared_ptr<const ComponentTypeRegistry> baseComponentTypes;
    // Where the shadow copy is written. Empty means
    // <temporary directory>/zenith_game_modules.
    std::filesystem::path shadowDirectory;
};

// A game module loaded from a shadow copy, and what it registered: a component
// type registry holding the base types plus the module's own, and the
// module's systems.
//
// Lifetime: the registries hold functions whose code lives in the module, and
// EnTT storage for a component type is instantiated by whichever binary first
// touches the type. Before a GameModule is destroyed the host must therefore
// destroy every scene a module component (or a module system) has touched,
// every SceneDocumentOptions, SystemScheduler, or copy that shares
// componentTypes() or systems(), and every std::function copied out of them.
// Serialize module components out first if they must survive a reload. If the
// registries are still shared when the module is destroyed, the library is
// left loaded (and its shadow copy on disk) rather than unloaded under live
// references.
class GameModule {
  public:
    ~GameModule();

    GameModule(const GameModule&) = delete;
    GameModule& operator=(const GameModule&) = delete;

    // Copies the library at path to a shadow file, loads the copy, checks its
    // API version and build hash, and runs its registration. On failure
    // nothing stays loaded and loaded is left unchanged.
    static GameModuleResult load(const std::filesystem::path& path, const GameModuleOptions& options,
                                 std::unique_ptr<GameModule>& loaded);

    // As reported by the module, for diagnostics.
    const std::string& name() const;
    const std::string& version() const;
    // The library that was requested and the shadow copy actually loaded.
    const std::filesystem::path& sourcePath() const;
    const std::filesystem::path& loadedPath() const;

    // Base types plus the module's; never null.
    std::shared_ptr<const ComponentTypeRegistry> componentTypes() const;
    // Keys of the component types the module added, in registration order.
    std::span<const std::string> moduleComponentKeys() const;
    // The module's systems; never null.
    std::shared_ptr<const SystemRegistry> systems() const;

    // Whether anything outside this object still shares the registries.
    bool registriesShared() const;

    // The address of an optional entry point the module exports beyond the
    // module API (for example a host-specific extension), or null when it
    // exports none. Like the registries, the address is valid only while this
    // object lives.
    void* findSymbol(const char* name) const;

  private:
    GameModule() = default;

    // Declaration order is destruction order in reverse: the registries (module
    // code) go first, then the library, then the shadow files.
    ModuleShadowCopy m_shadowCopy;
    SharedLibrary m_library;
    std::filesystem::path m_sourcePath;
    std::string m_name;
    std::string m_version;
    std::vector<std::string> m_moduleComponentKeys;
    std::shared_ptr<const ComponentTypeRegistry> m_componentTypes;
    std::shared_ptr<const SystemRegistry> m_systems;
};
