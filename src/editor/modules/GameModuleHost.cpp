#include "GameModuleHost.hpp"

#include "editor/document/EditorDocument.hpp"
#include "editor/interaction/ComponentEditorOverrides.hpp"
#include "editor/menus/EditorMenus.hpp"
#include "editor/operators/OperatorRegistry.hpp"
#include "editor/operators/OperatorRunner.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/panels/ViewportRegistry.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/behaviour/BehaviourRegistry.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <algorithm>
#include <string_view>
#include <system_error>
#include <utility>

// Modules name the editor's menus through EditorModuleApi, which cannot
// include the editor's headers.
static_assert(std::string_view(EditorModuleMenus::add) == addMenuId);
static_assert(std::string_view(EditorModuleMenus::addComponent) == addComponentMenuId);

void drawModulePanelContents(EditorContext& context, const std::function<void(EditorModulePanelContext&)>& draw) {
    // Copied: an operator the panel invokes may change the selection.
    const std::vector<SceneEntityId> selection = context.selection.ids();
    EditorModulePanelContext panelContext{
        .scene = context.app.scene,
        .assets = context.app.assets,
        .selection = selection,
        .editMode = context.editMode(),
        .invokeOperator = [&context](std::string_view operatorId) {
            return static_cast<bool>(context.actions.invokeOperator(context.app, operatorId, {}, KeymapRegion::Global));
        }};
    draw(panelContext);
}

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

// Wraps a module sidebar tab for every viewport. The wrapper holds the
// module's draw function, so it must be removed before the module unloads.
ViewportSidebarTab viewportSidebarTab(EditorModuleSidebarTab source) {
    return {.title = std::move(source.title), .draw = [draw = std::move(source.draw)](EditorContext& context) {
                drawModulePanelContents(context, draw);
            }};
}

