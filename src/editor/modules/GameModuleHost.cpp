#include "GameModuleHost.hpp"

#include "editor/document/EditorDocument.hpp"
#include "editor/operators/OperatorRegistry.hpp"
#include "editor/operators/OperatorRunner.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <algorithm>
#include <system_error>
#include <utility>

namespace {
// Wraps a module operator as an editor operator. The wrapper shares the
// module's functions, so it must be removed from the registry before the
// module unloads.
Operator editorOperator(EditorModuleOperator source) {
    const auto moduleOperator = std::make_shared<const EditorModuleOperator>(std::move(source));
    Operator op{.id = moduleOperator->id,
                .label = moduleOperator->label,
                .description = moduleOperator->description,
                .flags = moduleOperator->undoable ? OperatorFlags::Undo : OperatorFlags::None};
    op.poll = [moduleOperator](const OperatorContext& context) {
        if (!context.editMode()) {
            return false;
        }
        if (!moduleOperator->poll) {
            return true;
        }
        std::vector<SceneEntityId> selection = context.selection.ids();
        const EditorModuleContext moduleContext{
            .scene = context.editing.scene(), .assets = context.editing.assets(), .selection = selection};
        return moduleOperator->poll(moduleContext);
    };
    op.execute = [moduleOperator](OperatorContext& context, OperatorProperties&) {
        std::vector<SceneEntityId> selection = context.selection.ids();
        EditorModuleContext moduleContext{
            .scene = context.editing.scene(), .assets = context.editing.assets(), .selection = selection};
        const EditorModuleResult result = moduleOperator->execute(moduleContext);
        if (!result) {
            return OperatorResult::failed(result.error.empty() ? moduleOperator->label + " failed" : result.error);
        }
        if (selection != context.selection.ids()) {
            context.selection.assign(std::move(selection));
            context.selection.removeMissing(context.editing.scene());
        }
        return OperatorResult::finished();
    };
    return op;
}
} // namespace

// Records what the module's editor registration adds. Its virtual functions
// are how module code reaches the editor without linking it.
class GameModuleHost::Registrar final : public EditorModuleRegistrar {
  public:
    explicit Registrar(GameModuleHost& host)
        : m_host(host) {}

    EditorModuleResult addOperator(EditorModuleOperator op) override {
        if (!op.execute) {
            return fail("Module operator '" + op.id + "' needs an execute function");
        }
        const std::string id = op.id;
        if (const OperatorRegistrationResult result = m_host.m_operators.add(editorOperator(std::move(op))); !result) {
            return fail(result.error);
        }
        m_host.m_operatorIds.push_back(id);
        return {.success = true};
    }

    EditorModuleResult addPanel(EditorModulePanel panel) override {
        if (panel.title.empty() || !panel.draw) {
            return fail("Module panels need a title and a draw function");
        }
        const bool duplicate = std::ranges::any_of(
            m_host.m_panels, [&panel](const Panel& existing) { return existing.panel.title == panel.title; });
        if (duplicate) {
            return fail("Module panel '" + panel.title + "' is already registered");
        }
        // A panel open before a reload stays open.
        const bool visible =
            std::ranges::find(m_host.m_visiblePanelTitles, panel.title) != m_host.m_visiblePanelTitles.end();
        m_host.m_panels.push_back({.panel = std::move(panel), .visible = visible});
        return {.success = true};
    }

    std::vector<std::string> takeErrors() {
        return std::move(m_errors);
    }

  private:
    EditorModuleResult fail(std::string error) {
        m_errors.push_back(error);
        return {.error = std::move(error)};
    }

    GameModuleHost& m_host;
    std::vector<std::string> m_errors;
};

GameModuleHost::GameModuleHost(OperatorRegistry& operators, OperatorRunner& runner, GameModuleHostOptions options)
    : m_operators(operators)
    , m_runner(runner)
    , m_options(std::move(options)) {}

GameModuleHost::~GameModuleHost() {
    releaseEditorExtensions();
    m_scheduler = SystemScheduler{};
}

std::optional<GameModuleHost::FileStamp> GameModuleHost::stamp(const std::filesystem::path& path) {
    std::error_code error;
    FileStamp result;
    result.writeTime = std::filesystem::last_write_time(path, error);
    if (error) {
        return std::nullopt;
    }
    result.size = std::filesystem::file_size(path, error);
    if (error) {
        return std::nullopt;
    }
    return result;
}

