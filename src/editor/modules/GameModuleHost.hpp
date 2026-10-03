#pragma once

#include "editor/modules/EditorModuleApi.hpp"
#include "engine/module/GameModule.hpp"
#include "engine/module/SystemScheduler.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

class AssetManager;
class EditorDocument;
class OperatorRegistry;
class OperatorRunner;
class Scene;
struct ApplicationContext;

enum class GameModuleStatus : uint8_t {
    // The project names no game module.
    None,
    Loaded,
    // The last load or reload failed; the editor runs with the built-in
    // component types and keeps module components as preserved JSON.
    Failed,
};

struct GameModuleHostOptions {
    // Where shadow copies are written; empty uses GameModule's default.
    std::filesystem::path shadowDirectory;
    // How long the module file must stay unchanged after a change before
    // pollSourceChange() reports it, so a linker that is still writing the
    // library is not interrupted.
    std::chrono::milliseconds settleTime{750};
    // Minimum time between file checks in pollSourceChange().
    std::chrono::milliseconds checkInterval{250};
    // At most this many edit-mode fixed updates run per frame.
    uint32_t maximumEditFixedSteps{4};
};

// What a reload or load leaves for the session to report, beyond success.
struct GameModuleHostResult {
    bool success{false};
    std::string error;
    // Problems that did not stop the operation, for example an editor
    // extension the module failed to register.
    std::vector<std::string> warnings;

    explicit operator bool() const {
        return success;
    }
};

// Hosts a project's game module in the editor: loads it, gives the document
// its merged component registry, runs its systems, registers its editor
// extensions (operators and panels), and hot reloads it while preserving the
// edited scene.
//
// Lifetime: everything that can hold module code is released, in this order,
// before the module is unloaded: the editor operators it registered (and the
// runner's remembered last operator), its panels, the system scheduler, the
// document's component registry, and the scene's whole EnTT registry
// (replaced, not cleared, because module code created the storage for module
// component types). The caller must release any other scene or document
// options sharing the registry first (Play mode's retained scene, additive
// layers). The operator registry and runner must outlive the host.
class GameModuleHost {
  public:
    GameModuleHost(OperatorRegistry& operators, OperatorRunner& runner, GameModuleHostOptions options = {});
    // Removes the module's editor extensions. The module is unloaded only if
    // nothing else shares its registries; call unload() first to release the
    // scene and document.
    ~GameModuleHost();

    GameModuleHost(const GameModuleHost&) = delete;
    GameModuleHost& operator=(const GameModuleHost&) = delete;

    // Loads the module at path and gives document its registry. Call before
    // the scene holds module data. On failure the host stays Failed with the
    // error, the document keeps the built-in types, and reload() retries
    // the same path later.
    GameModuleHostResult load(const std::filesystem::path& path, EditorDocument& document);
    // Serializes scene with the current registry, unloads the module, loads a
    // fresh copy of the source library, and restores scene with the new
    // registry. Entity ids are kept, so selections and undo history (which
    // are stored as ids and JSON) stay valid. If the new module fails to load,
    // the scene is restored with the built-in types: module components become
    // PreservedComponentsComponent JSON, which a later successful reload
    // turns back into components. Edit mode only; the caller stops Play mode
    // and clears physics and audio state that refers to scene entities.
    GameModuleHostResult reload(Scene& scene, AssetManager& assets, EditorDocument& document);
    // Releases everything holding module code, replacing scene with an empty
    // one, and unloads the module. The source path is forgotten.
    void unload(Scene& scene, EditorDocument& document);

