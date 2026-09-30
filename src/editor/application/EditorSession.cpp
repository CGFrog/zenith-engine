#include "EditorSession.hpp"

#include "editor/document/SceneFragment.hpp"
#include "editor/interaction/ComponentEditors.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <ImGuizmo.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <ranges>
#include <utility>

namespace {
constexpr uint32_t currentLayoutVersion = 1;
}

EditorSession::EditorSession(std::filesystem::path settingsDirectory, std::filesystem::path assetRoot,
                             std::filesystem::path sceneRoot,
                             std::function<SceneResult(ApplicationContext&)> initialSceneLoader)
    : m_settings(settingsDirectory / "settings.ini")
    , m_document(assetRoot)
    , m_recovery(settingsDirectory / "recovery.zenith-scene")
    , m_assetCatalog(std::move(assetRoot), std::move(sceneRoot))
    , m_initialSceneLoader(std::move(initialSceneLoader))
    , m_layoutPath((settingsDirectory / "layout.ini").string())
    , m_smokeScenePath(settingsDirectory / "streaming-smoke.zenith-scene") {
    const std::pair<const char*, const char*> bindings[]{
        {"New", "Ctrl+N"},  {"Open", "Ctrl+O"}, {"Save", "Ctrl+S"},  {"Save As", "Ctrl+Shift+S"}, {"Undo", "Ctrl+Z"},
        {"Redo", "Ctrl+Y"}, {"Copy", "Ctrl+C"}, {"Paste", "Ctrl+V"}, {"Duplicate", "Ctrl+D"},     {"Play", "F5"},
        {"Pause", "F6"},    {"Step", "F7"},     {"Stop", "Shift+F5"}};
    for (const auto& [action, chord] : bindings) {
        if (const EditorShortcutResult result = m_shortcuts.bind(action, chord); !result) {
            reportDiagnostic(result.error);
        }
    }
}

SceneResult EditorSession::load(ApplicationContext& context) {
    if (const EditorSettingsResult result = m_settings.load(); !result) {
        reportDiagnostic(result.error);
    }
    refreshAssetCatalog();
    m_openRecoveryDialog = m_recovery.available();
    m_smokeScenario = SDL_getenv("ZENITH_EDITOR_SMOKE") != nullptr;

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = m_layoutPath.c_str();
    ImGui::LoadIniSettingsFromDisk(io.IniFilename);
    m_initializeLayout = m_settings.layoutVersion() < currentLayoutVersion;
    if (m_initialSceneLoader) {
        if (SceneResult result = m_initialSceneLoader(context); !result) {
            return result;
        }
    }
    if (m_smokeScenario) {
        Scene streamingFixture;
        for (uint32_t index = 0; index < 5'000; ++index) {
            streamingFixture.createEntity("Streamed entity " + std::to_string(index));
        }
        const auto models = context.scene.registry().view<const ModelComponent>();
        if (!models.empty()) {
            const ModelHandle model = models.get<const ModelComponent>(*models.begin()).model;
            const Entity modelRoot = streamingFixture.createEntity("Streamed model");
            if (SceneResult result = streamingFixture.instantiateModel(modelRoot, model, context.assets); !result) {
                return {.error = "Could not prepare streaming smoke model: " + result.error};
            }
        }
        if (SceneDocumentResult result =
                SceneDocument::save(m_smokeScenePath, streamingFixture, context.assets, m_document.options());
            !result) {
            return {.error = "Could not prepare streaming smoke scene: " + result.error};
        }
        m_smokeJobs = std::make_unique<JobSystem>(JobSystemConfig{.workerCount = 1, .queueCapacity = 4});
        m_smokeLayers = std::make_unique<AdditiveSceneManager>(*m_smokeJobs, context.assets, m_document.options());
    }
    return {.success = true};
}

void EditorSession::update(ApplicationContext& context, const InputState& input, float deltaSeconds) {
    if (m_smokeScenario) {
        if (m_smokeLayers) {
            m_smokeLayers->update();
        }
        ++m_smokeFrame;
        if (m_smokeFrame == 1) {
            RenderSettings settings = context.rendering.settings();
            settings.bloom = BloomQuality::Downsampled;
            settings.shadows = ShadowQuality::CascadedHigh;
            settings.visualizeShadowCascades = true;
            context.rendering.setSettings(settings);
            context.rendering.requestViewportSize(640, 360);
        } else if (m_smokeFrame == 2) {
            enterPlayMode(context);
        } else if (m_smokeFrame == 3) {
            m_playMode.pause();
            m_playMode.requestSingleStep();
        } else if (m_smokeFrame == 4) {
            stopPlayMode(context);
        } else if (m_smokeFrame == 5) {
            context.rendering.requestViewportSize(800, 450);
            m_smokeStreamInstance = m_smokeLayers->requestLoad(m_smokeScenePath, 100);
        }
        if (m_smokeStreamInstance && !m_smokeStreamCompleted) {
            const auto state = m_smokeLayers->state(*m_smokeStreamInstance);
            if (state && state->status == AdditiveSceneStatus::Committed) {
                m_smokeLayers->unload(*m_smokeStreamInstance);
                context.assets.collectUnused(true);
                m_smokeStreamCompleted = true;
            } else if (state && (state->status == AdditiveSceneStatus::Failed ||
                                 state->status == AdditiveSceneStatus::Cancelled)) {
                reportDiagnostic("Streaming smoke failed: " + state->error);
                context.control.requestSceneChange({std::numeric_limits<uint32_t>::max()});
            }
        }
        if (m_smokeFrame == 500 && !m_smokeStreamCompleted) {
            reportDiagnostic("Streaming smoke did not commit while frames were being presented");
            context.control.requestSceneChange({std::numeric_limits<uint32_t>::max()});
        } else if (m_smokeFrame >= 8 && m_smokeStreamCompleted) {
            context.control.requestQuit();
        }
    }
    m_assetRefreshElapsed += deltaSeconds;
    if (m_document.dirty() && m_playMode.mode() == EditorMode::Edit) {
        m_autosaveElapsed += deltaSeconds;
        if (m_autosaveElapsed >= 30.0f) {
            m_autosaveElapsed = 0.0f;
            if (const SceneDocumentResult result =
                    m_recovery.autosave(context.scene, context.assets, m_document.options());
                !result) {
                reportDiagnostic("Autosave failed: " + result.error);
            }
        }
    } else {
        m_autosaveElapsed = 0.0f;
    }
    if (m_assetRefreshElapsed >= 1.0f) {
        m_assetRefreshElapsed = 0.0f;
        refreshAssetCatalog();
    }
    m_viewportNavigating = m_viewportInputActive &&
                           (input.mouseButtonDown(MouseButton::Middle) || input.mouseButtonDown(MouseButton::Right));
    m_camera.update(input, deltaSeconds, m_viewportInputActive);

    if (m_viewportInputActive && !input.mouseButtonDown(MouseButton::Right)) {
        if (input.keyPressed(Key::Q)) {
            m_gizmoMode = GizmoMode::Select;
        } else if (input.keyPressed(Key::W)) {
            m_gizmoMode = GizmoMode::Translate;
        } else if (input.keyPressed(Key::E)) {
            m_gizmoMode = GizmoMode::Rotate;
        } else if (input.keyPressed(Key::R)) {
            m_gizmoMode = GizmoMode::Scale;
        }
        if (input.keyPressed(Key::F) && !m_selection.empty()) {
            focusOnSelection(context);
        }
    }

    context.rendering.setViewOverride(m_camera.renderView());
    if (m_showGrid) {
        const float distance = std::max(m_camera.distance(), 0.1f);
        const float spacing = std::pow(10.0f, std::floor(std::log10(distance / 5.0f)));
        context.rendering.setGridOverlay(
            GridOverlay{.planeHeight = 0.0f, .spacing = spacing, .fadeDistance = spacing * 40.0f});
    } else {
        context.rendering.setGridOverlay(std::nullopt);
    }

    m_outlinedObjects.clear();
    if (m_selection.size() == 1) {
        const Entity selectedEntity = context.scene.findEntity(m_selection.front());
        if (selectedEntity != entt::null) {
            m_outlineTraversal.clear();
            m_outlineTraversal.push_back(selectedEntity);
            while (!m_outlineTraversal.empty()) {
                const Entity entity = m_outlineTraversal.back();
                m_outlineTraversal.pop_back();
                m_outlinedObjects.push_back(static_cast<RenderObjectId>(entt::to_integral(entity)));
                const auto* relationship = context.scene.registry().try_get<RelationshipComponent>(entity);
                if (relationship != nullptr) {
                    m_outlineTraversal.insert(m_outlineTraversal.end(), relationship->children.begin(),
                                              relationship->children.end());
                }
            }
        }
    }
    context.rendering.setOutlinedObjects(m_outlinedObjects);
}

