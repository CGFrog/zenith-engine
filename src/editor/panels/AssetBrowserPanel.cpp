#include "AssetBrowserPanel.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/application/FileDropQueue.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/interaction/AssetReferenceField.hpp"
#include "editor/interaction/ReflectedFieldEditor.hpp"
#include "editor/panels/AssetDragPayloads.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/project/ProjectFileOperations.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"
#include "engine/assets/TextureImportSettings.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <utility>

namespace {
constexpr float folderPaneWidth = 200.0f;
constexpr float gridTileWidth = 96.0f;
constexpr float gridTileHeight = 72.0f;
constexpr const char* modelPayloadType = modelAssetPayload;
constexpr const char* prefabPayloadType = prefabAssetPayload;
// Any other file or folder dragged within the browser, to move it.
constexpr const char* itemPayloadType = "ZENITH_PROJECT_ITEM";
#if defined(_WIN32)
constexpr const char* revealLabel = "Show in Explorer";
constexpr const char* trashName = "Recycle Bin";
#else
constexpr const char* revealLabel = "Show in File Manager";
constexpr const char* trashName = "trash";
#endif
// In EditorAssetKind order, so a kind's value indexes the type filter.
constexpr std::array<EditorAssetKind, 8> filterableKinds{
    EditorAssetKind::Model,  EditorAssetKind::Texture, EditorAssetKind::Audio,    EditorAssetKind::Scene,
    EditorAssetKind::Prefab, EditorAssetKind::Script,  EditorAssetKind::Material, EditorAssetKind::Data};
constexpr std::array<TextureUsage, 3> textureUsages{TextureUsage::Color, TextureUsage::Normal, TextureUsage::Data};

// Every payload a folder accepts to move the dragged item, with whether its
// path is UTF-8 (the others use the narrow encoding).
struct MovablePayload {
    const char* type;
    bool utf8;
};

constexpr std::array<MovablePayload, 6> movablePayloads{{{itemPayloadType, false},
                                                         {modelPayloadType, false},
                                                         {prefabPayloadType, false},
                                                         {textureAssetPayload, true},
                                                         {audioAssetPayload, true},
                                                         {materialAssetPayload, true}}};

EditorIcon assetIcon(const EditorAssetRecord* asset) {
    if (asset == nullptr) {
        return EditorIcon::Folder;
    }
    switch (asset->kind) {
    case EditorAssetKind::Model:
        return EditorIcon::Mesh;
    case EditorAssetKind::Texture:
        return EditorIcon::Texture;
    case EditorAssetKind::Audio:
        return EditorIcon::AudioClip;
    case EditorAssetKind::Scene:
        return EditorIcon::Scene;
    case EditorAssetKind::Prefab:
        return EditorIcon::Components;
    case EditorAssetKind::Script:
        return EditorIcon::Script;
    case EditorAssetKind::Material:
        return EditorIcon::Environment;
    case EditorAssetKind::Data:
        return EditorIcon::Statistics;
    }
    return EditorIcon::Empty;
}

const char* kindName(EditorAssetKind kind) {
    switch (kind) {
    case EditorAssetKind::Model:
        return "Model";
    case EditorAssetKind::Texture:
        return "Texture";
    case EditorAssetKind::Audio:
        return "Audio";
    case EditorAssetKind::Scene:
        return "Scene";
    case EditorAssetKind::Prefab:
        return "Prefab";
    case EditorAssetKind::Script:
        return "Script";
    case EditorAssetKind::Material:
        return "Material";
    case EditorAssetKind::Data:
        return "Data";
    }
    return "Asset";
}

const char* textureUsageLabel(TextureUsage usage) {
    switch (usage) {
    case TextureUsage::Color:
        return "Color (sRGB)";
    case TextureUsage::Normal:
        return "Normal map (linear)";
    case TextureUsage::Data:
        return "Data (linear)";
    }
    return "Color (sRGB)";
}

// Scenes are opened and scripts compiled; nothing reads them again.
bool reimportable(EditorAssetKind kind) {
    return kind != EditorAssetKind::Scene && kind != EditorAssetKind::Script;
}

const char* referrerLabel(EditorAssetKind kind) {
    switch (kind) {
    case EditorAssetKind::Scene:
        return "Scene";
    case EditorAssetKind::Prefab:
        return "Prefab";
    case EditorAssetKind::Material:
        return "Material";
    default:
        return "File";
    }
}

std::string lowercase(std::string text) {
    std::ranges::transform(text, text.begin(),
                           [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return text;
}

std::string genericUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return {text.begin(), text.end()};
}

// True when path is base or lies inside it.
bool isWithin(const std::filesystem::path& path, const std::filesystem::path& base) {
    const std::filesystem::path relative = path.lexically_relative(base);
    return !relative.empty() && *relative.begin() != "..";
}

// Moves path from under oldBase to the same place under newBase.
std::filesystem::path rebase(const std::filesystem::path& path, const std::filesystem::path& oldBase,
                             const std::filesystem::path& newBase) {
    return normalizedCatalogPath(newBase / path.lexically_relative(oldBase));
}

std::string byteSizeText(uintmax_t bytes) {
    constexpr std::array<const char*, 4> units{"B", "KB", "MB", "GB"};
    double size = static_cast<double>(bytes);
    size_t unit = 0;
    while (size >= 1024.0 && unit + 1 < units.size()) {
        size /= 1024.0;
        ++unit;
    }
    char text[32];
    std::snprintf(text, sizeof(text), unit == 0 ? "%.0f %s" : "%.1f %s", size, units[unit]);
    return text;
}

// Shortens text with a trailing "..." so it fits width.
std::string fitText(const std::string& text, float width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width) {
        return text;
    }
    std::string shortened = text;
    while (!shortened.empty() && ImGui::CalcTextSize((shortened + "...").c_str()).x > width) {
        // Drop a whole UTF-8 sequence so the text stays valid.
        do {
            shortened.pop_back();
        } while (!shortened.empty() && (static_cast<unsigned char>(shortened.back()) & 0xC0) == 0x80);
        if (!shortened.empty() && static_cast<unsigned char>(shortened.back()) >= 0xC0) {
            shortened.pop_back();
        }
    }
    return shortened + "...";
}
} // namespace

AssetBrowserPanel::AssetBrowserPanel(ProjectAssets& project, AssetBrowserRole role)
    : m_project(project)
    , m_viewId(project.registerView())
    , m_viewMode(project.settings().viewMode)
    , m_followedItemMove(project.latestItemMoveSequence()) {
    if (role == AssetBrowserRole::Primary) {
        m_project.claimPrimaryView(m_viewId);
    }
    const std::string& lastFolder = project.settings().lastFolder;
    m_currentFolder = lastFolder.empty() ? project.catalog().roots().front() : project.resolveFolderKey(lastFolder);
    keepStateValid();
}