    // Runs the module's systems of phase that run in mode, recording their
    // CPU time in systemProfile(). application is null in Edit mode; input
    // is the frame's device state in Play mode and null when systems must
    // not react to input. Simulate never passes input to systems, whatever
    // the caller gives, and runs only the systems simulateMask() enables.
    void runSystems(SystemPhase phase, Scene& scene, ApplicationContext* application, SimulationMode mode,
                    float deltaSeconds, const InputState* input = nullptr);
    // Edit mode, once per frame: runs Update systems for deltaSeconds, then
    // FixedUpdate systems once per elapsed fixedStepSeconds (the simulation
    // itself does not step in Edit mode, so the host keeps the fixed clock).
    // Edit systems change the document directly, outside undo and the dirty
    // flag; a system that changes serialized data every frame also keeps
    // Adjust Last Operation from matching the operator's result. Edit systems
    // should therefore change only derived or transient state.
    void runEditFrame(Scene& scene, float deltaSeconds, float fixedStepSeconds);

    // Every module system of phase in run order, whatever its RunIn flags;
    // empty without a loaded module. Valid until the module is released.
    std::span<const SystemDescriptor* const> systems(SystemPhase phase) const;
    // Per-system CPU timings of the systems that ran. Call finishSystemFrame
    // once per editor frame, after the frame's systems ran; the profile
    // starts over whenever the module loads or reloads.
    const SystemProfile& systemProfile() const;
    void finishSystemFrame();
    void resetSystemTimingMaxima();
    // Which Play systems Simulate runs. Kept across module reloads (by
    // system name) and while no module is loaded.
    const SimulateSystemMask& simulateMask() const;
    void setSimulateMask(SimulateSystemMask mask);
    void setSimulateSystemEnabled(std::string name, bool enabled);

    // Checks the module file at most every checkInterval and returns true
    // once, when it changed since the last load and then stayed unchanged
    // for settleTime.
    bool pollSourceChange(std::chrono::steady_clock::time_point now);
    // Whether the module file differs from the one last loaded (or attempted).
    bool sourceChanged() const;

    GameModuleStatus status() const;
    // The library the project names; empty when status() is None.
    const std::filesystem::path& sourcePath() const;
    // Null unless status() is Loaded.
    const GameModule* module() const;
    // The last load or reload failure; empty while Loaded.
    const std::string& error() const;
    // How many times the module has loaded successfully.
    uint32_t loadCount() const;
    // Ids of the operators the module registered, in registration order.
    std::span<const std::string> operatorIds() const;

    struct Panel {
        EditorModulePanel panel;
        bool visible{false};
    };

    std::span<Panel> panels();

  private:
    class Registrar;

    struct FileStamp {
        std::filesystem::file_time_type writeTime{};
        uintmax_t size{0};

        bool operator==(const FileStamp&) const = default;
    };

    static std::optional<FileStamp> stamp(const std::filesystem::path& path);
    GameModuleHostResult loadSource(EditorDocument& document);
    void registerEditorExtensions(std::vector<std::string>& warnings);
    void releaseModule(Scene& scene, EditorDocument& document, std::vector<std::string>& warnings);
    void releaseEditorExtensions();

    OperatorRegistry& m_operators;
    OperatorRunner& m_runner;
    GameModuleHostOptions m_options;
    std::filesystem::path m_sourcePath;
    GameModuleStatus m_status{GameModuleStatus::None};
    std::string m_error;
    uint32_t m_loadCount{0};

    std::optional<FileStamp> m_loadedStamp;
    std::optional<FileStamp> m_pendingStamp;
    std::chrono::steady_clock::time_point m_pendingSince{};
    std::chrono::steady_clock::time_point m_lastCheck{};

    float m_editFixedAccumulator{0.0f};
    // Names and timings only; holds no module code.
    SystemProfile m_profile;
    // Copied into every scheduler the host builds.
    SimulateSystemMask m_simulateMask;

    // Declared before everything holding module code, so it is destroyed
    // after them.
    std::unique_ptr<GameModule> m_module;
    SystemScheduler m_scheduler;
    std::vector<std::string> m_operatorIds;
    std::vector<Panel> m_panels;
    // Titles of the panels open when the module was last released, reopened
    // when the reloaded module registers them again.
    std::vector<std::string> m_visiblePanelTitles;
};