void EditorSession::fixedUpdate(ApplicationContext&, const InputState&, float) {}

bool EditorSession::fixedUpdatesEnabled() const {
    return m_playMode.fixedUpdatesEnabled();
}

bool EditorSession::beginFixedUpdate() {
    return m_playMode.beginFixedUpdate();
}

void EditorSession::requestQuit(ApplicationContext& context) {
    requestClose(context);
}

void EditorSession::drawUi(ApplicationContext& context) {
    ImGuizmo::BeginFrame();
    const ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
    if (m_initializeLayout) {
        initializeDefaultLayout(dockspaceId);
    }
    drawMainMenu(context);
    drawPathDialog(context);
    drawUnsavedChangesDialog(context);
    drawRecoveryDialog(context);
    drawPanels(context);
    drawStatusBar(context.rendering.stats());
}

void EditorSession::initializeDefaultLayout(uint32_t dockspaceId) {
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

    ImGuiID center = dockspaceId;
    const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, nullptr, &center);
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.25f, nullptr, &center);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.25f, nullptr, &center);

    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Diagnostics", bottom);
    ImGui::DockBuilderDockWindow("Render Statistics", bottom);
    ImGui::DockBuilderFinish(dockspaceId);

    m_settings.panels() = {};
    m_settings.setLayoutVersion(currentLayoutVersion);
    m_initializeLayout = false;
}

void EditorSession::unload(ApplicationContext& context) {
    if (m_smokeLayers) {
        m_smokeLayers->clear();
        m_smokeLayers.reset();
    }
    if (m_smokeJobs) {
        m_smokeJobs->shutdown(true);
        m_smokeJobs.reset();
    }
    if (m_smokeScenario) {
        std::error_code ignored;
        std::filesystem::remove(m_smokeScenePath, ignored);
    }
    stopPlayMode(context);
    context.rendering.setGridOverlay(std::nullopt);
    context.rendering.clearOutlinedObjects();
    context.rendering.clearViewOverride();
    ImGui::SaveIniSettingsToDisk(m_layoutPath.c_str());
    if (const EditorSettingsResult result = m_settings.save(); !result) {
        reportDiagnostic(result.error);
    }
    ImGui::GetIO().IniFilename = nullptr;
}

