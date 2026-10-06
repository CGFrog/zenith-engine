// Prefab mode: a prefab opened as a document of its own over the parked scene.
// The first half drives DocumentStack headless, as the session holds it (its
// document, history, and selection, and the prefab operators reporting file
// writes to a ledger). The second half runs the whole editor session
// headlessly (as EditorSessionTests does) for what only the session decides:
// the unsaved-changes prompts and their chaining, Play and module reloads
// waiting for the scene, and recovering an autosaved prefab edit.

#include "HeadlessImGui.hpp"

#include "editor/application/DocumentStack.hpp"
#include "editor/application/EditorApplication.hpp"
#include "editor/application/EditorDiagnostics.hpp"
#include "editor/application/PrefabInstanceRefresh.hpp"
#include "editor/application/SceneEditing.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/document/PrefabAuthoring.hpp"
#include "editor/document/PrefabWriteCommand.hpp"
#include "editor/operators/Keymap.hpp"
#include "editor/operators/OperatorRunner.hpp"
#include "editor/operators/PrefabOperators.hpp"
#include "engine/application/Application.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/Prefab.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

fs::path freshDirectory(const fs::path& root, const std::string& name) {
    const fs::path directory = root / name;
    std::error_code error;
    fs::remove_all(directory, error);
    fs::create_directories(directory);
    return directory;
}