AssetBrowserPanel::~AssetBrowserPanel() {
    m_project.releasePrimaryView(m_viewId);
}

const char* AssetBrowserPanel::title() const {
    return "Asset Browser";
}

void AssetBrowserPanel::update(EditorContext&, const InputState&, float) {
    // The session polls and dispatches project changes; views only follow.
    syncWithProject();
}

void AssetBrowserPanel::refresh(EditorDiagnostics& diagnostics) {
    m_project.refresh(diagnostics);
    syncWithProject();
}

void AssetBrowserPanel::syncWithProject() {
    followItemMoves();
    keepStateValid();
}

// Carries the current folder and selection along renames and moves made
// through the service, by this browser or another.
void AssetBrowserPanel::followItemMoves() {
    for (const ProjectItemMove& move : m_project.recentItemMoves()) {
        if (move.sequence <= m_followedItemMove) {
            continue;
        }
        if (isWithin(m_currentFolder, move.source)) {
            m_currentFolder = rebase(m_currentFolder, move.source, move.destination);
            m_revealCurrentInTree = true;
        }
        if (!m_selected.empty() && isWithin(m_selected, move.source)) {
            m_selected = rebase(m_selected, move.source, move.destination);
        }
    }
    m_followedItemMove = m_project.latestItemMoveSequence();
}

// The current folder falls back to its nearest surviving ancestor and a
// vanished selection is cleared. Before the first scan there is nothing to
// check against, so the state is left alone.
void AssetBrowserPanel::keepStateValid() {
    const AssetCatalog& catalog = m_project.catalog();
    if (!m_project.scanned()) {
        return;
    }
    std::filesystem::path folder = m_currentFolder;
    while (!catalog.findFolder(folder)) {
        const bool insideRoot = std::ranges::any_of(
            catalog.roots(), [&folder](const std::filesystem::path& root) { return isWithin(folder, root); });
        if (!insideRoot || !folder.has_relative_path() || folder == folder.parent_path()) {
            folder = catalog.folders().empty() ? catalog.roots().front() : catalog.folders().front().absolutePath;
            break;
        }
        folder = folder.parent_path();
    }
    if (folder != m_currentFolder) {
        m_currentFolder = std::move(folder);
        m_revealCurrentInTree = true;
    }
    if (!m_selected.empty() && !catalog.findAsset(m_selected) && !catalog.findFolder(m_selected)) {
        m_selected.clear();
    }
}

void AssetBrowserPanel::select(const std::optional<std::filesystem::path>& produced) {
    syncWithProject();
    if (produced) {
        m_selected = *produced;
    }
}

void AssetBrowserPanel::rememberView(EditorDiagnostics& diagnostics) {
    if (m_project.isPrimaryView(m_viewId)) {
        m_project.rememberView(m_currentFolder, m_viewMode, diagnostics);
    }
}

std::string AssetBrowserPanel::rootLabel(const std::filesystem::path& root) const {
    const std::string name = utf8PathText(root.filename());
    return name.empty() ? utf8PathText(root) : name;
}

bool AssetBrowserPanel::isRoot(const std::filesystem::path& path) const {
    return m_project.isRoot(path);
}

const AssetCatalog& AssetBrowserPanel::catalog() const {
    return m_project.catalog();
}

AssetBrowserRole AssetBrowserPanel::role() const {
    return m_project.isPrimaryView(m_viewId) ? AssetBrowserRole::Primary : AssetBrowserRole::Secondary;
}

const std::filesystem::path& AssetBrowserPanel::currentFolder() const {
    return m_currentFolder;
}

void AssetBrowserPanel::navigateTo(const std::filesystem::path& folder, EditorDiagnostics& diagnostics) {
    const std::filesystem::path normalized = normalizedCatalogPath(folder);
    if (!m_project.catalog().findFolder(normalized)) {
        diagnostics.reportError("Cannot open folder '" + utf8PathText(normalized) + "': it is not a project folder.");
        return;
    }
    m_currentFolder = normalized;
    m_revealCurrentInTree = true;
    rememberView(diagnostics);
}

void AssetBrowserPanel::revealAsset(const std::filesystem::path& item, EditorDiagnostics& diagnostics) {
    const std::filesystem::path normalized = normalizedCatalogPath(item);
    navigateTo(normalized.parent_path(), diagnostics);
    if (m_currentFolder == normalizedCatalogPath(normalized.parent_path())) {
        m_selected = normalized;
        m_search.clear();
    }
}

AssetBrowserViewMode AssetBrowserPanel::viewMode() const {
    return m_viewMode;
}

void AssetBrowserPanel::setViewMode(AssetBrowserViewMode mode, EditorDiagnostics& diagnostics) {
    if (m_viewMode != mode) {
        m_viewMode = mode;
        rememberView(diagnostics);
    }
}

bool AssetBrowserPanel::isFavourite(const std::filesystem::path& folder) const {
    return m_project.isFavourite(folder);
}

void AssetBrowserPanel::toggleFavourite(const std::filesystem::path& folder, EditorDiagnostics& diagnostics) {
    m_project.toggleFavourite(folder, diagnostics);
}

const std::filesystem::path& AssetBrowserPanel::selectedPath() const {
    return m_selected;
}

void AssetBrowserPanel::setSearchText(std::string text) {
    m_search = std::move(text);
}

void AssetBrowserPanel::setKindShown(EditorAssetKind kind, bool shown) {
    m_shownKinds[static_cast<size_t>(kind)] = shown;
}

bool AssetBrowserPanel::openAsset(const std::filesystem::path& item, const AssetBrowserActions& actions) {
    const EditorAssetRecord* asset = m_project.catalog().findAsset(normalizedCatalogPath(item));
    if (asset == nullptr) {
        return false;
    }
    if (asset->kind == EditorAssetKind::Scene && actions.openScene) {
        actions.openScene(asset->absolutePath);
        return true;
    }
    if (asset->kind == EditorAssetKind::Script && actions.openScript) {
        actions.openScript(asset->absolutePath);
        return true;
    }
    if (asset->kind == EditorAssetKind::Prefab && actions.openPrefab) {
        actions.openPrefab(asset->absolutePath);
        return true;
    }
    return false;
}

void AssetBrowserPanel::createFolder(EditorDiagnostics& diagnostics) {
    select(m_project.createFolder(m_currentFolder, diagnostics));
}

void AssetBrowserPanel::createScene(EditorDiagnostics& diagnostics) {
    select(m_project.createScene(m_currentFolder, diagnostics));
}

void AssetBrowserPanel::createMaterial(EditorDiagnostics& diagnostics) {
    select(m_project.createMaterial(m_currentFolder, diagnostics));
}