void EditorSession::drawMainMenu(ApplicationContext& context) {
    if (!ImGui::GetIO().WantTextInput && m_playMode.mode() == EditorMode::Edit) {
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_N)) {
            requestNew(context);
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O)) {
            requestOpen(context);
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S)) {
            requestPathDialog(true);
        } else if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S)) {
            saveDocument(context);
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z) && m_history.canUndo()) {
            EditorCommandContext command = commandContext(context);
            if (const EditorCommandResult result = m_history.undo(command); !result) {
                reportDiagnostic(result.error);
            }
            syncDocumentDirtyState();
            removeInvalidSelection(context.scene);
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y) && m_history.canRedo()) {
            EditorCommandContext command = commandContext(context);
            if (const EditorCommandResult result = m_history.redo(command); !result) {
                reportDiagnostic(result.error);
            }
            syncDocumentDirtyState();
            removeInvalidSelection(context.scene);
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C)) {
            copySelection(context);
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V)) {
            pasteClipboard(context);
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D)) {
            duplicateSelection(context);
        }
    }
    if (!ImGui::GetIO().WantTextInput) {
        if (m_playMode.mode() == EditorMode::Edit && ImGui::Shortcut(ImGuiKey_F5)) {
            enterPlayMode(context);
        } else if (m_playMode.mode() != EditorMode::Edit && ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_F5)) {
            stopPlayMode(context);
        }
        if (m_playMode.mode() == EditorMode::Play && ImGui::Shortcut(ImGuiKey_F6)) {
            m_playMode.pause();
        } else if (m_playMode.mode() == EditorMode::Paused && ImGui::Shortcut(ImGuiKey_F6)) {
            m_playMode.resume();
        }
        if (m_playMode.mode() == EditorMode::Paused && ImGui::Shortcut(ImGuiKey_F7)) {
            m_playMode.requestSingleStep();
        }
    }
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    if (ImGui::BeginMenu("File")) {
        ImGui::BeginDisabled(m_playMode.mode() != EditorMode::Edit);
        if (ImGui::MenuItem("New", m_shortcuts.chord("New").c_str())) {
            requestNew(context);
        }
        if (ImGui::MenuItem("Open...", m_shortcuts.chord("Open").c_str())) {
            requestOpen(context);
        }
        if (ImGui::MenuItem("Save", m_shortcuts.chord("Save").c_str())) {
            saveDocument(context);
        }
        if (ImGui::MenuItem("Save As...", m_shortcuts.chord("Save As").c_str())) {
            requestPathDialog(true);
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) {
            requestClose(context);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        const bool editing = m_playMode.mode() == EditorMode::Edit;
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, editing && m_history.canUndo())) {
            EditorCommandContext command = commandContext(context);
            if (const EditorCommandResult result = m_history.undo(command); !result) {
                reportDiagnostic(result.error);
            }
            syncDocumentDirtyState();
            removeInvalidSelection(context.scene);
        }
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, editing && m_history.canRedo())) {
            EditorCommandContext command = commandContext(context);
            if (const EditorCommandResult result = m_history.redo(command); !result) {
                reportDiagnostic(result.error);
            }
            syncDocumentDirtyState();
            removeInvalidSelection(context.scene);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Copy", m_shortcuts.chord("Copy").c_str(), false, editing && !m_selection.empty())) {
            copySelection(context);
        }
        if (ImGui::MenuItem("Paste", m_shortcuts.chord("Paste").c_str(), false,
                            editing && !m_clipboardFragment.empty())) {
            pasteClipboard(context);
        }
        if (ImGui::MenuItem("Duplicate", m_shortcuts.chord("Duplicate").c_str(), false,
                            editing && !m_selection.empty())) {
            duplicateSelection(context);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        EditorPanelVisibility& panels = m_settings.panels();
        ImGui::MenuItem("Viewport", nullptr, &panels.viewport);
        ImGui::MenuItem("Hierarchy", nullptr, &panels.hierarchy);
        ImGui::MenuItem("Inspector", nullptr, &panels.inspector);
        ImGui::MenuItem("Diagnostics", nullptr, &panels.diagnostics);
        ImGui::MenuItem("Render Statistics", nullptr, &panels.renderStatistics);
        ImGui::MenuItem("Asset Browser", nullptr, &panels.assetBrowser);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Play")) {
        if (m_playMode.mode() == EditorMode::Edit) {
            if (ImGui::MenuItem("Play", "F5")) {
                enterPlayMode(context);
            }
        } else {
            if (m_playMode.mode() == EditorMode::Play && ImGui::MenuItem("Pause", "F6")) {
                m_playMode.pause();
            } else if (m_playMode.mode() == EditorMode::Paused && ImGui::MenuItem("Resume", "F6")) {
                m_playMode.resume();
            }
            if (ImGui::MenuItem("Single Step", "F7", false, m_playMode.mode() == EditorMode::Paused)) {
                m_playMode.requestSingleStep();
            }
            if (ImGui::MenuItem("Stop", "Shift+F5")) {
                stopPlayMode(context);
            }
        }
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void EditorSession::drawPanels(ApplicationContext& context) {
    const RenderStats& stats = context.rendering.stats();
    EditorPanelVisibility& panels = m_settings.panels();
    if (panels.viewport) {
        if (ImGui::Begin("Viewport", &panels.viewport,
                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            drawViewport(context);
        }
        ImGui::End();
    } else {
        m_viewportInputActive = false;
        context.rendering.setViewportInputActive(false);
    }
    if (panels.hierarchy) {
        if (ImGui::Begin("Hierarchy", &panels.hierarchy)) {
            ImGui::BeginDisabled(m_playMode.mode() != EditorMode::Edit);
            drawHierarchy(context);
            ImGui::EndDisabled();
        }
        ImGui::End();
    }
    if (panels.inspector) {
        if (ImGui::Begin("Inspector", &panels.inspector)) {
            ImGui::BeginDisabled(m_playMode.mode() != EditorMode::Edit);
            drawInspector(context);
            ImGui::EndDisabled();
        }
        ImGui::End();
    }
    if (panels.diagnostics) {
        if (ImGui::Begin("Diagnostics", &panels.diagnostics)) {
            if (m_diagnostics.entries().empty()) {
                ImGui::TextDisabled("No diagnostics.");
            } else {
                for (const EditorDiagnostic& diagnostic : m_diagnostics.entries()) {
                    ImGui::PushID(static_cast<int>(diagnostic.id));
                    const char* severity = diagnostic.severity == EditorDiagnosticSeverity::Error     ? "Error"
                                           : diagnostic.severity == EditorDiagnosticSeverity::Warning ? "Warning"
                                                                                                      : "Info";
                    ImGui::Text("[%s] %s", severity, diagnostic.source.c_str());
                    ImGui::TextWrapped("%s", diagnostic.message.c_str());
                    if (!diagnostic.action.empty()) {
                        ImGui::TextDisabled("Action: %s", diagnostic.action.c_str());
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Dismiss")) {
                        const uint64_t id = diagnostic.id;
                        m_diagnostics.dismiss(id);
                        ImGui::PopID();
                        break;
                    }
                    ImGui::Separator();
                    ImGui::PopID();
                }
            }

            ImGui::SeparatorText("Asset lifetime");
            const AssetMemoryBudget budget = context.assets.memoryBudget();
            ImGui::Text("CPU retained: %llu bytes", static_cast<unsigned long long>(context.assets.cpuBytes()));
            if (budget.cpuBytes != std::numeric_limits<uint64_t>::max()) {
                ImGui::SameLine();
                ImGui::TextDisabled("Budget: %llu bytes", static_cast<unsigned long long>(budget.cpuBytes));
            }
            if (ImGui::Button("Collect unused assets")) {
                // The scene may have changed during this UI callback. This
                // fresh lease set protects it while collection runs; renderer
                // frame leases protect submitted GPU work independently.
                const AssetLeaseSet liveAssets = context.assets.leaseSceneAssets(context.scene);
                static_cast<void>(liveAssets);
                m_lastAssetCollection = context.assets.collectUnused(true);
                m_lastAudioCollection = context.audio.collectUnusedAssets(true);
            }
            if (m_lastAssetCollection) {
                const AssetCollectionResult& collection = *m_lastAssetCollection;
                ImGui::SameLine();
                ImGui::Text("%u evicted, %llu bytes freed", collection.assetsEvicted,
                            static_cast<unsigned long long>(collection.bytesFreed));
                if (!collection.budgetSatisfied) {
                    ImGui::TextWrapped(
                        "Collection could not satisfy the asset memory budget. Inspect strong-reference counts "
                        "below and release previews, scenes, voices, or in-flight work before retrying.");
                }
            }

            const std::vector<AssetDiagnostic> assets = context.assets.diagnostics();
            if (ImGui::BeginTable("Asset ownership", 6,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn("Kind");
                ImGui::TableSetupColumn("Slot");
                ImGui::TableSetupColumn("Generation");
                ImGui::TableSetupColumn("Strong refs");
                ImGui::TableSetupColumn("CPU bytes");
                ImGui::TableSetupColumn("GPU estimate");
                ImGui::TableHeadersRow();
                for (const AssetDiagnostic& asset : assets) {
                    const char* kind = asset.kind == AssetKind::Model      ? "Model"
                                       : asset.kind == AssetKind::Mesh     ? "Mesh"
                                       : asset.kind == AssetKind::Material ? "Material"
                                       : asset.kind == AssetKind::Texture  ? "Texture"
                                                                           : "Font";
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(kind);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", asset.index);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", asset.generation);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", asset.strongReferences);
                    ImGui::TableNextColumn();
                    ImGui::Text("%llu", static_cast<unsigned long long>(asset.cpuBytes));
                    ImGui::TableNextColumn();
                    ImGui::Text("%llu", static_cast<unsigned long long>(asset.gpuBytes));
                    if (!asset.source.empty() && ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", asset.source.c_str());
                    }
                }
                ImGui::EndTable();
            }

            const std::vector<AudioAssetDiagnostic> audioAssets = context.audio.assetDiagnostics();
            ImGui::Text("Decoded audio retained: %llu bytes",
                        static_cast<unsigned long long>(context.audio.decodedAudioBytes()));
            if (m_lastAudioCollection) {
                ImGui::SameLine();
                ImGui::Text("%u clips and %u streams evicted", m_lastAudioCollection->clipsEvicted,
                            m_lastAudioCollection->streamsEvicted);
            }
            if (ImGui::BeginTable("Audio asset ownership", 5,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn("Kind");
                ImGui::TableSetupColumn("Slot");
                ImGui::TableSetupColumn("Generation");
                ImGui::TableSetupColumn("Active voices");
                ImGui::TableSetupColumn("Decoded bytes");
                ImGui::TableHeadersRow();
                for (const AudioAssetDiagnostic& asset : audioAssets) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(asset.streaming ? "Stream" : "Clip");
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", asset.index);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", asset.generation);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", asset.strongReferences);
                    ImGui::TableNextColumn();
                    ImGui::Text("%llu", static_cast<unsigned long long>(asset.decodedBytes));
                    if (!asset.source.empty() && ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", asset.source.c_str());
                    }
                }
                ImGui::EndTable();
            }
        }
        ImGui::End();
    }
    if (panels.renderStatistics) {
        if (ImGui::Begin("Render Statistics", &panels.renderStatistics)) {
            ImGui::Text("Draw calls: %u", stats.drawCalls);
            ImGui::Text("Visible: %u / %u", stats.visibleCount, stats.renderableCount);
            ImGui::Text("Triangles: %llu", static_cast<unsigned long long>(stats.triangleCount));
            ImGui::Text("GPU frame: %.2f ms", stats.gpuFrameMilliseconds);
            ImGui::Text("CPU render packets: %.3f ms%s", stats.cpuRenderPacketMilliseconds,
                        stats.renderPacketsRebuilt ? " (rebuilt)" : " (cached)");
        }
        ImGui::End();
    }
    if (panels.assetBrowser) {
        if (ImGui::Begin("Asset Browser", &panels.assetBrowser)) {
            drawAssetBrowser(context);
        }
        ImGui::End();
    }
}

void EditorSession::drawAssetBrowser(ApplicationContext& context) {
    if (ImGui::Button("Refresh")) {
        refreshAssetCatalog();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##AssetFilter", "Filter assets...", &m_assetFilter);
    ImGui::Separator();

    const auto kindName = [](EditorAssetKind kind) {
        switch (kind) {
        case EditorAssetKind::Model:
            return "Model";
        case EditorAssetKind::Texture:
            return "Texture";
        case EditorAssetKind::Audio:
            return "Audio";
        case EditorAssetKind::Scene:
            return "Scene";
        }
        return "Asset";
    };
    std::string filter = m_assetFilter;
    std::ranges::transform(filter, filter.begin(),
                           [](unsigned char value) { return static_cast<char>(std::tolower(value)); });

    for (const EditorAssetRecord& asset : m_assetCatalog.assets()) {
        std::string label = asset.relativePath.generic_string();
        std::string searchable = label;
        std::ranges::transform(searchable, searchable.begin(),
                               [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        if (!filter.empty() && searchable.find(filter) == std::string::npos) {
            continue;
        }

        ImGui::PushID(asset.absolutePath.string().c_str());
        ImGui::TextDisabled("[%s]", kindName(asset.kind));
        if (asset.kind == EditorAssetKind::Texture) {
            m_thumbnailCache.request(asset.absolutePath);
            if (const auto thumbnail = m_thumbnailCache.find(asset.absolutePath);
                thumbnail && thumbnail->state == ThumbnailState::Ready) {
                ImGui::SameLine();
                ImGui::TextDisabled("%ux%u", thumbnail->width, thumbnail->height);
            }
        }
        ImGui::SameLine();
        const bool selectedAsset = m_selectedAsset == asset.absolutePath;
        if (ImGui::Selectable(label.c_str(), selectedAsset, ImGuiSelectableFlags_AllowDoubleClick)) {
            m_selectedAsset = asset.absolutePath;
            if (asset.kind == EditorAssetKind::Scene && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                requestOpenPath(context, asset.absolutePath);
            }
        }
        if (asset.kind == EditorAssetKind::Model && ImGui::BeginDragDropSource()) {
            const std::string path = asset.absolutePath.string();
            ImGui::SetDragDropPayload("ZENITH_MODEL_ASSET", path.c_str(), path.size() + 1);
            ImGui::Text("Place %s", label.c_str());
            ImGui::EndDragDropSource();
        }
        ImGui::PopID();
    }

    if (!m_selectedAsset.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", m_selectedAsset.string().c_str());
        const auto found = std::ranges::find_if(m_assetCatalog.assets(), [this](const EditorAssetRecord& asset) {
            return asset.absolutePath == m_selectedAsset;
        });
        if (found != m_assetCatalog.assets().end() && found->kind == EditorAssetKind::Model &&
            ImGui::Button("Reload Model")) {
            const AssetLoadResult<ModelHandle> result = context.assets.reloadModel(found->absolutePath);
            if (!result) {
                reportDiagnostic("Model reload failed: " + result.error);
            } else {
                reportDiagnostic("Model reloaded. Existing instances retain their previous immutable resources; new "
                                 "placements use the refreshed model.");
            }
        }
    }
}

void EditorSession::drawViewport(ApplicationContext& context) {
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const uint32_t width = static_cast<uint32_t>(std::max(available.x, 16.0f));
    const uint32_t height = static_cast<uint32_t>(std::max(available.y, 16.0f));
    context.rendering.requestViewportSize(width, height);
    const UiTextureHandle texture = context.rendering.viewportTexture();
    const ImVec2 imagePosition = ImGui::GetCursorScreenPos();

    if (texture) {
        ImGui::Image(static_cast<ImTextureID>(texture.value), {static_cast<float>(width), static_cast<float>(height)});
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ZENITH_MODEL_ASSET")) {
                instantiateModelAsset(context, static_cast<const char*>(payload->Data), entt::null);
            }
            ImGui::EndDragDropTarget();
        }
        const bool imageHovered = ImGui::IsItemHovered();
        const bool imageClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        const ImVec2 mouse = ImGui::GetMousePos();
        m_viewportInputActive = imageHovered || m_viewportNavigating;
        context.rendering.setViewportInputActive(m_viewportInputActive);

        const glm::vec2 viewportPosition{imagePosition.x, imagePosition.y};
        const glm::vec2 viewportSize{static_cast<float>(width), static_cast<float>(height)};
        const bool toolbarBlocksPicking = drawViewportToolbar(viewportPosition, viewportSize);

        const bool gizmoBlocksPicking = drawGizmo(context, {imagePosition.x, imagePosition.y},
                                                  {static_cast<float>(width), static_cast<float>(height)});
        if (imageClicked && !toolbarBlocksPicking && !gizmoBlocksPicking) {
            const glm::vec2 normalized{(mouse.x - imagePosition.x) / static_cast<float>(width),
                                       (mouse.y - imagePosition.y) / static_cast<float>(height)};
            context.scene.updateWorldTransforms();
            const Entity picked =
                m_camera.pick(context.scene, context.assets, normalized, static_cast<float>(width) / height);
            if (picked == entt::null) {
                m_selection.clear();
            } else {
                select(context.scene.id(picked), ImGui::GetIO().KeyCtrl);
            }
        }
        if (!m_selection.empty()) {
            const Entity selectedEntity = context.scene.findEntity(m_selection.front());
            if (selectedEntity != entt::null) {
                const std::string& name = context.scene.registry().get<NameComponent>(selectedEntity).name;
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                const ImVec2 labelPosition{imagePosition.x + 8.0f, imagePosition.y + 8.0f};
                const ImVec2 labelSize = ImGui::CalcTextSize(name.c_str());
                drawList->AddRectFilled({labelPosition.x - 4.0f, labelPosition.y - 2.0f},
                                        {labelPosition.x + labelSize.x + 4.0f, labelPosition.y + labelSize.y + 2.0f},
                                        IM_COL32(20, 20, 24, 210), 3.0f);
                drawList->AddText(labelPosition, IM_COL32(255, 190, 60, 255), name.c_str());
            }
        }
    } else {
        ImGui::TextDisabled("Viewport will be available on the next rendered frame.");
        m_viewportInputActive = false;
        context.rendering.setViewportInputActive(false);
    }
}

bool EditorSession::drawViewportToolbar(glm::vec2 viewportPos, glm::vec2 viewportSize) {
    ImGui::SetCursorScreenPos({viewportPos.x + viewportSize.x * 0.5f - 200.0f, viewportPos.y + 8.0f});
    ImGui::BeginGroup();

    const char* modeNames[] = {"Select (Q)", "Move (W)", "Rotate (E)", "Scale (R)"};
    GizmoMode modes[] = {GizmoMode::Select, GizmoMode::Translate, GizmoMode::Rotate, GizmoMode::Scale};

    for (int i = 0; i < 4; ++i) {
        bool isActive = m_gizmoMode == modes[i];
        if (isActive) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
        }
        if (ImGui::Button(modeNames[i], ImVec2(90, 0))) {
            m_gizmoMode = modes[i];
        }
        if (isActive) {
            ImGui::PopStyleColor(3);
        }
        ImGui::SameLine(0, 4.0f);
    }

    ImGui::Dummy(ImVec2(10, 0));
    ImGui::SameLine();

    const char* spaceNames[] = {"Global", "Local"};
    if (ImGui::Button(spaceNames[static_cast<int>(m_transformSpace)], ImVec2(70, 0))) {
        m_transformSpace =
            (m_transformSpace == TransformSpace::Global) ? TransformSpace::Local : TransformSpace::Global;
    }
    ImGui::SameLine(0, 4.0f);

    if (ImGui::Checkbox("Snap", &m_gizmoSnapping)) {
    }
    ImGui::SameLine(0, 4.0f);

    float* snapValue = m_gizmoMode == GizmoMode::Translate
                           ? &m_translationSnap
                           : (m_gizmoMode == GizmoMode::Rotate ? &m_rotationSnap : &m_scaleSnap);
    ImGui::SetNextItemWidth(70.0f);
    ImGui::DragFloat("##SnapValue", snapValue, 0.1f, 0.001f, 1000.0f, "%.3f");
    ImGui::SameLine(0, 8.0f);
    ImGui::Checkbox("Grid", &m_showGrid);

    ImGui::EndGroup();
    return ImGui::IsItemHovered();
}

bool EditorSession::drawGizmo(ApplicationContext& context, glm::vec2 position, glm::vec2 size) {
    if (m_gizmoMode == GizmoMode::Select) {
        m_gizmoWasUsing = false;
        m_gizmoBefore.clear();
        return false;
    }

    if (m_selection.size() != 1) {
        m_gizmoWasUsing = false;
        m_gizmoBefore.clear();
        return false;
    }
    const Entity entity = context.scene.findEntity(m_selection.front());
    if (entity == entt::null) {
        return false;
    }
    context.scene.updateWorldTransforms();
    glm::mat4 world = context.scene.registry().get<TransformComponent>(entity).world;
    const glm::mat4 view = m_camera.viewMatrix();
    const glm::mat4 projection = m_camera.gizmoProjectionMatrix(size.x / size.y);
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(position.x, position.y, size.x, size.y);
    ImGuizmo::SetOrthographic(false);
    const ImGuizmo::OPERATION operation = m_gizmoMode == GizmoMode::Translate
                                              ? ImGuizmo::TRANSLATE
                                              : (m_gizmoMode == GizmoMode::Rotate ? ImGuizmo::ROTATE : ImGuizmo::SCALE);
    const ImGuizmo::MODE mode = m_transformSpace == TransformSpace::Local ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    const float* snapValue = m_gizmoMode == GizmoMode::Translate
                                 ? &m_translationSnap
                                 : (m_gizmoMode == GizmoMode::Rotate ? &m_rotationSnap : &m_scaleSnap);
    const float snap[3]{*snapValue, *snapValue, *snapValue};
    const glm::mat4 localBeforeManipulation = context.scene.registry().get<TransformComponent>(entity).local;
    const bool changed = ImGuizmo::Manipulate(&view[0][0], &projection[0][0], operation, mode, &world[0][0], nullptr,
                                              m_gizmoSnapping ? snap : nullptr);
    if (changed) {
        const Entity parent = context.scene.registry().get<RelationshipComponent>(entity).parent;
        context.scene.registry().get<TransformComponent>(entity).local =
            parent == entt::null ? world
                                 : glm::inverse(context.scene.registry().get<TransformComponent>(parent).world) * world;
        context.scene.updateWorldTransforms();
    }
    const bool usingGizmo = ImGuizmo::IsUsing();
    if (usingGizmo && !m_gizmoWasUsing) {
        const glm::mat4 editedLocal = context.scene.registry().get<TransformComponent>(entity).local;
        context.scene.registry().get<TransformComponent>(entity).local = localBeforeManipulation;
        context.scene.updateWorldTransforms();
        if (!captureSnapshot(context, m_gizmoBefore)) {
            m_gizmoBefore.clear();
        }
        context.scene.registry().get<TransformComponent>(entity).local = editedLocal;
        context.scene.updateWorldTransforms();
    } else if (!usingGizmo && m_gizmoWasUsing && !m_gizmoBefore.empty()) {
        commitMutation(context, "Transform Gizmo", {}, std::move(m_gizmoBefore));
        m_gizmoBefore.clear();
    }
    m_gizmoWasUsing = usingGizmo;
    return usingGizmo || ImGuizmo::IsOver();
}

void EditorSession::drawHierarchy(ApplicationContext& context) {
    removeInvalidSelection(context.scene);

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##HierarchyFilter", "Search...", &m_hierarchyFilter);
    if (ImGui::IsItemFocused() && !m_hierarchyFilterFocused) {
        m_hierarchyFilterFocused = true;
    } else if (!ImGui::IsItemFocused() && m_hierarchyFilterFocused) {
        m_hierarchyFilterFocused = false;
    }

    if (ImGui::Button("Create")) {
        const Entity parent = m_selection.size() == 1 ? context.scene.findEntity(m_selection.front()) : entt::null;
        SceneEntityId createdId;
        performMutation(context, "Create Entity", {}, [&] {
            const Entity created = context.scene.createEntity("Entity");
            createdId = context.scene.id(created);
            return parent == entt::null ? SceneResult{.success = true} : context.scene.reparent(created, parent);
        });
        if (createdId) {
            select(createdId, false);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Duplicate") && m_selection.size() == 1) {
        duplicateSelection(context);
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete") && !m_selection.empty()) {
        const std::vector<SceneEntityId> selection = m_selection;
        performMutation(context, "Delete Entities", {}, [&] {
            for (SceneEntityId id : selection) {
                const Entity entity = context.scene.findEntity(id);
                if (entity != entt::null) {
                    context.scene.destroyEntity(entity);
                }
            }
            return SceneResult{.success = true};
        });
        m_selection.clear();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Preserve world", &m_preserveWorldOnReparent);

    ImGui::Separator();

    if (ImGui::Selectable("Scene Root", false, ImGuiSelectableFlags_AllowDoubleClick)) {
        m_selection.clear();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ZENITH_SCENE_ENTITY")) {
            const SceneEntityId dragged{*static_cast<const uint64_t*>(payload->Data)};
            const Entity child = context.scene.findEntity(dragged);
            performMutation(context, "Reparent Entity", {}, [&] {
                return context.scene.reparent(child, entt::null,
                                              m_preserveWorldOnReparent ? ReparentMode::PreserveWorldTransform
                                                                        : ReparentMode::PreserveLocalTransform);
            });
        }
        ImGui::EndDragDropTarget();
    }

    auto shouldFilter = [this](const std::string& name) {
        if (m_hierarchyFilter.empty()) {
            return true;
        }
        std::string lowerName = name;
        std::string lowerFilter = m_hierarchyFilter;
        const auto lowercase = [](unsigned char character) { return static_cast<char>(std::tolower(character)); };
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), lowercase);
        std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(), lowercase);
        return lowerName.find(lowerFilter) != std::string::npos;
    };

    auto view = context.scene.registry().view<RelationshipComponent>();
    for (Entity entity : view) {
        if (view.get<RelationshipComponent>(entity).parent == entt::null) {
            const std::string& name = context.scene.registry().get<NameComponent>(entity).name;
            if (shouldFilter(name)) {
                drawHierarchyEntity(context, entity);
            }
        }
    }
}