std::string readText(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

Entity findNamed(const Scene& scene, std::string_view name) {
    const auto view = scene.registry().view<const NameComponent>();
    for (const Entity entity : view) {
        if (view.get<const NameComponent>(entity).name == name) {
            return entity;
        }
    }
    return entt::null;
}

size_t entityCount(const Scene& scene) {
    return scene.registry().view<const SceneEntityIdComponent>().size();
}

// The instance roots of reference in scene, nested ones included.
std::vector<Entity> instancesOf(const Scene& scene, const std::string& reference) {
    std::vector<Entity> roots;
    for (const auto [entity, instance] : scene.registry().view<const PrefabInstanceComponent>().each()) {
        if (instance.prefab == reference) {
            roots.push_back(entity);
        }
    }
    return roots;
}

// Authors lamp.prefab (a point light with a Shade child) and shelf.prefab (a
// Shelf holding one lamp instance) under root.
void authorPrefabs(const fs::path& root) {
    AssetManager assets;
    const SceneDocumentOptions options{.assetRoot = root};
    PrefabAuthoring authoring(assets, options);
    Scene staging;
    const Entity lamp = staging.createEntity("Lamp");
    staging.registry().emplace<PointLightComponent>(lamp, glm::vec3(1.0f), 10.0f, 4.0f);
    staging.setParent(staging.createEntity("Shade"), lamp);
    expect(authoring.create(staging, lamp, "lamp.prefab").success, "fixture: the lamp prefab should be created");
    Scene shelfStaging;
    const Entity shelf = shelfStaging.createEntity("Shelf");
    Entity nested = entt::null;
    expect(authoring.instantiate(shelfStaging, "lamp.prefab", shelf, nested).success &&
               authoring.create(shelfStaging, shelf, "shelf.prefab").success,
           "fixture: the shelf prefab should be created");
}

// The session's documents, headless: the active document, its history and
// selection, the prefab operators, and the write ledger their hooks fill.
struct Fixture {
    fs::path root;
    AssetManager assets;
    Scene scene;
    EditorDocument document;
    CommandHistory history;
    EditorDiagnostics diagnostics;
    Selection selection;
    PrefabWriteLedger ledger;
    OperatorRegistry registry;
    OperatorRunner runner{registry, history};
    DocumentStack stack;

    explicit Fixture(fs::path assetRoot)
        : root(std::move(assetRoot))
        , document(root) {
        expect(registerPrefabOperators(registry, {.writeHooks = hooks()}).success,
               "fixture: the prefab operators should register");
    }

    PrefabWriteHooks hooks() {
        return {.fileWritten = [this](const fs::path& file,
                                      const std::optional<std::string>& bytes) { ledger.record(file, bytes); },
                .fileRead =
                    [this](const fs::path& file, const std::optional<std::string>& bytes) {
                        if (ledger.recorded(file) == nullptr) {
                            ledger.record(file, bytes);
                        }
                    }};
    }

    DocumentBinding active() {
        return {.scene = scene, .document = document, .history = history, .selection = selection};
    }

    SceneEditing editing() {
        return SceneEditing(scene, assets, document, history, diagnostics);
    }

    OperatorResult invoke(std::string_view id, std::string_view properties = {}) {
        OperatorContext context{.editing = editing(), .selection = selection};
        return runner.invokeWithJson(id, context, properties);
    }

    void edit(const std::function<void()>& change) {
        editing().perform("Edit", {}, [&change] {
            change();
            return SceneResult{.success = true};
        });
    }

    PointLightComponent& light(Scene& in, SceneEntityId id) {
        return in.registry().get<PointLightComponent>(in.findEntity(id));
    }

    std::string snapshot() {
        std::string text;
        expect(SceneDocument::serialize(scene, assets, document.options(), text).success,
               "fixture: the scene should serialize");
        return text;
    }

    PrefabResult push(const std::string& reference) {
        return stack.push(active(), assets, reference);
    }

    DocumentStackPopResult pop() {
        return stack.pop(active(), assets, diagnostics, hooks());
    }

    PrefabResult save() {
        return stack.save(active(), assets, diagnostics, hooks());
    }

    // Two lamp instances in the level, the second with its own range.
    std::pair<SceneEntityId, SceneEntityId> placeLamps() {
        expect(invoke("prefab.instantiate", R"({"asset":"lamp.prefab"})").status == OperatorStatus::Finished,
               "fixture: the first lamp should instantiate");
        const SceneEntityId first = *selection.active();
        expect(invoke("prefab.instantiate", R"({"asset":"lamp.prefab"})").status == OperatorStatus::Finished,
               "fixture: the second lamp should instantiate");
        const SceneEntityId second = *selection.active();
        edit([&] { light(scene, second).range = 9.0f; });
        return {first, second};
    }
};

void testEnterAndExitRestoreTheScene(const fs::path& base) {
    Fixture fixture(freshDirectory(base, "restore"));
    authorPrefabs(fixture.root);
    const auto [first, second] = fixture.placeLamps();
    fixture.selection.assign({second, first});
    const std::string level = fixture.snapshot();
    const size_t steps = fixture.history.size();
    const bool dirty = fixture.document.dirty();
    const uint64_t revision = fixture.history.revision();

    expect(fixture.push("lamp.prefab").success && fixture.stack.depth() == 1, "restore: the lamp should open");
    const PrefabEditTarget* prefab = fixture.stack.activePrefab();
    expect(prefab != nullptr && prefab->displayName == "Lamp" && prefab->reference == "lamp.prefab",
           "restore: the open prefab should be named and referenced");
    expect(entityCount(fixture.scene) == 2 && findNamed(fixture.scene, "Shade") != entt::null,
           "restore: the active scene should hold only the prefab");
    expect(!fixture.history.canUndo() && !fixture.document.dirty() && fixture.history.revision() > revision,
           "restore: the prefab starts with a clean history of its own");
    expect(prefab != nullptr && fixture.selection.active() == prefab->root, "restore: the prefab root is selected");
    expect(fixture.stack.parkedScene() != nullptr && fixture.stack.parkedScene()->history.size() == steps,
           "restore: the level's history waits parked");
    expect(!fixture.push("lamp.prefab").success && fixture.stack.depth() == 1,
           "restore: a prefab already open cannot be opened again");
    expect(!fixture.push("missing.prefab").success && fixture.stack.depth() == 1 && entityCount(fixture.scene) == 2,
           "restore: a prefab that cannot open changes nothing");

    const DocumentStackPopResult popped = fixture.pop();
    expect(popped.refresh.success && fixture.stack.empty(), "restore: back should return to the level");
    expect(fixture.snapshot() == level, "restore: the level scene should come back exactly");
    expect(fixture.selection.ids() == std::vector<SceneEntityId>{second, first},
           "restore: the level's selection should come back");
    expect(fixture.history.size() == steps && fixture.history.canUndo() && fixture.document.dirty() == dirty,
           "restore: the level's history and dirty state should come back");
    expect(fixture.history.revision() > revision, "restore: the revision should only grow across the switch");
}

void testPrefabHistoryIsItsOwn(const fs::path& base) {
    Fixture fixture(freshDirectory(base, "history"));
    authorPrefabs(fixture.root);
    fixture.placeLamps();
    const size_t levelSteps = fixture.history.size();
    expect(fixture.push("lamp.prefab").success, "history: the lamp should open");
    const SceneEntityId root = fixture.stack.activePrefab()->root;
    fixture.edit([&] { fixture.light(fixture.scene, root).intensity = 20.0f; });
    expect(fixture.history.size() == 1 && fixture.document.dirty(), "history: the prefab edit is the prefab's step");
    expect(fixture.stack.parkedScene()->history.size() == levelSteps, "history: the level's history is untouched");
    fixture.editing().undo();
    fixture.editing().undo();
    expect(fixture.light(fixture.scene, root).intensity == 10.0f && !fixture.history.canUndo() &&
               fixture.stack.parkedScene()->history.size() == levelSteps,
           "history: undo in the prefab stays inside the prefab");
    fixture.pop();
    expect(fixture.history.size() == levelSteps, "history: leaving without saving records nothing in the level");
}

void testSaveAndBackRefreshesInstances(const fs::path& base) {
    Fixture fixture(freshDirectory(base, "save"));
    authorPrefabs(fixture.root);
    const fs::path file = fixture.root / "lamp.prefab";
    const std::string original = readText(file);
    const auto [first, second] = fixture.placeLamps();
    const size_t levelSteps = fixture.history.size();

    expect(fixture.push("lamp.prefab").success, "save: the lamp should open");
    const SceneEntityId root = fixture.stack.activePrefab()->root;
    fixture.edit([&] {
        fixture.light(fixture.scene, root).intensity = 30.0f;
        fixture.light(fixture.scene, root).range = 6.0f;
    });
    expect(fixture.save().success && !fixture.document.dirty(), "save: saving writes the prefab and cleans it");
    const std::string saved = readText(file);
    expect(saved != original && fixture.ledger.matches(file, saved), "save: the file changes and the ledger knows");
    PrefabFileChange change;
    expect(classifyPrefabFileChange(fixture.ledger, file, fixture.root, change).success && !change.external,
           "save: the watcher's report of the save is not an outside change");
    expect(fixture.light(fixture.stack.parkedScene()->scene, first).intensity == 10.0f,
           "save: the parked level waits for Back");

    expect(fixture.pop().refresh.success, "save: back should succeed");
    expect(fixture.history.size() == levelSteps + 1, "save: back records one level step");
    expect(fixture.light(fixture.scene, first).intensity == 30.0f &&
               fixture.light(fixture.scene, second).intensity == 30.0f &&
               fixture.light(fixture.scene, first).range == 6.0f,
           "save: back refreshes every instance");
    expect(fixture.light(fixture.scene, second).range == 9.0f, "save: an instance keeps its own override");

    fixture.editing().undo();
    expect(readText(file) == original && fixture.light(fixture.scene, first).intensity == 10.0f &&
               fixture.light(fixture.scene, second).intensity == 10.0f &&
               fixture.light(fixture.scene, second).range == 9.0f,
           "save: undo restores the file and the instances");
    expect(fixture.ledger.matches(file, original), "save: undo's write is in the ledger");
    fixture.editing().redo();
    expect(readText(file) == saved && fixture.light(fixture.scene, first).intensity == 30.0f &&
               fixture.light(fixture.scene, second).range == 9.0f,
           "save: redo writes the file and refreshes again");

    // Someone else edits the file: undo must not overwrite their change.
    const std::string outside = saved + "\n";
    {
        std::ofstream(file, std::ios::binary | std::ios::trunc) << outside;
    }
    fixture.diagnostics.acknowledgeErrors();
    fixture.editing().undo();
    expect(readText(file) == outside && fixture.light(fixture.scene, first).intensity == 30.0f &&
               fixture.diagnostics.hasUnacknowledgedError(),
           "save: undo refuses to overwrite a file changed on disk");
}

void testExternalChangeRefreshesParkedDocuments(const fs::path& base) {
    Fixture fixture(freshDirectory(base, "external"));
    authorPrefabs(fixture.root);
    const fs::path file = fixture.root / "lamp.prefab";
    const auto [first, second] = fixture.placeLamps();
    expect(fixture.push("shelf.prefab").success, "external: the shelf should open");
    const std::vector<Entity> nested = instancesOf(fixture.scene, "lamp.prefab");
    expect(nested.size() == 1, "external: the shelf holds one lamp");
    const size_t shelfSteps = fixture.history.size();
    const size_t levelSteps = fixture.stack.parkedScene()->history.size();

    PrefabDocument document;
    expect(loadPrefab(file, document).success, "external: the lamp should load");
    nlohmann::json light = nlohmann::json::parse(document.entities.front().components.front().second);
    light["intensity"] = 55.0f;
    document.entities.front().components.front().second = light.dump();
    expect(savePrefab(file, document).success, "external: the outside edit should save");

    PrefabFileChange change;
    expect(classifyPrefabFileChange(fixture.ledger, file, fixture.root, change).success && change.external &&
               change.previous.has_value(),
           "external: the change is outside, with the content before it");
    // What the session does: the active document, then the parked ones.
    size_t activeRefreshed = 0;
    expect(refreshPrefabInstances(fixture.editing(), change.reference, &*change.previous,
                                  externalPrefabRefreshStepName(change.displayName), &activeRefreshed)
               .success,
           "external: the active document refreshes");
    size_t parkedRefreshed = 0;
    expect(fixture.stack
               .refreshParked(fixture.assets, fixture.diagnostics, change.reference, &*change.previous,
                              externalPrefabRefreshStepName(change.displayName), parkedRefreshed)
               .success,
           "external: the parked documents refresh");
    Scene& level = fixture.stack.parkedScene()->scene;
    expect(activeRefreshed == 1 && fixture.history.size() == shelfSteps + 1 &&
               fixture.scene.registry()
                       .get<PointLightComponent>(instancesOf(fixture.scene, "lamp.prefab").front())
                       .intensity == 55.0f,
           "external: the open shelf's lamp takes the change as one step");
    expect(parkedRefreshed == 2 && fixture.stack.parkedScene()->history.size() == levelSteps + 1 &&
               fixture.light(level, first).intensity == 55.0f && fixture.light(level, second).intensity == 55.0f &&
               fixture.light(level, second).range == 9.0f,
           "external: the parked level's lamps take it as one step, keeping overrides");

    // A parked prefab's own root is never refreshed as an instance.
    expect(fixture.push("lamp.prefab").success, "external: the nested lamp should open");
    size_t ownRefreshed = 0;
    fixture.stack.refreshParked(fixture.assets, fixture.diagnostics, "shelf.prefab", nullptr, "Refresh", ownRefreshed);
    expect(ownRefreshed == 0, "external: the parked shelf document's own root is left alone");
}

void testNestedPushAndPop(const fs::path& base) {
    Fixture fixture(freshDirectory(base, "nested"));
    authorPrefabs(fixture.root);
    const fs::path lampFile = fixture.root / "lamp.prefab";
    const std::string original = readText(lampFile);
    const auto [first, second] = fixture.placeLamps();
    const size_t levelSteps = fixture.history.size();

    expect(fixture.push("shelf.prefab").success, "nested: the shelf should open");
    const SceneEntityId nestedLamp = fixture.scene.id(instancesOf(fixture.scene, "lamp.prefab").front());
    expect(fixture.push("lamp.prefab").success && fixture.stack.depth() == 2, "nested: the lamp opens over the shelf");
    expect(fixture.stack.frames()[0]->prefab.displayName == "Shelf" &&
               fixture.stack.frames()[1]->prefab.displayName == "Lamp",
           "nested: the frames read like breadcrumbs");
    const SceneEntityId root = fixture.stack.activePrefab()->root;
    fixture.edit([&] { fixture.light(fixture.scene, root).intensity = 40.0f; });
    expect(fixture.save().success, "nested: the lamp saves");

    expect(fixture.pop().refresh.success && fixture.stack.depth() == 1, "nested: back returns to the shelf");
    expect(fixture.light(fixture.scene, nestedLamp).intensity == 40.0f && fixture.history.size() == 1,
           "nested: saving the lamp refreshed the shelf's lamp as one shelf step");
    Scene& level = fixture.stack.parkedScene()->scene;
    expect(fixture.light(level, first).intensity == 40.0f &&
               fixture.stack.parkedScene()->history.size() == levelSteps + 1,
           "nested: the level parked further down refreshed its lamps too");
    fixture.editing().undo();
    expect(readText(lampFile) == original && fixture.light(fixture.scene, nestedLamp).intensity == 10.0f,
           "nested: undo in the shelf restores the lamp file and its lamp");
    fixture.pop();
    expect(fixture.stack.empty() && fixture.history.size() == levelSteps + 1, "nested: back at the level");
}

void testMovedPrefabFollows(const fs::path& base) {
    Fixture fixture(freshDirectory(base, "moved"));
    authorPrefabs(fixture.root);
    fixture.placeLamps();
    expect(fixture.push("lamp.prefab").success, "moved: the lamp should open");
    const fs::path from = (fixture.root / "lamp.prefab").lexically_normal();
    const fs::path to = (fixture.root / "props" / "lamp.prefab").lexically_normal();
    fs::create_directories(to.parent_path());
    fs::rename(from, to);
    const ProjectFileMove move{.from = fs::absolute(from).lexically_normal(),
                               .to = fs::absolute(to).lexically_normal()};
    fixture.stack.filesMoved(std::span<const ProjectFileMove>(&move, 1), fixture.assets, fixture.diagnostics);
    expect(fixture.stack.activePrefab()->reference == "props/lamp.prefab",
           "moved: the open prefab follows its renamed file");
    const SceneEntityId root = fixture.stack.activePrefab()->root;
    fixture.edit([&] { fixture.light(fixture.scene, root).intensity = 12.0f; });
    const std::string before = readText(to);
    expect(fixture.save().success && readText(to) != before && !fs::exists(from),
           "moved: saving writes the file where it now lives");

    const fs::path removed = fs::absolute(to).lexically_normal();
    fixture.stack.filesRemoved(std::span<const fs::path>(&removed, 1), fixture.diagnostics);
    expect(fixture.stack.depth() == 1 && !fixture.diagnostics.entries().empty() &&
               fixture.diagnostics.entries().back().message.find("was removed") != std::string::npos,
           "moved: a removed open prefab stays open with a warning");
}

// A prefab file the editor never wrote: the ledger learns its content when
// a document with its instances opens or an instance is placed, so an outside
// change keeps the instances' live overrides.
void testLedgerSeeding(const fs::path& base) {
    Fixture fixture(freshDirectory(base, "seeding"));
    authorPrefabs(fixture.root);
    const fs::path file = (fixture.root / "lamp.prefab").lexically_normal();
    expect(fixture.invoke("prefab.instantiate", R"({"asset":"shelf.prefab"})").status == OperatorStatus::Finished,
           "seeding: the shelf should instantiate");
    expect(fixture.ledger.recorded(file) != nullptr &&
               fixture.ledger.recorded(fixture.root / "shelf.prefab") != nullptr,
           "seeding: placing an instance notes its file and its nested prefabs' files");

    // A level saved earlier and opened now: nothing has been noted yet.
    PrefabAuthoring authoring(fixture.assets, fixture.document.options());
    Scene level;
    Entity first = entt::null;
    Entity second = entt::null;
    expect(authoring.instantiate(level, "lamp.prefab", entt::null, first).success &&
               authoring.instantiate(level, "lamp.prefab", entt::null, second).success,
           "seeding: the level's lamps should instantiate");
    // An override edited since the last prefab operation, so not persisted.
    level.registry().get<PointLightComponent>(second).range = 9.0f;
    Scene unseededLevel;
    std::string text;
    expect(SceneDocument::serialize(level, fixture.assets, fixture.document.options(), text).success &&
               SceneDocument::deserialize(text, fixture.assets, fixture.document.options(), unseededLevel).success,
           "seeding: the level should copy");

    PrefabWriteLedger seeded;
    notePrefabFilesRead(level, fixture.root,
                        {.fileRead = [&seeded](const fs::path& path, const std::optional<std::string>& bytes) {
                            seeded.record(path, bytes);
                        }});
    expect(seeded.matches(file, readText(file)), "seeding: opening a document notes its prefabs' content");

    PrefabDocument document;
    expect(loadPrefab(file, document).success, "seeding: the lamp should load");
    document.entities.front().name = "Renamed lamp";
    expect(savePrefab(file, document).success, "seeding: the outside edit should save");

    PrefabFileChange change;
    expect(classifyPrefabFileChange(seeded, file, fixture.root, change).success && change.previous.has_value(),
           "seeding: the outside change knows the content before it");
    expect(authoring.refreshInstances(level, change.reference, &*change.previous).success &&
               level.registry().get<NameComponent>(second).name == "Renamed lamp" &&
               level.registry().get<PointLightComponent>(second).range == 9.0f,
           "seeding: the refresh keeps the override edited since the last prefab operation");

    // Without the seed the same refresh falls back to persisted overrides.
    expect(
        authoring.refreshInstances(unseededLevel, "lamp.prefab", nullptr).success &&
            unseededLevel.registry().get<PointLightComponent>(instancesOf(unseededLevel, "lamp.prefab").back()).range ==
                4.0f,
        "seeding: without the previous content the live override is lost (what seeding prevents)");
}

// ---------------------------------------------------------------------------
// The whole session, headless.

// Declaration order is teardown order in reverse: the session ends before the
// ImGui context.
class EditorHarness {
  public:
    EditorHarness(const fs::path& root, ProjectConfig project)
        : m_application(root / "settings", ZENITH_EDITOR_FONT, std::move(project), std::nullopt, {})
        , m_session(m_application, scene, assets, m_physics, m_audio, m_rendering, m_gameUi, logger) {}

    SceneResult start() {
        return m_session.start();
    }

    ApplicationFrameResult frame() {
        m_session.update(InputState{}, 1.0f / 60.0f);
        buildFontAtlas();
        m_imgui.frame([this] { m_session.drawUi(); });
        return m_session.finishFrame();
    }

    ApplicationFrameResult press(ImGuiKeyChord chord) {
        m_session.update(InputState{}, 1.0f / 60.0f);
        buildFontAtlas();
        m_imgui.press(chord, [this] { m_session.drawUi(); });
        return m_session.finishFrame();
    }

    // Presses a button of the popup window named title, if it is open.
    bool clickPopupButton(const char* title, const char* button) {
        const ImGuiWindow* window = HeadlessImGuiContext::findWindow(title);
        if (window == nullptr || !window->Active) {
            return false;
        }
        HeadlessImGuiContext::activateNextFrame(HeadlessImGuiContext::itemId(*window, button));
        frame();
        return true;
    }

    bool popupOpen(const char* title) const {
        const ImGuiWindow* window = HeadlessImGuiContext::findWindow(title);
        return window != nullptr && window->Active;
    }

    ApplicationSession& session() {
        return m_session;
    }

    void shutdown() {
        m_session.shutdown();
    }

  private:
    static void buildFontAtlas() {
        ImFontAtlas& fonts = *ImGui::GetIO().Fonts;
        if (!fonts.IsBuilt()) {
            unsigned char* pixels = nullptr;
            int width = 0;
            int height = 0;
            fonts.GetTexDataAsRGBA32(&pixels, &width, &height);
        }
    }

    HeadlessImGui m_imgui;

  public:
    Scene scene;
    AssetManager assets;
    Logger logger{LoggerConfig{.echoToStderr = false}};

  private:
    PhysicsWorld m_physics;
    AudioSystem m_audio;
    RenderRuntime m_rendering;
    GameUiContext m_gameUi;
    EditorApplication m_application;
    ApplicationSession m_session;
};

constexpr ImGuiKeyChord editLampKey = ImGuiKey_F9;
constexpr ImGuiKeyChord backKey = ImGuiKey_F10;
// Adds a point light to the active object: an undoable edit of the prefab
// root, whose instances then gain a light.
constexpr ImGuiKeyChord addLightKey = ImGuiKey_F11;
// Adds an entity: an undoable edit outside any prefab root.
constexpr ImGuiKeyChord addEntityKey = ImGuiKey_F12;
constexpr ImGuiKeyChord reloadKey = ImGuiKey_F8;

// A project under root with lamp.prefab (without a light) and a startup
// level holding two lamp instances and a "Level marker"; keys bound to the
// prefab-mode operators.
ProjectConfig prefabProject(const fs::path& root, bool withModule = false) {
    const fs::path assetRoot = root / "assets";
    fs::create_directories(assetRoot);
    fs::create_directories(root / "scenes");
    fs::create_directories(root / "settings");
    AssetManager assets;
    const SceneDocumentOptions options{.assetRoot = assetRoot};
    PrefabAuthoring authoring(assets, options);
    Scene staging;
    const Entity lamp = staging.createEntity("Lamp");
    staging.setParent(staging.createEntity("Shade"), lamp);
    expect(authoring.create(staging, lamp, "lamp.prefab").success, "session: the lamp prefab should be created");
    Scene level;
    level.createEntity("Level marker");
    Entity instance = entt::null;
    expect(authoring.instantiate(level, "lamp.prefab", entt::null, instance).success &&
               authoring.instantiate(level, "lamp.prefab", entt::null, instance).success,
           "session: the level's lamps should instantiate");
    const fs::path levelPath = root / "scenes" / "level.zenith-scene";
    expect(SceneDocument::save(levelPath, level, assets, options).success, "session: the level should save");

    const auto binding = [](ImGuiKeyChord chord, const char* operatorId, nlohmann::json properties) {
        return nlohmann::json{{"operator", operatorId},
                              {"region", keymapRegionName(KeymapRegion::Global)},
                              {"chord", formatKeyChord(chord)},
                              {"properties", std::move(properties)}};
    };
    // These tests cover prefab mode, which prefab windows replace by default.
    std::ofstream(root / "settings" / "settings.ini") << "ui.prefab_windows=false\n";
    std::ofstream(root / "settings" / "keymap.json") << nlohmann::json{
        {"version", 1},
        {"removed", nlohmann::json::array()},
        {"added",
         {binding(editLampKey, "prefab.edit", {{"asset", "lamp.prefab"}}),
          binding(backKey, "prefab.exit_edit", nlohmann::json::object()),
          binding(addLightKey, "object.component_add", {{"component", "point_light"}}),
          binding(addEntityKey, "object.create_entity", nlohmann::json::object()),
          binding(reloadKey, "module.reload",
                  nlohmann::json::object())}}}.dump();

    ProjectConfig project;
    project.displayName = "Prefab mode test";
    project.assetRoot = assetRoot;
    project.sceneRoot = root / "scenes";
    project.startupScene = levelPath;
    if (withModule) {
        project.gameModule = ZENITH_EDITOR_TEST_MODULE_V1;
    }
    return project;
}

bool inLevel(const Scene& scene) {
    return findNamed(scene, "Level marker") != entt::null;
}

size_t litLamps(const Scene& scene) {
    size_t lit = 0;
    for (const Entity root : instancesOf(scene, "lamp.prefab")) {
        lit += scene.registry().all_of<PointLightComponent>(root) ? 1 : 0;
    }
    return lit;
}

void testUnsavedChangesPrompts(const fs::path& root) {
    EditorHarness editor(root, prefabProject(root));
    if (!editor.start()) {
        expect(false, "prompts: the editor session starts");
        return;
    }
    editor.frame();
    const fs::path file = root / "assets" / "lamp.prefab";
    const std::string original = readText(file);
    expect(inLevel(editor.scene) && instancesOf(editor.scene, "lamp.prefab").size() == 2,
           "prompts: the level opens with two lamps");

    // A clean prefab leaves without asking.
    editor.press(editLampKey);
    expect(!inLevel(editor.scene) && findNamed(editor.scene, "Shade") != entt::null, "prompts: F9 opens the lamp");
    editor.press(backKey);
    expect(inLevel(editor.scene), "prompts: back from a clean prefab returns at once");

    // Cancel stays; Discard leaves without writing.
    editor.press(editLampKey);
    editor.press(addLightKey);
    editor.press(backKey);
    expect(editor.popupOpen("Unsaved Changes") && !inLevel(editor.scene), "prompts: a dirty prefab asks first");
    expect(editor.clickPopupButton("Unsaved Changes", "Cancel") && !inLevel(editor.scene),
           "prompts: Cancel stays in the prefab");
    editor.press(backKey);
    expect(editor.clickPopupButton("Unsaved Changes", "Discard") && inLevel(editor.scene) &&
               readText(file) == original && litLamps(editor.scene) == 0,
           "prompts: Discard returns without writing the prefab");

    // Save writes the prefab and refreshes the level's lamps.
    editor.press(editLampKey);
    editor.press(addLightKey);
    editor.press(backKey);
    expect(editor.clickPopupButton("Unsaved Changes", "Save") && inLevel(editor.scene) && readText(file) != original &&
               litLamps(editor.scene) == 2,
           "prompts: Save writes the prefab, returns, and refreshes the instances");
    editor.press(ImGuiMod_Ctrl | ImGuiKey_Z);
    expect(readText(file) == original && litLamps(editor.scene) == 0,
           "prompts: Ctrl Z in the level restores the file and the instances");
    editor.press(ImGuiMod_Ctrl | ImGuiKey_Y);
    expect(litLamps(editor.scene) == 2, "prompts: redo applies the edit again");

    // New from a dirty prefab over a dirty level asks about both, top down;
    // cancelling the level's prompt keeps the level.
    editor.press(addEntityKey);
    editor.press(editLampKey);
    editor.press(addEntityKey);
    editor.press(ImGuiMod_Ctrl | ImGuiKey_N);
    expect(editor.popupOpen("Unsaved Changes") && !inLevel(editor.scene), "chain: New asks about the prefab first");
    expect(editor.clickPopupButton("Unsaved Changes", "Discard") && inLevel(editor.scene),
           "chain: discarding the prefab returns to the level");
    editor.frame();
    expect(editor.popupOpen("Unsaved Changes"), "chain: then New asks about the dirty level");
    expect(editor.clickPopupButton("Unsaved Changes", "Cancel") && inLevel(editor.scene),
           "chain: cancelling keeps the level");

    // Without anything dirty, New leaves the prefab and replaces the level.
    editor.press(editLampKey);
    editor.press(ImGuiMod_Ctrl | ImGuiKey_N);
    expect(editor.popupOpen("Unsaved Changes"), "chain: the dirty level still asks");
    expect(editor.clickPopupButton("Unsaved Changes", "Discard") && !inLevel(editor.scene) &&
               findNamed(editor.scene, "Shade") == entt::null,
           "chain: New leaves the clean prefab and replaces the level");
}

// The workspace workspaces.json names as active, which the session saves
// when a switch is applied.
std::string activeWorkspaceOnDisk(const fs::path& root) {
    const nlohmann::json file = nlohmann::json::parse(readText(root / "settings" / "workspaces.json"), nullptr, false);
    return file.is_object() ? file.value("activeWorkspace", std::string()) : std::string();
}

// Editing a prefab opens the Prefab workspace and Back returns to the
// workspace it came from; the preference turns that off.
void testPrefabWorkspace(const fs::path& root) {
    {
        EditorHarness editor(root, prefabProject(root));
        if (!editor.start()) {
            expect(false, "workspace: the editor session starts");
            return;
        }
        editor.frame();
        editor.frame();
        expect(activeWorkspaceOnDisk(root) == "Layout", "workspace: the session starts in Layout");
        editor.press(editLampKey);
        editor.frame();
        editor.frame();
        expect(!inLevel(editor.scene) && activeWorkspaceOnDisk(root) == "Prefab",
               "workspace: editing a prefab opens the Prefab workspace");
        editor.press(backKey);
        editor.frame();
        editor.frame();
        expect(inLevel(editor.scene) && activeWorkspaceOnDisk(root) == "Layout",
               "workspace: Back returns to the workspace the prefab was opened from");
    }
    {
        const fs::path off = root / "off";
        ProjectConfig project = prefabProject(off);
        {
            std::ofstream(off / "settings" / "settings.ini", std::ios::app) << "ui.prefab_workspace=false\n";
        }
        EditorHarness editor(off, project);
        if (!editor.start()) {
            expect(false, "workspace: the editor session starts with the preference off");
            return;
        }
        editor.frame();
        editor.press(editLampKey);
        editor.frame();
        editor.frame();
        expect(!inLevel(editor.scene) && activeWorkspaceOnDisk(off) != "Prefab",
               "workspace: with the preference off the prefab opens in the current workspace");
    }
}

void testPlayWaitsForTheScene(const fs::path& root) {
    EditorHarness editor(root, prefabProject(root));
    if (!editor.start()) {
        expect(false, "play: the editor session starts");
        return;
    }
    editor.frame();
    ApplicationSession& session = editor.session();
    editor.press(editLampKey);
    editor.press(ImGuiKey_F5);
    expect(!session.fixedUpdatesEnabled() && !inLevel(editor.scene), "play: F5 does nothing while a prefab is open");
    editor.press(backKey);
    editor.press(ImGuiKey_F5);
    expect(session.fixedUpdatesEnabled(), "play: F5 plays once back in the scene");
    editor.press(ImGuiMod_Shift | ImGuiKey_F5);
    editor.press(ImGuiKey_F5);
    expect(session.fixedUpdatesEnabled(), "play: playing again works");
    editor.press(editLampKey);
    expect(inLevel(editor.scene), "play: a prefab does not open during Play");
    editor.press(ImGuiMod_Shift | ImGuiKey_F5);
}

void testModuleReloadWaitsForTheScene(const fs::path& root) {
    EditorHarness editor(root, prefabProject(root, true));
    if (!editor.start()) {
        expect(false, "reload: the editor session starts with the test module");
        return;
    }
    editor.frame();
    editor.press(editLampKey);
    editor.press(reloadKey);
    const uint64_t generation = editor.scene.contentGeneration();
    editor.frame();
    editor.frame();
    expect(!inLevel(editor.scene) && editor.scene.contentGeneration() == generation,
           "reload: a requested reload waits while a prefab is open");
    editor.press(backKey);
    const uint64_t returned = editor.scene.contentGeneration();
    expect(inLevel(editor.scene), "reload: back to the scene");
    editor.frame();
    expect(editor.scene.contentGeneration() != returned && inLevel(editor.scene),
           "reload: the reload runs once the scene is back");
}

void testRecoverAPrefabEdit(const fs::path& root) {
    ProjectConfig project = prefabProject(root);
    {
        std::ofstream(root / "settings" / "settings.ini", std::ios::app) << "autosave.interval=0.01\n";
    }
    {
        EditorHarness editor(root, project);
        if (!editor.start()) {
            expect(false, "recovery: the editor session starts");
            return;
        }
        editor.frame();
        editor.press(editLampKey);
        editor.press(addLightKey);
        editor.frame();
        editor.frame();
        expect(fs::is_regular_file(root / "settings" / "recovery.prefab-edit") &&
                   fs::is_regular_file(root / "settings" / "recovery.prefab-edit.json"),
               "recovery: autosave writes the prefab edit and its note");
        expect(!fs::exists(root / "settings" / "recovery.zenith-scene"),
               "recovery: the clean parked level is not autosaved");
        editor.shutdown();
    }
    EditorHarness editor(root, project);
    if (!editor.start()) {
        expect(false, "recovery: the editor session starts again");
        return;
    }
    editor.frame();
    expect(editor.popupOpen("Recover Autosave"), "recovery: the next session offers the autosave");
    expect(editor.clickPopupButton("Recover Autosave", "Recover") && !inLevel(editor.scene) &&
               litLamps(editor.scene) == 1,
           "recovery: Recover reopens the prefab edit with its unsaved change");
    editor.press(backKey);
    expect(editor.clickPopupButton("Unsaved Changes", "Save") && inLevel(editor.scene) && litLamps(editor.scene) == 2,
           "recovery: the recovered edit saves and refreshes the level");
    expect(!fs::exists(root / "settings" / "recovery.prefab-edit"), "recovery: leaving the prefab drops its autosave");
}
} // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "zenith_prefab_edit_mode_tests";
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root);

    testEnterAndExitRestoreTheScene(root);
    testPrefabHistoryIsItsOwn(root);
    testSaveAndBackRefreshesInstances(root);
    testExternalChangeRefreshesParkedDocuments(root);
    testNestedPushAndPop(root);
    testMovedPrefabFollows(root);
    testLedgerSeeding(root);
    testUnsavedChangesPrompts(root / "prompts");
    testPrefabWorkspace(root / "workspace");
    testPlayWaitsForTheScene(root / "play");
    testModuleReloadWaitsForTheScene(root / "reload");
    testRecoverAPrefabEdit(root / "recovery");

    fs::remove_all(root, error);
    if (failures != 0) {
        std::cerr << failures << " prefab edit mode test(s) failed\n";
        return 1;
    }
    std::cout << "Prefab edit mode tests passed\n";
    return 0;
}