void AssetBrowserPanel::renameItem(const std::filesystem::path& item, const std::string& utf8NewName,
                                   EditorDiagnostics& diagnostics) {
    select(m_project.renameItem(item, utf8NewName, diagnostics));
}

void AssetBrowserPanel::moveItem(const std::filesystem::path& item, const std::filesystem::path& folder,
                                 EditorDiagnostics& diagnostics) {
    select(m_project.moveItem(item, folder, diagnostics));
}

void AssetBrowserPanel::duplicateItem(const std::filesystem::path& item, EditorDiagnostics& diagnostics) {
    select(m_project.duplicateItem(item, diagnostics));
}

void AssetBrowserPanel::moveItemToTrash(const std::filesystem::path& item, EditorDiagnostics& diagnostics) {
    m_project.moveItemToTrash(item, diagnostics);
    syncWithProject();
}

void AssetBrowserPanel::revealItem(const std::filesystem::path& item, EditorDiagnostics& diagnostics) {
    m_project.revealItem(item, diagnostics);
}

void AssetBrowserPanel::importFiles(std::span<const std::filesystem::path> sources, const std::filesystem::path& folder,
                                    EditorDiagnostics& diagnostics) {
    const std::vector<std::filesystem::path> imported = m_project.importFiles(sources, folder, diagnostics);
    select(imported.empty() ? std::nullopt : std::optional<std::filesystem::path>(imported.back()));
}

void AssetBrowserPanel::reimport(const std::filesystem::path& item, EditorDiagnostics& diagnostics) {
    if (!m_project.queueReimport(item)) {
        diagnostics.add(EditorDiagnosticSeverity::Info, "Asset Browser",
                        "'" + utf8PathText(item.filename()) + "' has nothing to reimport.");
    }
}

void AssetBrowserPanel::findReferences(const std::filesystem::path& item, const AssetBrowserActions& actions,
                                       EditorDiagnostics& diagnostics) {
    m_references = {.item = normalizedCatalogPath(item), .files = m_project.findReferences(item, diagnostics)};
    if (const EditorAssetRecord* asset = m_project.catalog().findAsset(m_references.item);
        asset != nullptr && actions.sceneUsers) {
        m_references.sceneEntities = actions.sceneUsers(*asset);
    }
    m_openModal = OpenModal::References;
    m_modalNeedsOpening = true;
    m_modalTarget = m_references.item;
}

void AssetBrowserPanel::requestDelete(const std::filesystem::path& item, EditorDiagnostics& diagnostics) {
    m_deleteReferrers = m_project.findReferences(item, diagnostics);
    m_openModal = OpenModal::Delete;
    m_modalNeedsOpening = true;
    m_modalTarget = normalizedCatalogPath(item);
}

const AssetBrowserReferences& AssetBrowserPanel::references() const {
    return m_references;
}

std::span<const AssetReferrer> AssetBrowserPanel::deleteReferrers() const {
    return m_deleteReferrers;
}

AssetBrowserActions AssetBrowserPanel::sessionActions(EditorContext& context) {
    return {
        .openScene =
            [&context](const std::filesystem::path& path) { context.actions.requestOpenScene(context.app, path); },
        .reloadModel =
            [&context](const std::filesystem::path& path) {
                reloadProjectModel(context.app.assets, context.diagnostics, path);
            },
        // The runner reports a failed launch.
        .openScript =
            [&context](const std::filesystem::path& path) {
                context.invoke("module.open_source", nlohmann::json{{"file", utf8PathText(path)}}.dump());
            },
        // The runner reports why a prefab could not open.
        .openPrefab =
            [&context](const std::filesystem::path& path) {
                context.invoke("prefab.edit", nlohmann::json{{"asset", utf8PathText(path)}}.dump());
            },
        .reloadMaterial =
            [&context](const std::filesystem::path& path) {
                std::array<Scene*, 2> scenes{&context.app.scene, context.editing.editScene()};
                reloadProjectMaterial(context.app.assets, scenes, context.diagnostics, path);
            },
        .thumbnailTexture =
            [this, &context](const std::filesystem::path& image) {
                return m_project.thumbnailTexture(context.app.rendering, image).value;
            },
        .sceneUsers =
            [&context](const EditorAssetRecord& asset) {
                std::string document;
                if (!context.editing.capture(document)) {
                    return std::vector<SceneEntityId>{};
                }
                return sceneEntitiesReferencing(document, context.editing.document().options().assetRoot,
                                                asset.absolutePath, asset.guid);
            },
        .selectEntities =
            [&context](std::span<const SceneEntityId> entities) {
                context.selection.assign({entities.begin(), entities.end()});
            },
        .fileDrops = context.fileDrops,
        .definitionSchemas = context.definitionSchemas,
    };
}

void AssetBrowserPanel::draw(EditorContext& context) {
    drawBrowser(context.diagnostics, sessionActions(context));
}

void AssetBrowserPanel::hidden(EditorContext&) {
    // A shown browser takes over as the primary.
    m_project.releasePrimaryView(m_viewId);
}