void EditorSession::drawHierarchyEntity(ApplicationContext& context, Entity entity) {
    entt::registry& registry = context.scene.registry();
    const SceneEntityId entityId = context.scene.id(entity);
    const auto& relationship = registry.get<RelationshipComponent>(entity);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (relationship.children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }
    if (selected(entityId)) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    const std::string& name = registry.get<NameComponent>(entity).name;
    const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<uintptr_t>(entityId.value)), flags, "%s",
                                        name.empty() ? "Unnamed Entity" : name.c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        select(entityId, ImGui::GetIO().KeyCtrl);
    }
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("ZENITH_SCENE_ENTITY", &entityId.value, sizeof(entityId.value));
        ImGui::TextUnformatted(name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ZENITH_SCENE_ENTITY")) {
            const SceneEntityId dragged{*static_cast<const uint64_t*>(payload->Data)};
            const Entity child = context.scene.findEntity(dragged);
            performMutation(context, "Reparent Entity", {}, [&] {
                return context.scene.reparent(child, entity,
                                              m_preserveWorldOnReparent ? ReparentMode::PreserveWorldTransform
                                                                        : ReparentMode::PreserveLocalTransform);
            });
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem("HierarchyContextMenu")) {
        if (ImGui::MenuItem("Create Child")) {
            SceneEntityId createdId;
            performMutation(context, "Create Entity", {}, [&] {
                const Entity created = context.scene.createEntity("Entity");
                createdId = context.scene.id(created);
                return context.scene.reparent(created, entity);
            });
            if (createdId) {
                select(createdId, false);
            }
        }
        if (ImGui::MenuItem("Create Sibling")) {
            const Entity parent = registry.get<RelationshipComponent>(entity).parent;
            SceneEntityId createdId;
            performMutation(context, "Create Entity", {}, [&] {
                const Entity created = context.scene.createEntity("Entity");
                createdId = context.scene.id(created);
                return parent == entt::null ? SceneResult{.success = true} : context.scene.reparent(created, parent);
            });
            if (createdId) {
                select(createdId, false);
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, true)) {
            duplicateSelection(context);
        }
        if (ImGui::MenuItem("Delete", "Del", false, true)) {
            performMutation(context, "Delete Entity", {}, [&] {
                context.scene.destroyEntity(entity);
                return SceneResult{.success = true};
            });
            m_selection.clear();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Rename", "F2", false, true)) {
            ImGui::SetKeyboardFocusHere(-1);
        }
        if (ImGui::MenuItem("Expand All")) {
        }
        if (ImGui::MenuItem("Collapse All")) {
        }
        ImGui::EndPopup();
    }
    if (open) {
        const std::vector<Entity> children = relationship.children;
        for (Entity child : children) {
            if (registry.valid(child)) {
                const std::string& childName = registry.get<NameComponent>(child).name;
                if (m_hierarchyFilter.empty() || childName.find(m_hierarchyFilter) != std::string::npos) {
                    drawHierarchyEntity(context, child);
                }
            }
        }
        ImGui::TreePop();
    }
}

