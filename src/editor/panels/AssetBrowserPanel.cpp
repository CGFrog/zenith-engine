#include "AssetBrowserPanel.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/panels/AssetDragPayloads.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/project/ProjectFileOperations.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

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
constexpr std::array<EditorAssetKind, 5> filterableKinds{EditorAssetKind::Model, EditorAssetKind::Texture,
                                                         EditorAssetKind::Audio, EditorAssetKind::Scene,
                                                         EditorAssetKind::Prefab};

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
    }
    return "Asset";
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

void AssetBrowserPanel::createFolder(EditorDiagnostics& diagnostics) {
    select(m_project.createFolder(m_currentFolder, diagnostics));
}

void AssetBrowserPanel::createScene(EditorDiagnostics& diagnostics) {
    select(m_project.createScene(m_currentFolder, diagnostics));
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

AssetBrowserActions AssetBrowserPanel::sessionActions(EditorContext& context) {
    return {
        .openScene =
            [&context](const std::filesystem::path& path) { context.actions.requestOpenScene(context.app, path); },
        .reloadModel =
            [&context](const std::filesystem::path& path) {
                reloadProjectModel(context.app.assets, context.diagnostics, path);
            },
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
    if (widgets::Button("Refresh")) {
        refresh(diagnostics);
    }
    ImGui::SameLine();
    drawToolbar(diagnostics);
    drawBreadcrumbs();
    ImGui::Separator();

    const float detailsHeight = m_selected.empty() ? 0.0f : ImGui::GetFrameHeightWithSpacing() * 2.5f;
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
    drawSelectedAsset(actions);
    runDeferredCommand(diagnostics);
    drawModals(diagnostics);
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

        // Large kind icons stand in for previews until the browser shows
        // thumbnails; ready texture thumbnails add their dimensions.
        const std::string detail = entry.asset == nullptr ? std::string{} : textureDetail(*entry.asset);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const float lineHeight = ImGui::GetTextLineHeight();
        const float iconPixel = editorArtPixel() * 3.0f;
        const float iconExtent = iconPixel * static_cast<float>(iconBitmapSize);
        const float iconTop = tileMin.y + (gridTileHeight - iconExtent - (detail.empty() ? 0.0f : lineHeight)) * 0.5f;
        drawIcon(drawList, assetIcon(entry.asset), {tileMin.x + (gridTileWidth - iconExtent) * 0.5f, iconTop},
                 iconPixel);
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
    if (pressed) {
        m_selected = entry.path;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (folder) {
                m_deferred = DeferredCommand{CommandKind::Navigate, entry.path};
            } else if (entry.asset->kind == EditorAssetKind::Scene && actions.openScene) {
                actions.openScene(entry.path);
            }
        }
    }
    if (ImGui::BeginDragDropSource()) {
        // Models and prefabs keep the payloads viewports place; folders
        // accept every payload.
        const bool model = !folder && entry.asset->kind == EditorAssetKind::Model;
        const bool prefab = !folder && entry.asset->kind == EditorAssetKind::Prefab;
        const char* payloadType = model ? modelPayloadType : prefab ? prefabPayloadType : itemPayloadType;
        const std::string path = entry.path.string();
        ImGui::SetDragDropPayload(payloadType, path.c_str(), path.size() + 1);
        ImGui::Text("%s %s", model || prefab ? "Place" : "Move", entry.label.c_str());
        ImGui::EndDragDropSource();
    }
    if (folder) {
        acceptItemDrop(entry.path);
    }
    if (ImGui::BeginPopupContextItem("##ItemMenu")) {
        m_selected = entry.path;
        drawItemMenu(entry.path, folder);
        ImGui::EndPopup();
    }
}

void AssetBrowserPanel::acceptItemDrop(const std::filesystem::path& folder) {
    if (!ImGui::BeginDragDropTarget()) {
        return;
    }
    for (const char* type : {itemPayloadType, modelPayloadType, prefabPayloadType}) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(type)) {
            const std::filesystem::path item(static_cast<const char*>(payload->Data));
            m_deferred = DeferredCommand{CommandKind::Move, item, folder};
        }
    }
    ImGui::EndDragDropTarget();
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
    if (ImGui::MenuItem(revealLabel)) {
        m_deferred = DeferredCommand{CommandKind::Reveal, item};
    }
}

void AssetBrowserPanel::drawFolderMenu() {
    if (ImGui::MenuItem("New Folder")) {
        m_deferred = DeferredCommand{CommandKind::CreateFolder, m_currentFolder};
    }
    if (ImGui::MenuItem("New Scene")) {
        m_deferred = DeferredCommand{CommandKind::CreateScene, m_currentFolder};
    }
}

void AssetBrowserPanel::drawSelectedAsset(const AssetBrowserActions& actions) {
    if (m_selected.empty()) {
        return;
    }
    ImGui::Separator();
    ImGui::TextWrapped("%s", utf8PathText(m_selected).c_str());
    const EditorAssetRecord* asset = m_project.catalog().findAsset(m_selected);
    if (asset != nullptr && asset->kind == EditorAssetKind::Model && actions.reloadModel &&
        widgets::Button("Reload Model")) {
        actions.reloadModel(asset->absolutePath);
    }
}

void AssetBrowserPanel::runDeferredCommand(EditorDiagnostics& diagnostics) {
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
        m_openModal = OpenModal::Delete;
        m_modalNeedsOpening = true;
        m_modalTarget = command.path;
        break;
    case CommandKind::Reveal:
        revealItem(command.path, diagnostics);
        break;
    case CommandKind::ToggleFavourite:
        toggleFavourite(command.path, diagnostics);
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
    if (m_modalNeedsOpening) {
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