void AssetBrowserPanel::drawBrowser(EditorDiagnostics& diagnostics, const AssetBrowserActions& actions) {
    m_project.reportPendingSettingsError(diagnostics);
    syncWithProject();
    const bool wasPrimary = m_project.isPrimaryView(m_viewId);
    if (m_project.claimPrimaryView(m_viewId) && !wasPrimary) {
        // Promoted: this browser's view is now the one remembered.
        rememberView(diagnostics);
    }
    if (const std::optional<std::filesystem::path> reveal = m_project.takeRevealRequest(m_viewId)) {
        revealAsset(*reveal, diagnostics);
    }
    m_frameDrops = actions.fileDrops;
    m_dropFolder.reset();
    pollImportDialog(diagnostics);
    if (widgets::Button("Refresh")) {
        refresh(diagnostics);
    }
    ImGui::SameLine();
    drawToolbar(diagnostics);
    drawBreadcrumbs();
    ImGui::Separator();

    const EditorAssetRecord* selectedAsset = m_selected.empty() ? nullptr : m_project.catalog().findAsset(m_selected);
    const EditorAssetKind selectedKind = selectedAsset != nullptr ? selectedAsset->kind : EditorAssetKind::Scene;
    const float detailRows =
        selectedAsset != nullptr && (selectedKind == EditorAssetKind::Material || selectedKind == EditorAssetKind::Data)
            ? 14.0f
        : selectedAsset != nullptr && selectedKind == EditorAssetKind::Texture ? 5.0f
        : m_selected.empty()                                                   ? 0.0f
                                                                               : 2.5f;
    const float detailsHeight = ImGui::GetFrameHeightWithSpacing() * detailRows;
    const float paneHeight =
        std::max(ImGui::GetContentRegionAvail().y - detailsHeight, ImGui::GetFrameHeightWithSpacing() * 4.0f);
    if (ImGui::BeginTable("##AssetBrowserLayout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Folders", ImGuiTableColumnFlags_WidthFixed, folderPaneWidth);
        ImGui::TableSetupColumn("Contents", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextColumn();
        if (ImGui::BeginChild("##Folders", ImVec2(0.0f, paneHeight))) {
            drawFolderPane();
        }
        ImGui::EndChild();
        ImGui::TableNextColumn();
        if (ImGui::BeginChild("##Contents", ImVec2(0.0f, paneHeight))) {
            drawContents(actions);
        }
        ImGui::EndChild();
        ImGui::EndTable();
    }
    drawSelectedAsset(actions, diagnostics);
    importDroppedFiles(diagnostics);
    runDeferredCommand(actions, diagnostics);
    drawModals(diagnostics);
    drawReferencesWindow(actions, diagnostics);
}

void AssetBrowserPanel::noteDropTarget(const std::filesystem::path& folder) {
    if (m_frameDrops == nullptr || m_dropFolder) {
        return;
    }
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    for (const DroppedFile& drop : m_frameDrops->pending()) {
        if (drop.position.x >= minimum.x && drop.position.y >= minimum.y && drop.position.x < maximum.x &&
            drop.position.y < maximum.y) {
            m_dropFolder = folder;
            return;
        }
    }
}

void AssetBrowserPanel::importDroppedFiles(EditorDiagnostics& diagnostics) {
    if (m_frameDrops == nullptr || m_frameDrops->empty()) {
        return;
    }
    const ImVec2 position = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    const std::vector<std::filesystem::path> dropped =
        m_frameDrops->claimWithin({position.x, position.y}, {position.x + size.x, position.y + size.y});
    if (!dropped.empty()) {
        importFiles(dropped, m_dropFolder.value_or(m_currentFolder), diagnostics);
    }
}

void AssetBrowserPanel::pollImportDialog(EditorDiagnostics& diagnostics) {
    const std::optional<FileDialogOutcome> outcome = m_importDialog.poll();
    if (!outcome) {
        return;
    }
    if (outcome->status == FileDialogStatus::Failed) {
        diagnostics.reportError(outcome->error);
    } else if (outcome->status == FileDialogStatus::Chosen) {
        importFiles(outcome->paths, m_importFolder, diagnostics);
    }
}

void AssetBrowserPanel::drawToolbar(EditorDiagnostics& diagnostics) {
    if (widgets::Button("New")) {
        ImGui::OpenPopup("##NewMenu");
    }
    if (ImGui::BeginPopup("##NewMenu")) {
        drawFolderMenu();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (widgets::Button("Import...")) {
        m_deferred = DeferredCommand{CommandKind::Import, m_currentFolder};
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Copy files into the current folder. You can also drop them here from Explorer.");
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Grid", m_viewMode == AssetBrowserViewMode::Grid)) {
        setViewMode(AssetBrowserViewMode::Grid, diagnostics);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("List", m_viewMode == AssetBrowserViewMode::List)) {
        setViewMode(AssetBrowserViewMode::List, diagnostics);
    }
    ImGui::SameLine();
    if (widgets::Button("Types")) {
        ImGui::OpenPopup("##TypeFilter");
    }
    if (ImGui::BeginPopup("##TypeFilter")) {
        for (size_t index = 0; index < filterableKinds.size(); ++index) {
            ImGui::Checkbox(kindName(filterableKinds[index]), &m_shownKinds[index]);
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##AssetFilter", "Search assets...", &m_search);
}

void AssetBrowserPanel::drawBreadcrumbs() {
    const auto root = std::ranges::find_if(m_project.catalog().roots(), [this](const std::filesystem::path& candidate) {
        return isWithin(m_currentFolder, candidate);
    });
    if (root == m_project.catalog().roots().end()) {
        ImGui::TextDisabled("%s", utf8PathText(m_currentFolder).c_str());
        return;
    }
    if (ImGui::SmallButton((rootLabel(*root) + "##Crumb").c_str())) {
        m_deferred = DeferredCommand{CommandKind::Navigate, *root};
    }
    std::filesystem::path crumb = *root;
    int index = 0;
    for (const std::filesystem::path& part : m_currentFolder.lexically_relative(*root)) {
        if (part == ".") {
            continue;
        }
        crumb /= part;
        ImGui::SameLine();
        ImGui::TextDisabled(">");
        ImGui::SameLine();
        ImGui::PushID(++index);
        if (ImGui::SmallButton(utf8PathText(part).c_str())) {
            m_deferred = DeferredCommand{CommandKind::Navigate, crumb};
        }
        ImGui::PopID();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::SmallButton(isFavourite(m_currentFolder) ? "Unfavourite" : "Favourite")) {
        m_deferred = DeferredCommand{CommandKind::ToggleFavourite, m_currentFolder};
    }
}

void AssetBrowserPanel::drawFolderPane() {
    if (widgets::SectionHeader("Favourites", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (m_project.settings().favourites.empty()) {
            ImGui::TextDisabled("Right-click a folder to add it.");
        }
        for (size_t index = 0; index < m_project.settings().favourites.size(); ++index) {
            const std::string& key = m_project.settings().favourites[index];
            const std::filesystem::path folder = m_project.resolveFolderKey(key);
            const bool exists = m_project.catalog().findFolder(folder) != nullptr;
            std::string label = isRoot(folder) ? rootLabel(folder) : utf8PathText(folder.filename());
            if (!exists) {
                label += " (missing)";
            }
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::Selectable(label.c_str(), folder == m_currentFolder) && exists) {
                m_deferred = DeferredCommand{CommandKind::Navigate, folder};
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip("%s", key.c_str());
            }
            if (ImGui::BeginPopupContextItem("##FavouriteMenu")) {
                if (ImGui::MenuItem("Remove from Favourites")) {
                    m_deferred = DeferredCommand{CommandKind::ToggleFavourite, folder};
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }
    if (widgets::SectionHeader("Folders", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const std::filesystem::path& root : m_project.catalog().roots()) {
            if (const EditorAssetFolder* folder = m_project.catalog().findFolder(root)) {
                drawFolderNode(*folder, rootLabel(root));
            } else {
                ImGui::TextDisabled("%s (missing)", rootLabel(root).c_str());
            }
        }
    }
    m_revealCurrentInTree = false;
}

void AssetBrowserPanel::drawFolderNode(const EditorAssetFolder& folder, const std::string& label) {
    const std::vector<const EditorAssetFolder*> children = m_project.catalog().subfolders(folder.absolutePath);
    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }
    if (folder.relativePath.empty()) {
        flags |= ImGuiTreeNodeFlags_DefaultOpen;
    }
    if (folder.absolutePath == m_currentFolder) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    if (m_revealCurrentInTree && folder.absolutePath != m_currentFolder &&
        isWithin(m_currentFolder, folder.absolutePath)) {
        ImGui::SetNextItemOpen(true);
    }
    const std::string id = genericUtf8(folder.absolutePath);
    const bool open = ImGui::TreeNodeEx(id.c_str(), flags, "%s", label.c_str());
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
        m_deferred = DeferredCommand{CommandKind::Navigate, folder.absolutePath};
    }
    acceptItemDrop(folder.absolutePath);
    noteDropTarget(folder.absolutePath);
    if (ImGui::BeginPopupContextItem()) {
        drawItemMenu(folder.absolutePath, true);
        ImGui::EndPopup();
    }
    if (open) {
        for (const EditorAssetFolder* child : children) {
            drawFolderNode(*child, utf8PathText(child->absolutePath.filename()));
        }
        ImGui::TreePop();
    }
}

std::vector<AssetBrowserPanel::BrowserEntry> AssetBrowserPanel::visibleEntries() const {
    const std::string search = lowercase(m_search);
    const auto matches = [&search](const std::filesystem::path& path) {
        return search.empty() || lowercase(utf8PathText(path.filename())).find(search) != std::string::npos;
    };
    const auto shown = [this](const EditorAssetRecord& asset) { return m_shownKinds[static_cast<size_t>(asset.kind)]; };
    std::vector<BrowserEntry> entries;
    if (search.empty()) {
        for (const EditorAssetFolder* folder : m_project.catalog().subfolders(m_currentFolder)) {
            entries.push_back({folder->absolutePath, utf8PathText(folder->absolutePath.filename()), nullptr});
        }
        for (const EditorAssetRecord* asset : m_project.catalog().assetsInFolder(m_currentFolder)) {
            if (shown(*asset)) {
                entries.push_back({asset->absolutePath, utf8PathText(asset->absolutePath.filename()), asset});
            }
        }
        return entries;
    }
    // Searching covers the current folder and everything below it; labels show
    // where each match lives.
    for (const EditorAssetFolder& folder : m_project.catalog().folders()) {
        if (folder.absolutePath != m_currentFolder && isWithin(folder.absolutePath, m_currentFolder) &&
            matches(folder.absolutePath)) {
            entries.push_back(
                {folder.absolutePath, genericUtf8(folder.absolutePath.lexically_relative(m_currentFolder)), nullptr});
        }
    }
    for (const EditorAssetRecord& asset : m_project.catalog().assets()) {
        if (isWithin(asset.absolutePath, m_currentFolder) && shown(asset) && matches(asset.absolutePath)) {
            entries.push_back(
                {asset.absolutePath, genericUtf8(asset.absolutePath.lexically_relative(m_currentFolder)), &asset});
        }
    }
    return entries;
}

void AssetBrowserPanel::drawContents(const AssetBrowserActions& actions) {
    const std::vector<BrowserEntry> entries = visibleEntries();
    if (entries.empty()) {
        ImGui::TextDisabled("%s", m_search.empty() ? "This folder is empty." : "No matching assets.");
    } else if (m_viewMode == AssetBrowserViewMode::Grid) {
        drawGrid(entries, actions);
    } else {
        drawList(entries, actions);
    }

    if (ImGui::BeginPopupContextWindow("##FolderMenu",
                                       ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        drawFolderMenu();
        ImGui::Separator();
        if (ImGui::MenuItem(isFavourite(m_currentFolder) ? "Remove from Favourites" : "Add to Favourites")) {
            m_deferred = DeferredCommand{CommandKind::ToggleFavourite, m_currentFolder};
        }
        if (ImGui::MenuItem(revealLabel)) {
            m_deferred = DeferredCommand{CommandKind::Reveal, m_currentFolder};
        }
        ImGui::EndPopup();
    }

    // The viewport and hierarchy take Delete and F2 on hover, so the browser
    // only answers them while it is both focused and hovered.
    if (!m_selected.empty() && !isRoot(m_selected) && ImGui::IsWindowFocused() && ImGui::IsWindowHovered() &&
        !ImGui::IsAnyItemActive()) {
        if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
            m_deferred = DeferredCommand{CommandKind::BeginRename, m_selected};
        } else if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            m_deferred = DeferredCommand{CommandKind::BeginDelete, m_selected};
        }
    }
}

void AssetBrowserPanel::drawList(const std::vector<BrowserEntry>& entries, const AssetBrowserActions& actions) {
    if (!ImGui::BeginTable("##AssetList", 3,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) {
        return;
    }
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 120.0f);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableHeadersRow();
    for (const BrowserEntry& entry : entries) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(genericUtf8(entry.path).c_str());
        // The label leaves room for the kind icon; the id stays fixed.
        const std::string label = widgets::IconLabelPadding() + entry.label + "###entry";
        const bool pressed =
            ImGui::Selectable(label.c_str(), m_selected == entry.path,
                              ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick);
        widgets::ItemIcon(assetIcon(entry.asset), 0.0f);
        handleEntryInteraction(entry, pressed, actions);
        ImGui::TableNextColumn();
        if (entry.asset == nullptr) {
            ImGui::TextDisabled("Folder");
        } else {
            ImGui::TextUnformatted(kindName(entry.asset->kind));
            if (const std::string detail = textureDetail(*entry.asset); !detail.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", detail.c_str());
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(byteSizeText(entry.asset->byteSize).c_str());
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void AssetBrowserPanel::drawGrid(const std::vector<BrowserEntry>& entries, const AssetBrowserActions& actions) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float rightEdge = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    for (size_t index = 0; index < entries.size(); ++index) {
        const BrowserEntry& entry = entries[index];
        ImGui::PushID(genericUtf8(entry.path).c_str());
        ImGui::BeginGroup();
        const ImVec2 tileMin = ImGui::GetCursorScreenPos();
        const bool pressed =
            ImGui::Selectable("##Tile", m_selected == entry.path, ImGuiSelectableFlags_AllowDoubleClick,
                              ImVec2(gridTileWidth, gridTileHeight));
        handleEntryInteraction(entry, pressed, actions);

        // Images show their thumbnails once uploaded, other kinds large kind
        // icons; ready texture thumbnails add their dimensions.
        const std::string detail = entry.asset == nullptr ? std::string{} : textureDetail(*entry.asset);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const float lineHeight = ImGui::GetTextLineHeight();
        const float iconPixel = editorArtPixel() * 3.0f;
        const float iconExtent = iconPixel * static_cast<float>(iconBitmapSize);
        const float iconTop = tileMin.y + (gridTileHeight - iconExtent - (detail.empty() ? 0.0f : lineHeight)) * 0.5f;
        const uint64_t thumbnail =
            entry.asset != nullptr && entry.asset->kind == EditorAssetKind::Texture && actions.thumbnailTexture
                ? actions.thumbnailTexture(entry.asset->absolutePath)
                : 0;
        const ImVec2 iconMin{tileMin.x + (gridTileWidth - iconExtent) * 0.5f, iconTop};
        if (thumbnail != 0) {
            drawList->AddImage(static_cast<ImTextureID>(thumbnail), iconMin,
                               {iconMin.x + iconExtent, iconMin.y + iconExtent});
        } else {
            drawIcon(drawList, assetIcon(entry.asset), iconMin, iconPixel);
        }
        if (!detail.empty()) {
            drawList->AddText(ImVec2(tileMin.x + (gridTileWidth - ImGui::CalcTextSize(detail.c_str()).x) * 0.5f,
                                     iconTop + iconExtent),
                              ImGui::GetColorU32(ImGuiCol_TextDisabled), detail.c_str());
        }
        ImGui::TextUnformatted(fitText(entry.label, gridTileWidth).c_str());
        ImGui::EndGroup();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip("%s", entry.label.c_str());
        }
        ImGui::PopID();
        const float nextTileRight = ImGui::GetItemRectMax().x + style.ItemSpacing.x + gridTileWidth;
        if (index + 1 < entries.size() && nextTileRight <= rightEdge) {
            ImGui::SameLine();
        }
    }
}

void AssetBrowserPanel::handleEntryInteraction(const BrowserEntry& entry, bool pressed,
                                               const AssetBrowserActions& actions) {
    const bool folder = entry.asset == nullptr;
    if (folder) {
        noteDropTarget(entry.path);
    }
    if (pressed) {
        m_selected = entry.path;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (folder) {
                m_deferred = DeferredCommand{CommandKind::Navigate, entry.path};
            } else {
                openAsset(entry.path, actions);
            }
        }
    }
    if (ImGui::BeginDragDropSource()) {
        // Models and prefabs keep the narrow payloads viewports place;
        // textures, sounds, and materials go to asset fields in UTF-8; folders
        // accept every payload.
        const char* assetPayload = folder ? nullptr : assetDragPayloadFor(entry.asset->kind);
        const bool narrow = folder || entry.asset->kind == EditorAssetKind::Model ||
                            entry.asset->kind == EditorAssetKind::Prefab || assetPayload == nullptr;
        const std::string path = narrow ? entry.path.string() : genericUtf8(entry.path);
        ImGui::SetDragDropPayload(assetPayload != nullptr ? assetPayload : itemPayloadType, path.c_str(),
                                  path.size() + 1);
        const bool placeable =
            !folder && (entry.asset->kind == EditorAssetKind::Model || entry.asset->kind == EditorAssetKind::Prefab);
        ImGui::Text("%s %s", dragVerb(placeable), entry.label.c_str());
        ImGui::EndDragDropSource();
    }
    if (folder) {
        acceptItemDrop(entry.path);
    }
    if (ImGui::BeginPopupContextItem("##ItemMenu")) {
        m_selected = entry.path;
        if (!folder && entry.asset->kind == EditorAssetKind::Script && actions.openScript) {
            if (ImGui::MenuItem("Open in IDE")) {
                actions.openScript(entry.path);
            }
            ImGui::Separator();
        }
        if (!folder && entry.asset->kind == EditorAssetKind::Prefab && actions.openPrefab) {
            if (ImGui::MenuItem("Edit Prefab")) {
                actions.openPrefab(entry.asset->absolutePath);
            }
            ImGui::Separator();
        }
        drawItemMenu(entry.path, folder);
        ImGui::EndPopup();
    }
}

void AssetBrowserPanel::acceptItemDrop(const std::filesystem::path& folder) {
    if (!ImGui::BeginDragDropTarget()) {
        return;
    }
    m_folderDropHoverFrame = ImGui::GetFrameCount();
    for (const MovablePayload& movable : movablePayloads) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(movable.type)) {
            const char* text = static_cast<const char*>(payload->Data);
            const std::filesystem::path item = movable.utf8 ? pathFromUtf8Text(text) : std::filesystem::path(text);
            m_deferred = DeferredCommand{CommandKind::Move, item, folder};
        }
    }
    ImGui::EndDragDropTarget();
}

const char* AssetBrowserPanel::dragVerb(bool placeable) const {
    // Targets are drawn after the source, so last frame's hover decides.
    if (m_folderDropHoverFrame >= ImGui::GetFrameCount() - 1) {
        return "Move";
    }
    const ImGuiContext& imgui = *ImGui::GetCurrentContext();
    if (imgui.DragDropAcceptIdPrev != 0) {
        return placeable ? "Place" : "Assign";
    }
    return placeable ? "Place" : "Drag";
}

void AssetBrowserPanel::drawItemMenu(const std::filesystem::path& item, bool folder) {
    const bool root = isRoot(item);
    if (folder) {
        if (ImGui::MenuItem("Open")) {
            m_deferred = DeferredCommand{CommandKind::Navigate, item};
        }
        if (ImGui::MenuItem(isFavourite(item) ? "Remove from Favourites" : "Add to Favourites")) {
            m_deferred = DeferredCommand{CommandKind::ToggleFavourite, item};
        }
        ImGui::Separator();
    }
    if (ImGui::MenuItem("Rename", "F2", false, !root)) {
        m_deferred = DeferredCommand{CommandKind::BeginRename, item};
    }
    if (ImGui::MenuItem("Duplicate", nullptr, false, !root)) {
        m_deferred = DeferredCommand{CommandKind::Duplicate, item};
    }
    if (ImGui::MenuItem("Delete", "Del", false, !root)) {
        m_deferred = DeferredCommand{CommandKind::BeginDelete, item};
    }
    ImGui::Separator();
    const EditorAssetRecord* asset = folder ? nullptr : m_project.catalog().findAsset(item);
    if (asset != nullptr && reimportable(asset->kind) && ImGui::MenuItem("Reimport")) {
        m_deferred = DeferredCommand{CommandKind::Reimport, item};
    }
    if (ImGui::MenuItem("Find References", nullptr, false, !root)) {
        m_deferred = DeferredCommand{CommandKind::FindReferences, item};
    }
    if (ImGui::MenuItem(revealLabel)) {
        m_deferred = DeferredCommand{CommandKind::Reveal, item};
    }
}

void AssetBrowserPanel::drawFolderMenu() {
    if (ImGui::MenuItem("New Folder")) {
        m_deferred = DeferredCommand{CommandKind::CreateFolder, m_currentFolder};
    }
    // The source folder lists only scripts, so a scene there would vanish.
    if (ImGui::MenuItem("New Scene", nullptr, false, !m_project.catalog().inSourceRoot(m_currentFolder))) {
        m_deferred = DeferredCommand{CommandKind::CreateScene, m_currentFolder};
    }
    if (ImGui::MenuItem("New Material", nullptr, false, !m_project.catalog().inSourceRoot(m_currentFolder))) {
        m_deferred = DeferredCommand{CommandKind::CreateMaterial, m_currentFolder};
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Import...", nullptr, false, !m_project.catalog().inSourceRoot(m_currentFolder))) {
        m_deferred = DeferredCommand{CommandKind::Import, m_currentFolder};
    }
}

void AssetBrowserPanel::drawSelectedAsset(const AssetBrowserActions& actions, EditorDiagnostics& diagnostics) {
    if (m_selected.empty()) {
        return;
    }
    ImGui::Separator();
    ImGui::TextWrapped("%s", utf8PathText(m_selected).c_str());
    const EditorAssetRecord* asset = m_project.catalog().findAsset(m_selected);
    if (asset != nullptr && asset->kind == EditorAssetKind::Material) {
        drawMaterialInspector(*asset, actions, diagnostics);
    }
    if (asset != nullptr && asset->kind == EditorAssetKind::Model && actions.reloadModel &&
        widgets::Button("Reload Model")) {
        actions.reloadModel(asset->absolutePath);
    }
    if (asset != nullptr && asset->kind == EditorAssetKind::Script && actions.openScript &&
        widgets::Button("Open in IDE")) {
        actions.openScript(asset->absolutePath);
    }
    if (asset != nullptr && (asset->kind == EditorAssetKind::Audio || asset->kind == EditorAssetKind::Texture ||
                             asset->kind == EditorAssetKind::Data)) {
        drawImportSettings(*asset, actions, diagnostics);
    }
}

void AssetBrowserPanel::drawImportSettings(const EditorAssetRecord& asset, const AssetBrowserActions& actions,
                                           EditorDiagnostics& diagnostics) {
    // The settings are edited in place until Apply stores them; selecting
    // another asset reads its own.
    if (m_settingsPath != asset.absolutePath) {
        m_settingsPath = asset.absolutePath;
        m_settings = m_project.importSettings(asset.absolutePath).value_or(nlohmann::json::object());
    }
    const nlohmann::json stored = m_project.importSettings(asset.absolutePath).value_or(nlohmann::json::object());
    if (asset.kind == EditorAssetKind::Texture) {
        TextureImportSettings texture;
        if (const std::string problem = readTextureImportSettings(m_settings, texture); !problem.empty()) {
            ImGui::TextColored(toImVec4(palette::error), "Import settings: %s", problem.c_str());
        }
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::BeginCombo("Usage##ImportUsage", textureUsageLabel(texture.usage))) {
            for (const TextureUsage usage : textureUsages) {
                if (ImGui::Selectable(textureUsageLabel(usage), usage == texture.usage)) {
                    texture.usage = usage;
                    writeTextureImportSettings(texture, m_settings);
                }
            }
            ImGui::EndCombo();
        }
    } else if (asset.kind == EditorAssetKind::Data) {
        const std::string schema = definitionSchemaSetting(m_settings);
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::BeginCombo("Schema##ImportSchema", schema.empty() ? "(by file name)" : schema.c_str())) {
            if (ImGui::Selectable("(by file name)", schema.empty())) {
                m_settings.erase("schema");
            }
            if (actions.definitionSchemas != nullptr) {
                for (const DefinitionSchema& candidate : actions.definitionSchemas->schemas()) {
                    if (ImGui::Selectable(candidate.name.c_str(), candidate.name == schema)) {
                        m_settings["schema"] = candidate.name;
                    }
                }
            }
            ImGui::EndCombo();
        }
    }
    // Sounds have no import settings yet, only a reimport.
    if (asset.kind != EditorAssetKind::Audio) {
        const bool edited = m_settings != stored;
        ImGui::BeginDisabled(!edited);
        if (widgets::Button("Apply")) {
            m_project.setImportSettings(asset.absolutePath, m_settings, diagnostics);
        }
        ImGui::SameLine();
        if (widgets::Button("Revert")) {
            m_settings = stored;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    if (widgets::Button("Reimport")) {
        reimport(asset.absolutePath, diagnostics);
    }
    if (asset.kind == EditorAssetKind::Data) {
        if (ImGui::BeginChild("##DataTable", {0.0f, 0.0f})) {
            m_dataTable.draw(asset.absolutePath, actions.definitionSchemas, stored, m_project.dataRevision(),
                             diagnostics);
        }
        ImGui::EndChild();
    }
}

void AssetBrowserPanel::drawReferencesWindow(const AssetBrowserActions& actions, EditorDiagnostics& diagnostics) {
    constexpr const char* referencesTitle = "References##AssetBrowser";
    if (m_openModal == OpenModal::References && m_modalNeedsOpening) {
        ImGui::OpenPopup(referencesTitle);
        m_modalNeedsOpening = false;
    }
    if (!ImGui::BeginPopupModal(referencesTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::Text("What uses '%s'", utf8PathText(m_references.item.filename()).c_str());
    ImGui::Separator();
    if (m_references.files.empty()) {
        ImGui::TextDisabled("No scene, prefab, or material in the project references it.");
    }
    std::optional<std::filesystem::path> reveal;
    for (const AssetReferrer& referrer : m_references.files) {
        const std::string label = std::string(referrerLabel(referrer.kind)) + "  " + utf8PathText(referrer.file) +
                                  "##" + genericUtf8(referrer.file);
        if (ImGui::Selectable(label.c_str())) {
            reveal = referrer.file;
        }
    }
    ImGui::Separator();
    if (m_references.sceneEntities.empty()) {
        ImGui::TextDisabled("No entity of the open scene uses it.");
    } else {
        ImGui::Text("%zu entities of the open scene use it.", m_references.sceneEntities.size());
        if (actions.selectEntities) {
            ImGui::SameLine();
            if (widgets::Button("Select")) {
                actions.selectEntities(m_references.sceneEntities);
            }
        }
    }
    if (reveal || widgets::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        m_openModal = OpenModal::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    if (reveal) {
        revealAsset(*reveal, diagnostics);
    }
}

void AssetBrowserPanel::runDeferredCommand(const AssetBrowserActions& actions, EditorDiagnostics& diagnostics) {
    if (!m_deferred) {
        return;
    }
    const DeferredCommand command = std::move(*m_deferred);
    m_deferred.reset();
    switch (command.kind) {
    case CommandKind::Navigate:
        navigateTo(command.path, diagnostics);
        break;
    case CommandKind::CreateFolder: {
        const std::filesystem::path previousSelection = m_selected;
        createFolder(diagnostics);
        // A new folder starts in rename, as in most file browsers.
        if (m_selected != previousSelection && m_project.catalog().findFolder(m_selected)) {
            beginRename(m_selected);
        }
        break;
    }
    case CommandKind::CreateScene:
        createScene(diagnostics);
        break;
    case CommandKind::CreateMaterial:
        createMaterial(diagnostics);
        break;
    case CommandKind::BeginRename:
        beginRename(command.path);
        break;
    case CommandKind::Move:
        moveItem(command.path, command.destination, diagnostics);
        break;
    case CommandKind::Duplicate:
        duplicateItem(command.path, diagnostics);
        break;
    case CommandKind::BeginDelete:
        requestDelete(command.path, diagnostics);
        break;
    case CommandKind::Reveal:
        revealItem(command.path, diagnostics);
        break;
    case CommandKind::ToggleFavourite:
        toggleFavourite(command.path, diagnostics);
        break;
    case CommandKind::Import:
        m_importFolder = command.path;
        m_importDialog.request(FileDialogKind::Open, command.path, true);
        break;
    case CommandKind::Reimport:
        reimport(command.path, diagnostics);
        break;
    case CommandKind::FindReferences:
        findReferences(command.path, actions, diagnostics);
        break;
    }
}

void AssetBrowserPanel::beginRename(const std::filesystem::path& item) {
    const EditorAssetFolder* folder = m_project.catalog().findFolder(item);
    m_openModal = OpenModal::Rename;
    m_modalNeedsOpening = true;
    m_modalTarget = item;
    // Files are renamed by stem; their extension is kept automatically.
    m_renameText = utf8PathText(folder != nullptr ? item.filename() : item.stem());
}

void AssetBrowserPanel::drawModals(EditorDiagnostics& diagnostics) {
    constexpr const char* renameTitle = "Rename##AssetBrowser";
    constexpr const char* deleteTitle = "Move to Trash##AssetBrowser";
    if (m_modalNeedsOpening && (m_openModal == OpenModal::Rename || m_openModal == OpenModal::Delete)) {
        ImGui::OpenPopup(m_openModal == OpenModal::Rename ? renameTitle : deleteTitle);
        m_modalNeedsOpening = false;
    }
    const std::string name = utf8PathText(m_modalTarget.filename());
    if (ImGui::BeginPopupModal(renameTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Rename '%s'", name.c_str());
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(320.0f);
        const bool submitted = ImGui::InputText("##NewName", &m_renameText, ImGuiInputTextFlags_EnterReturnsTrue);
        if (!m_project.catalog().findFolder(m_modalTarget) && !m_modalTarget.extension().empty()) {
            ImGui::TextDisabled("The %s extension is kept.", utf8PathText(m_modalTarget.extension()).c_str());
        }
        if (submitted || widgets::Button("Rename")) {
            renameItem(m_modalTarget, m_renameText, diagnostics);
            m_openModal = OpenModal::None;
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::SameLine();
            if (widgets::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                m_openModal = OpenModal::None;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal(deleteTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Move '%s' to the %s?", name.c_str(), trashName);
        ImGui::TextDisabled("It can be restored from there.");
        if (!m_deleteReferrers.empty()) {
            ImGui::TextColored(toImVec4(palette::error), "Still used by %zu files:", m_deleteReferrers.size());
            for (const AssetReferrer& referrer : m_deleteReferrers) {
                ImGui::BulletText("%s %s", referrerLabel(referrer.kind), utf8PathText(referrer.file).c_str());
            }
            ImGui::TextDisabled("Their references will be missing.");
        }
        if (widgets::Button("Move to Trash")) {
            moveItemToTrash(m_modalTarget, diagnostics);
            m_openModal = OpenModal::None;
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::SameLine();
            if (widgets::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                m_openModal = OpenModal::None;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
}

std::string AssetBrowserPanel::textureDetail(const EditorAssetRecord& asset) {
    if (asset.kind != EditorAssetKind::Texture) {
        return {};
    }
    m_project.thumbnails().request(asset.absolutePath);
    const std::optional<CachedThumbnail> thumbnail = m_project.thumbnails().find(asset.absolutePath);
    if (!thumbnail || thumbnail->state != ThumbnailState::Ready) {
        return {};
    }
    return std::to_string(thumbnail->width) + "x" + std::to_string(thumbnail->height);
}

void AssetBrowserPanel::drawMaterialInspector(const EditorAssetRecord& asset, const AssetBrowserActions& actions,
                                              EditorDiagnostics& diagnostics) {
    const std::filesystem::path assetRoot = m_project.catalog().roots().front();
    const MaterialDocumentLocation location{.assetRoot = assetRoot, .database = m_project.assetDatabase().get()};
    // Read again for another material, or when the file changed elsewhere
    // while this view had nothing unsaved.
    const bool changedOnDisk = !m_materialUnsaved && asset.lastWriteTime != m_materialWriteTime;
    if (m_materialPath != asset.absolutePath || changedOnDisk) {
        m_materialPath = asset.absolutePath;
        m_materialWriteTime = asset.lastWriteTime;
        m_materialUnsaved = false;
        MaterialDocument loaded;
        const MaterialFileResult read = loadMaterialDocument(asset.absolutePath, location, loaded);
        m_material = read ? std::optional<MaterialDocument>(std::move(loaded)) : std::nullopt;
        m_materialError = read ? std::string{} : read.error;
    }
    if (!m_material) {
        ImGui::TextColored(toImVec4(palette::error), "%s", m_materialError.c_str());
        return;
    }
    const AssetReferenceFieldContext fieldContext{
        .catalog = m_project.catalog(), .assetRoot = assetRoot, .thumbnail = actions.thumbnailTexture};
    if (ImGui::BeginChild("##MaterialInspector", {0.0f, 0.0f})) {
        ImGui::PushID("MaterialInspector");
        m_materialUnsaved |= drawReflectedFields(&*m_material, materialDocumentFields(), {.assets = &fieldContext});
        ImGui::PopID();
    }
    ImGui::EndChild();
    // Saved when the edit ends (a drag released, typing confirmed), not on
    // every frame of it.
    if (!m_materialUnsaved || ImGui::IsAnyItemActive()) {
        return;
    }
    m_materialUnsaved = false;
    if (const MaterialFileResult saved = saveMaterialDocument(m_materialPath, location, *m_material); !saved) {
        diagnostics.reportError("Could not save material '" + utf8PathText(m_materialPath.filename()) +
                                "': " + saved.error);
        return;
    }
    std::error_code error;
    m_materialWriteTime = std::filesystem::last_write_time(m_materialPath, error);
    if (actions.reloadMaterial) {
        actions.reloadMaterial(m_materialPath);
    }
}