void EditorSession::drawInspector(ApplicationContext& context) {
    removeInvalidSelection(context.scene);
    if (m_selection.empty()) {
        ImGui::TextDisabled("Select an entity to inspect it.");
        return;
    }
    if (m_selection.size() != 1) {
        ImGui::Text("%zu entities selected", m_selection.size());
        if (ImGui::Button("Delete All")) {
            const std::vector<SceneEntityId> selection = m_selection;
            performMutation(context, "Delete Entities", {}, [&] {
                for (SceneEntityId id : selection) {
                    const Entity entity = context.scene.findEntity(id);
                    if (entity != entt::null) {
                        context.scene.destroyEntity(entity);
                    }
                }
                return SceneResult{.success = true};
            });
            m_selection.clear();
        }
        return;
    }
    const Entity entity = context.scene.findEntity(m_selection.front());
    if (entity == entt::null) {
        return;
    }
    entt::registry& registry = context.scene.registry();
    auto& name = registry.get<NameComponent>(entity).name;
    std::array<char, 256> nameBuffer{};
    const size_t nameLength = std::min(name.size(), nameBuffer.size() - 1);
    std::copy_n(name.data(), nameLength, nameBuffer.data());
    if (ImGui::InputText("Name", nameBuffer.data(), nameBuffer.size())) {
        std::string beforeName;
        if (captureSnapshot(context, beforeName)) {
            name = nameBuffer.data();
            commitMutation(context, "Rename Entity", "name:" + std::to_string(m_selection.front().value),
                           std::move(beforeName));
        }
    }
    ImGui::TextDisabled("ID %llu", static_cast<unsigned long long>(m_selection.front().value));

    ImGui::Separator();

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##InspectorFilter", "Filter properties...", &m_inspectorFilter);
    if (ImGui::IsItemFocused() && !m_inspectorFilterFocused) {
        m_inspectorFilterFocused = true;
    } else if (!ImGui::IsItemFocused() && m_inspectorFilterFocused) {
        m_inspectorFilterFocused = false;
    }

    auto shouldShowProperty = [this](const char* label) {
        if (m_inspectorFilter.empty()) {
            return true;
        }
        std::string lowerLabel = label;
        std::string lowerFilter = m_inspectorFilter;
        const auto lowercase = [](unsigned char character) { return static_cast<char>(std::tolower(character)); };
        std::transform(lowerLabel.begin(), lowerLabel.end(), lowerLabel.begin(), lowercase);
        std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(), lowercase);
        return lowerLabel.find(lowerFilter) != std::string::npos;
    };

    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (shouldShowProperty("Transform")) {
            const Entity currentEntity = context.scene.findEntity(m_selection.front());
            TransformComponent& transform = context.scene.registry().get<TransformComponent>(currentEntity);
            const TransformComponent original = transform;
            if (ComponentEditors::drawTransform(transform)) {
                const TransformComponent edited = transform;
                transform = original;
                std::string beforeTransform;
                if (captureSnapshot(context, beforeTransform)) {
                    transform = edited;
                    commitMutation(context, "Edit Transform", "transform:" + std::to_string(m_selection.front().value),
                                   std::move(beforeTransform));
                }
            }
        }
    }
    drawComponentControls(context, context.scene.findEntity(m_selection.front()));
}