GameModuleHostResult GameModuleHost::load(const std::filesystem::path& path, EditorDocument& document) {
    if (m_module) {
        return {.error = "A game module is already loaded; reload it instead"};
    }
    std::error_code error;
    m_sourcePath = std::filesystem::absolute(path, error).lexically_normal();
    if (error) {
        m_sourcePath = path;
    }
    return loadSource(document);
}

GameModuleHostResult GameModuleHost::loadSource(EditorDocument& document) {
    m_loadedStamp = stamp(m_sourcePath);
    m_pendingStamp.reset();
    m_editFixedAccumulator = 0.0f;

    std::unique_ptr<GameModule> loaded;
    if (const GameModuleResult result =
            GameModule::load(m_sourcePath, {.shadowDirectory = m_options.shadowDirectory}, loaded);
        !result) {
        m_status = GameModuleStatus::Failed;
        m_error = result.error;
        document.setComponentTypes(nullptr);
        return {.error = result.error};
    }
    m_module = std::move(loaded);
    m_status = GameModuleStatus::Loaded;
    m_error.clear();
    ++m_loadCount;
    m_scheduler = SystemScheduler(m_module->systems());
    m_scheduler.setSimulateMask(m_simulateMask);
    m_profile.clear();
    document.setComponentTypes(m_module->componentTypes());

    GameModuleHostResult result{.success = true};
    registerEditorExtensions(result.warnings);
    return result;
}

void GameModuleHost::registerEditorExtensions(std::vector<std::string>& warnings) {
    const auto registerEditor =
        reinterpret_cast<ZenithEditorModuleRegisterFunction>(m_module->findSymbol(editorModuleRegisterSymbol));
    if (registerEditor == nullptr) {
        return;
    }
    Registrar registrar(*this);
    registerEditor(registrar);
    for (std::string& error : registrar.takeErrors()) {
        warnings.push_back("Game module '" + m_module->name() + "' editor extension: " + std::move(error));
    }
}

GameModuleHostResult GameModuleHost::reload(Scene& scene, AssetManager& assets, EditorDocument& document) {
    if (m_sourcePath.empty()) {
        return {.error = "The project does not name a game module"};
    }
    std::string snapshot;
    if (const SceneDocumentResult saved = SceneDocument::serialize(scene, assets, document.options(), snapshot);
        !saved) {
        return {.error = "Could not save the scene before reloading the game module: " + saved.error};
    }

    std::vector<std::string> warnings;
    releaseModule(scene, document, warnings);
    GameModuleHostResult result = loadSource(document);
    result.warnings.insert(result.warnings.begin(), warnings.begin(), warnings.end());

    const SceneDocumentResult restored = SceneDocument::deserialize(snapshot, assets, document.options(), scene);
    if (restored) {
        return result;
    }
    // The new module could not read its old components. The built-in types
    // keep every module component as preserved JSON, so nothing is lost.
    SceneDocumentOptions builtInOptions = document.options();
    builtInOptions.componentTypes = nullptr;
    if (const SceneDocumentResult preserved = SceneDocument::deserialize(snapshot, assets, builtInOptions, scene);
        !preserved) {
        result.success = false;
        result.error = "The scene could not be restored after reloading the game module: " + preserved.error;
        return result;
    }
    result.warnings.push_back("The reloaded game module could not read the scene (" + restored.error +
                              "); its components are kept as unedited data until the next reload");
    return result;
}

void GameModuleHost::unload(Scene& scene, EditorDocument& document) {
    std::vector<std::string> ignored;
    releaseModule(scene, document, ignored);
    m_sourcePath.clear();
    m_status = GameModuleStatus::None;
    m_error.clear();
    m_loadedStamp.reset();
    m_pendingStamp.reset();
}

void GameModuleHost::releaseModule(Scene& scene, EditorDocument& document, std::vector<std::string>& warnings) {
    releaseEditorExtensions();
    m_runner.forgetLast();
    m_scheduler = SystemScheduler{};
    m_profile.clear();
    document.setComponentTypes(nullptr);
    // Replacing the registry destroys the storage module code created while
    // that code is still loaded.
    scene.replaceWith(Scene{});
    if (m_module && m_module->registriesShared()) {
        warnings.push_back("Game module '" + m_module->name() +
                           "' is still referenced, so its old library stays loaded until the editor exits");
    }
    m_module.reset();
}