// Wraps a module inspector override as a component editor; removed from the
// overrides before the module unloads.
ComponentEditorDraw componentEditor(EditorModuleInspectorOverride source) {
    return [key = source.componentKey, draw = std::move(source.draw)](EditorContext& context, Entity entity,
                                                                      void* component) {
        EditorModuleInspectorContext inspectorContext{.scene = context.app.scene,
                                                      .assets = context.app.assets,
                                                      .entity = context.app.scene.id(entity),
                                                      .componentKey = key,
                                                      .component = component,
                                                      .editMode = context.editMode()};
        return draw(inspectorContext);
    };
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
        if (const OperatorRegistrationResult result = m_host.m_services.operators.add(editorOperator(std::move(op)));
            !result) {
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

    EditorModuleResult addSidebarTab(EditorModuleSidebarTab tab) override {
        const std::string title = tab.title;
        if (const ViewportSidebarTabResult result =
                m_host.m_services.viewports.addSidebarTab(viewportSidebarTab(std::move(tab)));
            !result) {
            return fail("Module sidebar tab: " + result.error);
        }
        m_host.m_sidebarTabTitles.push_back(title);
        return {.success = true};
    }

    EditorModuleResult addInspectorOverride(EditorModuleInspectorOverride inspector) override {
        const std::string key = inspector.componentKey;
        const std::shared_ptr<const ComponentTypeRegistry> types = m_host.m_module->componentTypes();
        if (types == nullptr || types->find(key) == nullptr) {
            return fail("Inspector override for '" + key + "': no registered component type has that key");
        }
        if (!inspector.draw) {
            return fail("Inspector override for '" + key + "' needs a draw function");
        }
        if (const ComponentEditorOverrideResult result =
                m_host.m_services.componentEditors.add(key, componentEditor(std::move(inspector)));
            !result) {
            return fail(result.error);
        }
        m_host.m_inspectorOverrideKeys.push_back(key);
        return {.success = true};
    }

    EditorModuleResult addMenu(EditorModuleMenu menu) override {
        if (const MenuResult result = m_host.m_services.menus.addMenu(
                m_host.m_menuOwner, {.id = menu.id, .title = menu.title, .layout = MenuLayout::Submenus});
            !result) {
            return fail("Module menu: " + result.error);
        }
        m_host.m_menus.push_back(std::move(menu));
        return {.success = true};
    }

    EditorModuleResult addMenuEntry(EditorModuleMenuEntry entry) override {
        const std::string menu = entry.menu;
        MenuEntry menuEntry{.label = std::move(entry.label),
                            .operatorId = std::move(entry.operatorId),
                            .properties = std::move(entry.properties),
                            .submenu = std::move(entry.submenu),
                            .icon = std::nullopt,
                            .category = std::move(entry.category)};
        if (const MenuResult result = m_host.m_services.menus.addEntry(m_host.m_menuOwner, menu, std::move(menuEntry));
            !result) {
            return fail("Module menu entry: " + result.error);
        }
        ++m_host.m_menuEntryCount;
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

GameModuleHost::GameModuleHost(GameModuleEditorServices services, GameModuleHostOptions options)
    : m_services(services)
    , m_menuOwner(services.menus.createOwner())
    , m_options(std::move(options)) {}

GameModuleHost::~GameModuleHost() {
    stopGameplay();
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
    m_inputActions = *m_module->inputActions();
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
    const auto apiVersion =
        reinterpret_cast<ZenithEditorModuleApiVersionFunction>(m_module->findSymbol(editorModuleApiVersionSymbol));
    const uint32_t moduleVersion = apiVersion != nullptr ? apiVersion() : 1;
    if (moduleVersion != editorModuleApiVersion) {
        warnings.push_back("Game module '" + m_module->name() + "' was built against editor module API version " +
                           std::to_string(moduleVersion) + ", but this editor uses version " +
                           std::to_string(editorModuleApiVersion) +
                           "; its editor extensions are not loaded until it is rebuilt");
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
    stopGameplay();
    releaseEditorExtensions();
    m_services.runner.forgetLast();
    m_scheduler = SystemScheduler{};
    m_inputActions = {};
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
        m_services.operators.remove(id);
    }
    m_operatorIds.clear();
    for (const std::string& title : m_sidebarTabTitles) {
        m_services.viewports.removeSidebarTab(title);
    }
    m_sidebarTabTitles.clear();
    for (const std::string& key : m_inspectorOverrideKeys) {
        m_services.componentEditors.remove(key);
    }
    m_inspectorOverrideKeys.clear();
    // Menus hold no module code, but entries naming its operators and menus
    // must not outlive them.
    m_services.menus.removeOwner(m_menuOwner);
    m_menus.clear();
    m_menuEntryCount = 0;
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
                                float deltaSeconds, const InputState* input, GameUiContext* ui) {
    if (!m_module) {
        return;
    }
    if (mode != SimulationMode::Play) {
        input = nullptr;
    }
    std::optional<InputActionState> actions;
    if (input != nullptr) {
        actions = m_inputActions.evaluate(*input);
    }
    runGameplayPhase(m_scheduler, phase,
                     {.scene = scene,
                      .application = application,
                      .mode = mode,
                      .deltaSeconds = deltaSeconds,
                      .input = input,
                      .logger = m_logger,
                      .services = mode == SimulationMode::Edit ? nullptr : m_gameServices.get(),
                      .actions = actions ? &*actions : nullptr,
                      .ui = phase == SystemPhase::Ui ? ui : nullptr,
                      .behaviours = m_module->behaviours().get()},
                     &m_profile);
}

void GameModuleHost::setLogger(Logger& logger) {
    m_logger = &logger;
}

void GameModuleHost::startGameplay(AssetManager& assets, SceneDocumentOptions documents, DebugDraw& debugDraw) {
    m_gameServices.reset();
    m_gameServices = std::make_unique<GameServices>(assets, std::move(documents), debugDraw, m_logger,
                                                    m_module ? m_module->behaviours() : nullptr);
}

void GameModuleHost::stopGameplay() {
    m_gameServices.reset();
}

GameServices* GameModuleHost::gameServices() {
    return m_gameServices.get();
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

std::optional<EditorSourceLocation> GameModuleHost::behaviourSource(std::string_view componentKey) const {
    if (m_module == nullptr) {
        return std::nullopt;
    }
    const BehaviourType* behaviour = m_module->behaviours()->find(componentKey);
    if (behaviour == nullptr || behaviour->source.file.empty()) {
        return std::nullopt;
    }
    // std::source_location's file name is the compiler's UTF-8 path.
    const std::string& file = behaviour->source.file;
    return EditorSourceLocation{.file = std::filesystem::path(std::u8string(file.begin(), file.end())),
                                .line = behaviour->source.line};
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

std::span<const std::string> GameModuleHost::sidebarTabTitles() const {
    return m_sidebarTabTitles;
}

std::span<const std::string> GameModuleHost::inspectorOverrideKeys() const {
    return m_inspectorOverrideKeys;
}

std::span<const EditorModuleMenu> GameModuleHost::menus() const {
    return m_menus;
}

GameModuleHost::ExtensionCounts GameModuleHost::extensionCounts() const {
    return {.operators = m_operatorIds.size(),
            .panels = m_panels.size(),
            .sidebarTabs = m_sidebarTabTitles.size(),
            .inspectorOverrides = m_inspectorOverrideKeys.size(),
            .menus = m_menus.size(),
            .menuEntries = m_menuEntryCount};
}

std::vector<std::string> GameModuleHost::missingExtensions() const {
    std::vector<std::string> missing;
    for (const std::string& id : m_operatorIds) {
        if (m_services.operators.find(id) == nullptr) {
            missing.push_back("operator '" + id + "'");
        }
    }
    const std::span<const ViewportSidebarTab> tabs = m_services.viewports.sidebarTabs();
    for (const std::string& title : m_sidebarTabTitles) {
        if (std::ranges::none_of(tabs, [&title](const ViewportSidebarTab& tab) { return tab.title == title; })) {
            missing.push_back("sidebar tab '" + title + "'");
        }
    }
    for (const std::string& key : m_inspectorOverrideKeys) {
        if (m_services.componentEditors.find(key) == nullptr) {
            missing.push_back("inspector override '" + key + "'");
        }
    }
    for (const EditorModuleMenu& menu : m_menus) {
        if (m_services.menus.find(menu.id) == nullptr) {
            missing.push_back("menu '" + menu.id + "'");
        }
    }
    return missing;
}