void EditorSession::drawComponentControls(ApplicationContext& context, Entity entity) {
    const SceneEntityId entityId = context.scene.id(entity);
    const auto section = [this, &context, entityId]<typename Component>(const char* name, auto draw) {
        Entity entity = context.scene.findEntity(entityId);
        entt::registry& registry = context.scene.registry();
        Component* component = registry.try_get<Component>(entity);
        if (component == nullptr) {
            return;
        }
        ImGui::PushID(name);
        const bool open = ImGui::CollapsingHeader(name, ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::SameLine();
        const bool remove = ImGui::SmallButton("Remove");
        if (open && !remove) {
            const Component original = *component;
            if (draw(*component)) {
                const Component edited = *component;
                *component = original;
                std::string before;
                if (captureSnapshot(context, before)) {
                    *component = edited;
                    commitMutation(context, std::string("Edit ") + name,
                                   "component:" + std::to_string(entityId.value) + ':' + name, std::move(before));
                }
            }
        }
        if (remove) {
            std::string before;
            if (captureSnapshot(context, before)) {
                registry.remove<Component>(entity);
                commitMutation(context, std::string("Remove ") + name, {}, std::move(before));
            }
        }
        ImGui::PopID();
    };
    section.operator()<CameraComponent>("Camera", ComponentEditors::drawCamera);
    section.operator()<DirectionalLightComponent>("Directional Light", ComponentEditors::drawDirectionalLight);
    section.operator()<EnvironmentLightComponent>("Environment Light", ComponentEditors::drawEnvironmentLight);
    section.operator()<ColliderComponent>("Collider", ComponentEditors::drawCollider);
    section.operator()<RigidBodyComponent>("Rigid Body", ComponentEditors::drawRigidBody);
    section.operator()<AnimationComponent>("Animation", [&context, entityId](AnimationComponent& animation) {
        const Entity current = context.scene.findEntity(entityId);
        const auto* modelComponent = context.scene.registry().try_get<ModelComponent>(current);
        const ModelAsset* model = modelComponent ? context.assets.model(modelComponent->model) : nullptr;
        return ComponentEditors::drawAnimation(animation, model);
    });
    section.operator()<MorphWeightsComponent>("Morph Weights", ComponentEditors::drawMorphWeights);

    entity = context.scene.findEntity(entityId);
    entt::registry& registry = context.scene.registry();
    if (const auto* model = registry.try_get<ModelComponent>(entity);
        model != nullptr && context.assets.model(model->model) == nullptr) {
        ImGui::TextColored({1.0f, 0.35f, 0.35f, 1.0f}, "Model asset reference is invalid or missing.");
    }
    if (const auto* mesh = registry.try_get<MeshRendererComponent>(entity);
        mesh != nullptr && context.assets.mesh(mesh->mesh) == nullptr) {
        ImGui::TextColored({1.0f, 0.35f, 0.35f, 1.0f}, "Mesh asset reference is invalid or missing.");
    }

    ImGui::Button("Drop Model Asset to Add Child", {-1.0f, 0.0f});
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ZENITH_MODEL_ASSET")) {
            instantiateModelAsset(context, static_cast<const char*>(payload->Data), entity);
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::Separator();
    if (ImGui::Button("Add Component")) {
        ImGui::OpenPopup("AddComponentPopup");
    }
    if (ImGui::BeginPopup("AddComponentPopup")) {
        const auto add = [this, &context, entityId]<typename Component>(const char* name) {
            const Entity current = context.scene.findEntity(entityId);
            if (!context.scene.registry().any_of<Component>(current) && ImGui::MenuItem(name)) {
                performMutation(context, std::string("Add ") + name, {}, [&] {
                    context.scene.registry().emplace<Component>(context.scene.findEntity(entityId));
                    return SceneResult{.success = true};
                });
            }
        };
        add.operator()<CameraComponent>("Camera");
        add.operator()<DirectionalLightComponent>("Directional Light");
        add.operator()<EnvironmentLightComponent>("Environment Light");
        add.operator()<ColliderComponent>("Collider");
        add.operator()<RigidBodyComponent>("Rigid Body");
        add.operator()<AnimationComponent>("Animation");
        add.operator()<MorphWeightsComponent>("Morph Weights");
        ImGui::EndPopup();
    }
}

bool EditorSession::selected(SceneEntityId id) const {
    return std::ranges::find(m_selection, id) != m_selection.end();
}

void EditorSession::select(SceneEntityId id, bool additive) {
    if (!additive) {
        m_selection.clear();
    }
    if (const auto found = std::ranges::find(m_selection, id); found != m_selection.end()) {
        if (additive) {
            m_selection.erase(found);
        }
    } else {
        m_selection.push_back(id);
    }
}

void EditorSession::removeInvalidSelection(const Scene& scene) {
    std::erase_if(m_selection, [&scene](SceneEntityId id) { return scene.findEntity(id) == entt::null; });
}

void EditorSession::drawStatusBar(const RenderStats& stats) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float height = ImGui::GetFrameHeight();
    ImGui::SetNextWindowPos({viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - height});
    ImGui::SetNextWindowSize({viewport->WorkSize.x, height});
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("##EditorStatus", nullptr, flags)) {
        const char* modeNames[] = {"Select", "Move", "Rotate", "Scale"};
        const char* spaceNames[] = {"Global", "Local"};
        if (m_diagnostic.empty()) {
            ImGui::Text("%s | Ready", m_document.displayName().c_str());
        } else {
            ImGui::Text("%s | Diagnostics available", m_document.displayName().c_str());
        }
        ImGui::SameLine();
        ImGui::Text("| Mode: %s | Space: %s | Snap: %s", modeNames[static_cast<int>(m_gizmoMode)],
                    spaceNames[static_cast<int>(m_transformSpace)], m_gizmoSnapping ? "On" : "Off");
        ImGui::SameLine();
        ImGui::Text("| Selection: %zu | Draws %u | Visible %u | GPU %.2f ms", m_selection.size(), stats.drawCalls,
                    stats.visibleCount, stats.gpuFrameMilliseconds);
        ImGui::SameLine();
        const char* playModeNames[] = {"Edit", "Play", "Paused"};
        ImGui::Text("| %s", playModeNames[static_cast<int>(m_playMode.mode())]);
    }
    ImGui::End();
}