void GameModuleHost::releaseEditorExtensions() {
    for (const std::string& id : m_operatorIds) {
        m_operators.remove(id);
    }
    m_operatorIds.clear();
    // A module that failed to load has no panels; keep what was open before.
    if (!m_panels.empty()) {
        m_visiblePanelTitles.clear();
        for (const Panel& panel : m_panels) {
            if (panel.visible) {
                m_visiblePanelTitles.push_back(panel.panel.title);
            }
        }
    }
    m_panels.clear();
}

void GameModuleHost::runSystems(SystemPhase phase, Scene& scene, ApplicationContext* application, SimulationMode mode,
                                float deltaSeconds, const InputState* input) {
    if (!m_module) {
        return;
    }
    if (mode == SimulationMode::Simulate) {
        input = nullptr;
    }
    m_scheduler.run(
        phase, {.scene = scene, .application = application, .mode = mode, .deltaSeconds = deltaSeconds, .input = input},
        &m_profile);
}

std::span<const SystemDescriptor* const> GameModuleHost::systems(SystemPhase phase) const {
    return m_scheduler.systems(phase);
}

const SystemProfile& GameModuleHost::systemProfile() const {
    return m_profile;
}

void GameModuleHost::finishSystemFrame() {
    m_profile.finishFrame();
}

void GameModuleHost::resetSystemTimingMaxima() {
    m_profile.resetMaxima();
}

const SimulateSystemMask& GameModuleHost::simulateMask() const {
    return m_simulateMask;
}

void GameModuleHost::setSimulateMask(SimulateSystemMask mask) {
    m_simulateMask = std::move(mask);
    m_scheduler.setSimulateMask(m_simulateMask);
}

void GameModuleHost::setSimulateSystemEnabled(std::string name, bool enabled) {
    m_simulateMask.set(std::move(name), enabled);
    m_scheduler.setSimulateMask(m_simulateMask);
}

void GameModuleHost::runEditFrame(Scene& scene, float deltaSeconds, float fixedStepSeconds) {
    if (!m_module) {
        return;
    }
    runSystems(SystemPhase::Update, scene, nullptr, SimulationMode::Edit, deltaSeconds);
    if (fixedStepSeconds <= 0.0f) {
        return;
    }
    m_editFixedAccumulator += deltaSeconds;
    uint32_t steps = 0;
    while (m_editFixedAccumulator >= fixedStepSeconds && steps < m_options.maximumEditFixedSteps) {
        runSystems(SystemPhase::FixedUpdate, scene, nullptr, SimulationMode::Edit, fixedStepSeconds);
        m_editFixedAccumulator -= fixedStepSeconds;
        ++steps;
    }
    // A long frame does not leave a backlog of steps for later frames.
    m_editFixedAccumulator = std::min(m_editFixedAccumulator, fixedStepSeconds);
}

bool GameModuleHost::pollSourceChange(std::chrono::steady_clock::time_point now) {
    if (m_sourcePath.empty() ||
        (m_lastCheck != std::chrono::steady_clock::time_point{} && now - m_lastCheck < m_options.checkInterval)) {
        return false;
    }
    m_lastCheck = now;
    // A missing file is usually a linker between deleting and writing it.
    const std::optional<FileStamp> current = stamp(m_sourcePath);
    if (!current || current == m_loadedStamp) {
        m_pendingStamp.reset();
        return false;
    }
    if (current != m_pendingStamp) {
        m_pendingStamp = current;
        m_pendingSince = now;
        return false;
    }
    if (now - m_pendingSince < m_options.settleTime) {
        return false;
    }
    m_pendingStamp.reset();
    return true;
}

bool GameModuleHost::sourceChanged() const {
    return !m_sourcePath.empty() && stamp(m_sourcePath) != m_loadedStamp;
}

GameModuleStatus GameModuleHost::status() const {
    return m_status;
}

const std::filesystem::path& GameModuleHost::sourcePath() const {
    return m_sourcePath;
}

const GameModule* GameModuleHost::module() const {
    return m_module.get();
}

const std::string& GameModuleHost::error() const {
    return m_error;
}

uint32_t GameModuleHost::loadCount() const {
    return m_loadCount;
}

std::span<const std::string> GameModuleHost::operatorIds() const {
    return m_operatorIds;
}

std::span<GameModuleHost::Panel> GameModuleHost::panels() {
    return m_panels;
}