void EditorSession::drawPathDialog(ApplicationContext& context) {
    if (m_openPathDialog) {
        ImGui::OpenPopup(m_pathDialogSaves ? "Save Scene As" : "Open Scene");
        m_openPathDialog = false;
    }

    bool open = true;
    const char* title = m_pathDialogSaves ? "Save Scene As" : "Open Scene";
    if (!ImGui::BeginPopupModal(title, &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::InputText("Path", m_pathBuffer.data(), m_pathBuffer.size());
    if (ImGui::Button(m_pathDialogSaves ? "Save" : "Open")) {
        const std::filesystem::path path(m_pathBuffer.data());
        const SceneDocumentResult result = m_pathDialogSaves ? m_document.saveAs(path, context.scene, context.assets)
                                                             : m_document.open(path, context.scene, context.assets);
        if (!result) {
            reportDiagnostic(result.error);
        } else {
            if (!m_pathDialogSaves) {
                m_history.crossBoundary(CommandHistoryBoundary::OpenDocument);
                m_selection.clear();
            } else {
                m_history.crossBoundary(CommandHistoryBoundary::SaveDocument);
            }
            syncDocumentDirtyState();
            m_diagnostic.clear();
            std::string recoveryError;
            if (!m_recovery.discard(recoveryError)) {
                reportDiagnostic(recoveryError);
            }
            ImGui::CloseCurrentPopup();
            if (m_pathDialogSaves && m_continuePendingAfterSaveAs) {
                m_continuePendingAfterSaveAs = false;
                finishPendingAction(context);
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void EditorSession::drawUnsavedChangesDialog(ApplicationContext& context) {
    if (m_openUnsavedDialog) {
        ImGui::OpenPopup("Unsaved Changes");
        m_openUnsavedDialog = false;
    }
    if (!ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::TextWrapped("%s has unsaved changes.", m_document.displayName().c_str());
    if (ImGui::Button("Save")) {
        if (m_document.path()) {
            if (saveDocument(context)) {
                ImGui::CloseCurrentPopup();
                finishPendingAction(context);
            }
        } else {
            m_continuePendingAfterSaveAs = true;
            requestPathDialog(true);
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard")) {
        std::string ignored;
        m_recovery.discard(ignored);
        ImGui::CloseCurrentPopup();
        finishPendingAction(context);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        m_pendingAction = PendingAction::None;
        m_pendingOpenPath.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void EditorSession::drawRecoveryDialog(ApplicationContext& context) {
    if (m_openRecoveryDialog) {
        ImGui::OpenPopup("Recover Autosave");
        m_openRecoveryDialog = false;
    }
    if (!ImGui::BeginPopupModal("Recover Autosave", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::TextWrapped("A recovery document is available from the previous editor session.");
    if (ImGui::Button("Recover")) {
        if (const SceneDocumentResult result = m_recovery.recover(context.scene, context.assets, m_document.options());
            !result) {
            reportDiagnostic("Recovery failed: " + result.error);
        } else {
            m_document.markDirty();
            m_history.crossBoundary(CommandHistoryBoundary::OpenDocument);
            m_document.markDirty();
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard Recovery")) {
        std::string error;
        if (!m_recovery.discard(error)) {
            reportDiagnostic(error);
        } else {
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndPopup();
}

void EditorSession::requestPathDialog(bool saveAs) {
    m_pathDialogSaves = saveAs;
    m_pathBuffer.fill('\0');
    if (m_document.path()) {
        const std::string path = m_document.path()->string();
        const size_t count = std::min(path.size(), m_pathBuffer.size() - 1);
        std::copy_n(path.data(), count, m_pathBuffer.data());
    }
    m_openPathDialog = true;
}

void EditorSession::requestNew(ApplicationContext& context) {
    m_pendingOpenPath.reset();
    m_pendingAction = PendingAction::NewDocument;
    if (m_document.dirty()) {
        m_openUnsavedDialog = true;
    } else {
        finishPendingAction(context);
    }
}

void EditorSession::requestOpen(ApplicationContext& context) {
    m_pendingOpenPath.reset();
    m_pendingAction = PendingAction::OpenDocument;
    if (m_document.dirty()) {
        m_openUnsavedDialog = true;
    } else {
        finishPendingAction(context);
    }
}

void EditorSession::requestOpenPath(ApplicationContext& context, std::filesystem::path path) {
    m_pendingOpenPath = std::move(path);
    m_pendingAction = PendingAction::OpenDocument;
    if (m_document.dirty()) {
        m_openUnsavedDialog = true;
    } else {
        finishPendingAction(context);
    }
}

void EditorSession::requestClose(ApplicationContext& context) {
    m_pendingOpenPath.reset();
    if (m_playMode.mode() != EditorMode::Edit) {
        stopPlayMode(context);
    }
    m_pendingAction = PendingAction::CloseEditor;
    if (m_document.dirty()) {
        m_openUnsavedDialog = true;
    } else {
        finishPendingAction(context);
    }
}

void EditorSession::finishPendingAction(ApplicationContext& context) {
    const PendingAction action = m_pendingAction;
    m_pendingAction = PendingAction::None;
    if (action == PendingAction::NewDocument) {
        m_document.createNew(context.scene);
        m_history.crossBoundary(CommandHistoryBoundary::NewDocument);
        m_selection.clear();
        std::string ignored;
        m_recovery.discard(ignored);
    } else if (action == PendingAction::OpenDocument) {
        requestPathDialog(false);
        if (m_pendingOpenPath) {
            m_pathBuffer.fill('\0');
            const std::string path = m_pendingOpenPath->string();
            const size_t count = std::min(path.size(), m_pathBuffer.size() - 1);
            std::copy_n(path.data(), count, m_pathBuffer.data());
            m_pendingOpenPath.reset();
        }
    } else if (action == PendingAction::CloseEditor) {
        context.control.requestQuit();
    }
}

bool EditorSession::saveDocument(ApplicationContext& context) {
    if (!m_document.path()) {
        requestPathDialog(true);
        return false;
    }
    if (const SceneDocumentResult result = m_document.save(context.scene, context.assets); !result) {
        reportDiagnostic("Save failed: " + result.error);
        return false;
    }
    m_history.crossBoundary(CommandHistoryBoundary::SaveDocument);
    std::string recoveryError;
    if (!m_recovery.discard(recoveryError)) {
        reportDiagnostic(recoveryError);
    }
    m_diagnostic.clear();
    return true;
}

void EditorSession::copySelection(ApplicationContext& context) {
    if (m_selection.empty()) {
        return;
    }
    if (const SceneDocumentResult result =
            SceneFragment::copy(context.scene, context.assets, m_document.options(), m_selection, m_clipboardFragment);
        !result) {
        reportDiagnostic("Copy failed: " + result.error);
        return;
    }
    ImGui::SetClipboardText(m_clipboardFragment.c_str());
}

void EditorSession::pasteClipboard(ApplicationContext& context) {
    const char* systemClipboard = ImGui::GetClipboardText();
    const std::string_view fragment =
        systemClipboard != nullptr && *systemClipboard != '\0' ? systemClipboard : m_clipboardFragment;
    if (fragment.empty()) {
        return;
    }
    std::string before;
    if (!captureSnapshot(context, before)) {
        return;
    }
    std::vector<SceneEntityId> pasted;
    if (const SceneDocumentResult result =
            SceneFragment::paste(fragment, context.scene, context.assets, m_document.options(), entt::null, pasted);
        !result) {
        reportDiagnostic("Paste failed: " + result.error);
        return;
    }
    commitMutation(context, "Paste Entities", {}, std::move(before));
    m_selection = std::move(pasted);
}

void EditorSession::duplicateSelection(ApplicationContext& context) {
    if (m_selection.empty()) {
        return;
    }
    std::string fragment;
    if (const SceneDocumentResult result =
            SceneFragment::copy(context.scene, context.assets, m_document.options(), m_selection, fragment);
        !result) {
        reportDiagnostic("Duplicate failed: " + result.error);
        return;
    }
    std::string before;
    if (!captureSnapshot(context, before)) {
        return;
    }
    std::vector<SceneEntityId> duplicates;
    Entity duplicateParent = entt::null;
    if (m_selection.size() == 1) {
        const Entity source = context.scene.findEntity(m_selection.front());
        if (source != entt::null) {
            duplicateParent = context.scene.registry().get<RelationshipComponent>(source).parent;
        }
    }
    if (const SceneDocumentResult result = SceneFragment::paste(fragment, context.scene, context.assets,
                                                                m_document.options(), duplicateParent, duplicates);
        !result) {
        reportDiagnostic("Duplicate failed: " + result.error);
        return;
    }
    commitMutation(context, "Duplicate Entities", {}, std::move(before));
    m_selection = std::move(duplicates);
}

bool EditorSession::captureSnapshot(ApplicationContext& context, std::string& snapshot) {
    const SceneDocumentResult result =
        SceneDocument::serialize(context.scene, context.assets, m_document.options(), snapshot);
    if (!result) {
        reportDiagnostic(result.error);
        return false;
    }
    return true;
}

void EditorSession::commitMutation(ApplicationContext& context, std::string name, std::string coalesceKey,
                                   std::string before) {
    std::string after;
    if (!captureSnapshot(context, after)) {
        return;
    }
    if (before == after) {
        return;
    }
    if (const SceneDocumentResult result =
            SceneDocument::deserialize(before, context.assets, m_document.options(), context.scene);
        !result) {
        reportDiagnostic("Could not prepare editor command: " + result.error);
        return;
    }
    EditorCommandContext command = commandContext(context);
    if (const EditorCommandResult result =
            m_history.execute(std::make_unique<SceneStateCommand>(std::move(name), std::move(before), std::move(after),
                                                                  std::move(coalesceKey)),
                              command);
        !result) {
        reportDiagnostic(result.error);
        return;
    }
    syncDocumentDirtyState();
}

void EditorSession::performMutation(ApplicationContext& context, std::string name, std::string coalesceKey,
                                    const std::function<SceneResult()>& mutation) {
    std::string before;
    if (!captureSnapshot(context, before)) {
        return;
    }
    if (const SceneResult result = mutation(); !result) {
        reportDiagnostic(result.error);
        return;
    }
    commitMutation(context, std::move(name), std::move(coalesceKey), std::move(before));
}

EditorCommandContext EditorSession::commandContext(ApplicationContext& context) {
    return {context.scene, context.assets, m_document.options()};
}

void EditorSession::syncDocumentDirtyState() {
    m_document.setDirty(m_history.dirty());
}

void EditorSession::reportDiagnostic(std::string message) {
    m_diagnostic = std::move(message);
    m_diagnostics.add(EditorDiagnosticSeverity::Error, "Editor", m_diagnostic,
                      "Review the operation, correct the path or data, then retry.");
}

void EditorSession::refreshAssetCatalog() {
    AssetCatalogResult result = m_assetCatalog.refresh();
    if (!result) {
        reportDiagnostic(result.error);
        return;
    }
    if (m_assetCatalogInitialized && !result.changes.empty()) {
        for (const AssetCatalogChange& change : result.changes) {
            if (change.asset.kind == EditorAssetKind::Texture) {
                m_thumbnailCache.invalidate(change.asset.absolutePath);
            }
        }
        reportDiagnostic(std::to_string(result.changes.size()) +
                         " project asset change(s) detected. Select a changed model and choose Reload Model to "
                         "reload it explicitly.");
    }
    m_assetCatalogInitialized = true;
}

void EditorSession::instantiateModelAsset(ApplicationContext& context, const std::filesystem::path& path,
                                          Entity parent) {
    if (m_playMode.mode() != EditorMode::Edit) {
        reportDiagnostic("Assets can only be placed in Edit mode");
        return;
    }
    const AssetLoadResult<ModelHandle> loaded = context.assets.loadModel(path);
    if (!loaded) {
        reportDiagnostic("Could not place model: " + loaded.error);
        return;
    }
    std::string before;
    if (!captureSnapshot(context, before)) {
        return;
    }
    const Entity root = context.scene.createEntity(path.stem().string());
    if (parent != entt::null) {
        context.scene.setParent(root, parent);
    }
    if (const SceneResult result = context.scene.instantiateModel(root, loaded.handle, context.assets); !result) {
        context.scene.destroyEntity(root);
        reportDiagnostic("Could not place model: " + result.error);
        return;
    }
    const SceneEntityId id = context.scene.id(root);
    commitMutation(context, "Place Model", {}, std::move(before));
    if (context.scene.findEntity(id) != entt::null) {
        m_selection = {id};
    }
}

void EditorSession::enterPlayMode(ApplicationContext& context) {
    context.physics.clear();
    context.audio.stopSceneVoices();
    if (const SceneDocumentResult result =
            m_playMode.enter(context.scene, context.assets, m_document.options(), m_selection);
        !result) {
        reportDiagnostic("Could not enter Play mode: " + result.error);
        return;
    }
    m_history.crossBoundary(CommandHistoryBoundary::EnterPlayMode);
    m_diagnostic.clear();
}

void EditorSession::stopPlayMode(ApplicationContext& context) {
    if (m_playMode.mode() == EditorMode::Edit) {
        return;
    }
    context.audio.stopSceneVoices();
    context.physics.clear();
    m_playMode.stop(context.scene, m_selection);
    m_history.crossBoundary(CommandHistoryBoundary::LeavePlayMode);
    syncDocumentDirtyState();
}

void EditorSession::focusOnSelection(ApplicationContext& context) {
    if (m_selection.empty()) {
        return;
    }
    const Entity entity = context.scene.findEntity(m_selection.front());
    if (entity == entt::null) {
        return;
    }

    context.scene.updateWorldTransforms();
    const auto& transform = context.scene.registry().get<TransformComponent>(entity);
    const glm::vec3 position = glm::vec3(transform.world[3]);

    // Calculate bounds radius from mesh if available
    float radius = 1.0f;
    if (const auto* meshRenderer = context.scene.registry().try_get<MeshRendererComponent>(entity)) {
        const MeshAsset* mesh = context.assets.mesh(meshRenderer->mesh);
        if (mesh) {
            const float scale =
                std::max({glm::length(glm::vec3(transform.world[0])), glm::length(glm::vec3(transform.world[1])),
                          glm::length(glm::vec3(transform.world[2]))});
            radius = mesh->bounds.radius * scale;
        }
    }

    m_camera.focusOnBounds(position, radius);
}
