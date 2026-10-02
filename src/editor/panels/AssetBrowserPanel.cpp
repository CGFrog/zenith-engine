#include "AssetBrowserPanel.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/project/ProjectFileOperations.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <utility>

namespace {
constexpr float refreshIntervalSeconds = 1.0f;
constexpr float folderPaneWidth = 200.0f;
constexpr float gridTileWidth = 96.0f;
constexpr float gridTileHeight = 72.0f;
constexpr const char* diagnosticSource = "Asset Browser";
constexpr const char* modelPayloadType = "ZENITH_MODEL_ASSET";
#if defined(_WIN32)
constexpr const char* revealLabel = "Show in Explorer";
constexpr const char* trashName = "Recycle Bin";
#else
constexpr const char* revealLabel = "Show in File Manager";
constexpr const char* trashName = "trash";
#endif
// In EditorAssetKind order, so a kind's value indexes the type filter.
constexpr std::array<EditorAssetKind, 4> filterableKinds{EditorAssetKind::Model, EditorAssetKind::Texture,
                                                         EditorAssetKind::Audio, EditorAssetKind::Scene};

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

AssetBrowserPanel::AssetBrowserPanel(std::filesystem::path assetRoot, std::filesystem::path sceneRoot,
                                     std::filesystem::path settingsFile)
    : m_catalog(std::move(assetRoot), std::move(sceneRoot))
    , m_settingsPath(std::move(settingsFile)) {
    if (const AssetBrowserSettingsResult loaded = loadAssetBrowserSettings(m_settingsPath, m_settings); !loaded) {
        m_pendingSettingsError = loaded.error;
    }
    m_currentFolder =
        m_settings.lastFolder.empty() ? m_catalog.roots().front() : resolveFolderKey(m_settings.lastFolder);
}

const char* AssetBrowserPanel::title() const {
    return "Asset Browser";
}

void AssetBrowserPanel::update(EditorContext& context, const InputState&, float deltaSeconds) {
    reportPendingSettingsError(context.diagnostics);
    m_refreshElapsed += deltaSeconds;
    // The first frame scans at once; afterwards, including after a failed
    // scan, rescans wait for the interval.
    if (!m_scanAttempted || m_refreshElapsed >= refreshIntervalSeconds) {
        m_scanAttempted = true;
        m_refreshElapsed = 0.0f;
        refresh(context.diagnostics);
    }
}

void AssetBrowserPanel::refresh(EditorDiagnostics& diagnostics) {
    rescan(diagnostics, true);
}

void AssetBrowserPanel::rescan(EditorDiagnostics& diagnostics, bool reportChanges) {
    AssetCatalogResult result = m_catalog.refresh();
    if (!result) {
        diagnostics.reportError(result.error);
        return;
    }
    for (const AssetCatalogChange& change : result.changes) {
        if (change.asset.kind == EditorAssetKind::Texture) {
            m_thumbnails.invalidate(change.asset.absolutePath);
        }
    }
    if (reportChanges && m_initialized && !result.changes.empty()) {
        diagnostics.reportError(std::to_string(result.changes.size()) +
                                " project asset change(s) detected. Select a changed model and choose Reload Model "
                                "to reload it explicitly.");
    }
    m_initialized = true;
    keepStateValid();
}

// After a rescan, the current folder falls back to its nearest surviving
// ancestor and a vanished selection is cleared.
void AssetBrowserPanel::keepStateValid() {
    std::filesystem::path folder = m_currentFolder;
    while (!m_catalog.findFolder(folder)) {
        const bool insideRoot = std::ranges::any_of(
            m_catalog.roots(), [&folder](const std::filesystem::path& root) { return isWithin(folder, root); });
        if (!insideRoot || !folder.has_relative_path() || folder == folder.parent_path()) {
            folder = m_catalog.folders().empty() ? m_catalog.roots().front() : m_catalog.folders().front().absolutePath;
            break;
        }
        folder = folder.parent_path();
    }
    if (folder != m_currentFolder) {
        m_currentFolder = std::move(folder);
        m_revealCurrentInTree = true;
    }
    if (!m_selected.empty() && !m_catalog.findAsset(m_selected) && !m_catalog.findFolder(m_selected)) {
        m_selected.clear();
    }
}

void AssetBrowserPanel::reportOutcome(EditorDiagnostics& diagnostics, bool success, const std::string& message) {
    if (success) {
        diagnostics.add(EditorDiagnosticSeverity::Info, diagnosticSource, message);
    } else {
        diagnostics.reportError(message);
    }
}

void AssetBrowserPanel::saveSettings(EditorDiagnostics& diagnostics) {
    if (const AssetBrowserSettingsResult saved = saveAssetBrowserSettings(m_settingsPath, m_settings); !saved) {
        diagnostics.reportError(saved.error);
    }
}

void AssetBrowserPanel::reportPendingSettingsError(EditorDiagnostics& diagnostics) {
    if (!m_pendingSettingsError.empty()) {
        diagnostics.add(EditorDiagnosticSeverity::Warning, diagnosticSource, std::move(m_pendingSettingsError));
        m_pendingSettingsError.clear();
    }
}

std::string AssetBrowserPanel::folderKey(const std::filesystem::path& folder) const {
    const std::filesystem::path relative = folder.lexically_relative(m_catalog.roots().front());
    return genericUtf8(relative.empty() ? folder : relative);
}

std::filesystem::path AssetBrowserPanel::resolveFolderKey(const std::string& key) const {
    return normalizedCatalogPath(m_catalog.roots().front() / pathFromUtf8Text(key));
}

std::string AssetBrowserPanel::rootLabel(const std::filesystem::path& root) const {
    const std::string name = utf8PathText(root.filename());
    return name.empty() ? utf8PathText(root) : name;
}

bool AssetBrowserPanel::isRoot(const std::filesystem::path& path) const {
    return std::ranges::find(m_catalog.roots(), path) != m_catalog.roots().end();
}

const AssetCatalog& AssetBrowserPanel::catalog() const {
    return m_catalog;
}

const std::filesystem::path& AssetBrowserPanel::currentFolder() const {
    return m_currentFolder;
}

void AssetBrowserPanel::navigateTo(const std::filesystem::path& folder, EditorDiagnostics& diagnostics) {
    const std::filesystem::path normalized = normalizedCatalogPath(folder);
    if (!m_catalog.findFolder(normalized)) {
        diagnostics.reportError("Cannot open folder '" + utf8PathText(normalized) + "': it is not a project folder.");
        return;
    }
    m_currentFolder = normalized;
    m_revealCurrentInTree = true;
    if (const std::string key = folderKey(normalized); key != m_settings.lastFolder) {
        m_settings.lastFolder = key;
        saveSettings(diagnostics);
    }
}

AssetBrowserViewMode AssetBrowserPanel::viewMode() const {
    return m_settings.viewMode;
}

void AssetBrowserPanel::setViewMode(AssetBrowserViewMode mode, EditorDiagnostics& diagnostics) {
    if (m_settings.viewMode != mode) {
        m_settings.viewMode = mode;
        saveSettings(diagnostics);
    }
}

bool AssetBrowserPanel::isFavourite(const std::filesystem::path& folder) const {
    return std::ranges::find(m_settings.favourites, folderKey(normalizedCatalogPath(folder))) !=
           m_settings.favourites.end();
}

void AssetBrowserPanel::toggleFavourite(const std::filesystem::path& folder, EditorDiagnostics& diagnostics) {
    const std::string key = folderKey(normalizedCatalogPath(folder));
    if (const auto found = std::ranges::find(m_settings.favourites, key); found != m_settings.favourites.end()) {
        m_settings.favourites.erase(found);
    } else {
        m_settings.favourites.push_back(key);
    }
    saveSettings(diagnostics);
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
    const ProjectFileResult result = createProjectFolder(m_currentFolder);
    reportOutcome(diagnostics, result.success,
                  result ? "Created folder '" + utf8PathText(result.path) + "'." : result.error);
    if (result) {
        rescan(diagnostics, false);
        m_selected = normalizedCatalogPath(result.path);
    }
}

void AssetBrowserPanel::createScene(EditorDiagnostics& diagnostics) {
    const ProjectFileResult result = createProjectScene(m_currentFolder, m_catalog.roots().front());
    reportOutcome(diagnostics, result.success,
                  result ? "Created scene '" + utf8PathText(result.path) + "'." : result.error);
    if (result) {
        rescan(diagnostics, false);
        m_selected = normalizedCatalogPath(result.path);
    }
}

void AssetBrowserPanel::renameItem(const std::filesystem::path& item, const std::string& utf8NewName,
                                   EditorDiagnostics& diagnostics) {
    const std::filesystem::path source = normalizedCatalogPath(item);
    if (isRoot(source)) {
        diagnostics.reportError("Project roots cannot be renamed from the asset browser.");
        return;
    }
    const ProjectFileResult result = renameProjectItem(source, utf8NewName);
    if (!result) {
        diagnostics.reportError(result.error);
        return;
    }
    const std::filesystem::path destination = normalizedCatalogPath(result.path);
    if (destination == source) {
        return;
    }
    reportOutcome(diagnostics, true,
                  "Renamed '" + utf8PathText(source) + "' to '" + utf8PathText(destination.filename()) + "'.");
    // Thumbnails are keyed by path; the old entry would otherwise linger.
    m_thumbnails.invalidate(source);
    bool settingsChanged = false;
    for (std::string& favourite : m_settings.favourites) {
        if (const std::filesystem::path folder = resolveFolderKey(favourite); isWithin(folder, source)) {
            favourite = folderKey(rebase(folder, source, destination));
            settingsChanged = true;
        }
    }
    if (isWithin(m_currentFolder, source)) {
        m_currentFolder = rebase(m_currentFolder, source, destination);
        m_settings.lastFolder = folderKey(m_currentFolder);
        settingsChanged = true;
    }
    if (settingsChanged) {
        saveSettings(diagnostics);
    }
    m_selected = destination;
    rescan(diagnostics, false);
}

void AssetBrowserPanel::duplicateItem(const std::filesystem::path& item, EditorDiagnostics& diagnostics) {
    const std::filesystem::path source = normalizedCatalogPath(item);
    if (isRoot(source)) {
        diagnostics.reportError("Project roots cannot be duplicated from the asset browser.");
        return;
    }
    const ProjectFileResult result = duplicateProjectItem(source);
    reportOutcome(diagnostics, result.success,
                  result
                      ? "Duplicated '" + utf8PathText(source) + "' as '" + utf8PathText(result.path.filename()) + "'."
                      : result.error);
    if (result) {
        rescan(diagnostics, false);
        m_selected = normalizedCatalogPath(result.path);
    }
}

void AssetBrowserPanel::moveItemToTrash(const std::filesystem::path& item, EditorDiagnostics& diagnostics) {
    const std::filesystem::path target = normalizedCatalogPath(item);
    if (isRoot(target)) {
        diagnostics.reportError("Project roots cannot be deleted from the asset browser.");
        return;
    }
    const ProjectFileResult result = moveProjectItemToTrash(target);
    reportOutcome(diagnostics, result.success,
                  result ? "Moved '" + utf8PathText(target) + "' to the trash." : result.error);
    if (!result) {
        return;
    }
    m_thumbnails.invalidate(target);
    const auto removed = std::erase_if(m_settings.favourites, [this, &target](const std::string& favourite) {
        return isWithin(resolveFolderKey(favourite), target);
    });
    if (removed > 0) {
        saveSettings(diagnostics);
    }
    rescan(diagnostics, false);
}

void AssetBrowserPanel::revealItem(const std::filesystem::path& item, EditorDiagnostics& diagnostics) {
    if (const ProjectFileResult result = revealInFileBrowser(item); !result) {
        diagnostics.reportError(result.error);
    }
}

void AssetBrowserPanel::draw(EditorContext& context) {
    const AssetBrowserActions actions{
        .openScene =
            [&context](const std::filesystem::path& path) { context.actions.requestOpenScene(context.app, path); },
        .reloadModel =
            [&context](const std::filesystem::path& path) {
                const AssetLoadResult<ModelHandle> result = context.app.assets.reloadModel(path);
                if (!result) {
                    context.diagnostics.reportError("Model reload failed: " + result.error);
                } else {
                    context.diagnostics.reportError(
                        "Model reloaded. Existing instances retain their previous immutable resources; new "
                        "placements use the refreshed model.");
                }
            },
    };
    drawBrowser(context.diagnostics, actions);
}

void AssetBrowserPanel::drawBrowser(EditorDiagnostics& diagnostics, const AssetBrowserActions& actions) {
    reportPendingSettingsError(diagnostics);
    if (ImGui::Button("Refresh")) {
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
    if (ImGui::Button("New")) {
        ImGui::OpenPopup("##NewMenu");
    }
    if (ImGui::BeginPopup("##NewMenu")) {
        drawFolderMenu();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Grid", m_settings.viewMode == AssetBrowserViewMode::Grid)) {
        setViewMode(AssetBrowserViewMode::Grid, diagnostics);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("List", m_settings.viewMode == AssetBrowserViewMode::List)) {
        setViewMode(AssetBrowserViewMode::List, diagnostics);
    }
    ImGui::SameLine();
    if (ImGui::Button("Types")) {
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
    const auto root = std::ranges::find_if(m_catalog.roots(), [this](const std::filesystem::path& candidate) {
        return isWithin(m_currentFolder, candidate);
    });
    if (root == m_catalog.roots().end()) {
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
    if (ImGui::CollapsingHeader("Favourites", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (m_settings.favourites.empty()) {
            ImGui::TextDisabled("Right-click a folder to add it.");
        }
        for (size_t index = 0; index < m_settings.favourites.size(); ++index) {
            const std::string& key = m_settings.favourites[index];
            const std::filesystem::path folder = resolveFolderKey(key);
            const bool exists = m_catalog.findFolder(folder) != nullptr;
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
    if (ImGui::CollapsingHeader("Folders", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const std::filesystem::path& root : m_catalog.roots()) {
            if (const EditorAssetFolder* folder = m_catalog.findFolder(root)) {
                drawFolderNode(*folder, rootLabel(root));
            } else {
                ImGui::TextDisabled("%s (missing)", rootLabel(root).c_str());
            }
        }
    }
    m_revealCurrentInTree = false;
}

void AssetBrowserPanel::drawFolderNode(const EditorAssetFolder& folder, const std::string& label) {
    const std::vector<const EditorAssetFolder*> children = m_catalog.subfolders(folder.absolutePath);
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
        for (const EditorAssetFolder* folder : m_catalog.subfolders(m_currentFolder)) {
            entries.push_back({folder->absolutePath, utf8PathText(folder->absolutePath.filename()), nullptr});
        }
        for (const EditorAssetRecord* asset : m_catalog.assetsInFolder(m_currentFolder)) {
            if (shown(*asset)) {
                entries.push_back({asset->absolutePath, utf8PathText(asset->absolutePath.filename()), asset});
            }
        }
        return entries;
    }
    // Searching covers the current folder and everything below it; labels show
    // where each match lives.
    for (const EditorAssetFolder& folder : m_catalog.folders()) {
        if (folder.absolutePath != m_currentFolder && isWithin(folder.absolutePath, m_currentFolder) &&
            matches(folder.absolutePath)) {
            entries.push_back(
                {folder.absolutePath, genericUtf8(folder.absolutePath.lexically_relative(m_currentFolder)), nullptr});
        }
    }
    for (const EditorAssetRecord& asset : m_catalog.assets()) {
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
    } else if (m_settings.viewMode == AssetBrowserViewMode::Grid) {
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
        const bool pressed =
            ImGui::Selectable(entry.label.c_str(), m_selected == entry.path,
                              ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick);
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

        // Kind badges stand in for previews until the renderer can show UI
        // textures; ready texture thumbnails add their dimensions.
        const char* badge = entry.asset == nullptr ? "Folder" : kindName(entry.asset->kind);
        const std::string detail = entry.asset == nullptr ? std::string{} : textureDetail(*entry.asset);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const float lineHeight = ImGui::GetTextLineHeight();
        const float badgeTop = tileMin.y + (gridTileHeight - lineHeight * (detail.empty() ? 1.0f : 2.0f)) * 0.5f;
        const ImU32 badgeColour = ImGui::GetColorU32(entry.asset == nullptr ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        drawList->AddText(ImVec2(tileMin.x + (gridTileWidth - ImGui::CalcTextSize(badge).x) * 0.5f, badgeTop),
                          badgeColour, badge);
        if (!detail.empty()) {
            drawList->AddText(ImVec2(tileMin.x + (gridTileWidth - ImGui::CalcTextSize(detail.c_str()).x) * 0.5f,
                                     badgeTop + lineHeight),
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
    if (!folder && entry.asset->kind == EditorAssetKind::Model && ImGui::BeginDragDropSource()) {
        const std::string path = entry.path.string();
        ImGui::SetDragDropPayload(modelPayloadType, path.c_str(), path.size() + 1);
        ImGui::Text("Place %s", entry.label.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginPopupContextItem("##ItemMenu")) {
        m_selected = entry.path;
        drawItemMenu(entry.path, folder);
        ImGui::EndPopup();
    }
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
    const EditorAssetRecord* asset = m_catalog.findAsset(m_selected);
    if (asset != nullptr && asset->kind == EditorAssetKind::Model && actions.reloadModel &&
        ImGui::Button("Reload Model")) {
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
        if (m_selected != previousSelection && m_catalog.findFolder(m_selected)) {
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
    const EditorAssetFolder* folder = m_catalog.findFolder(item);
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
        if (!m_catalog.findFolder(m_modalTarget) && !m_modalTarget.extension().empty()) {
            ImGui::TextDisabled("The %s extension is kept.", utf8PathText(m_modalTarget.extension()).c_str());
        }
        if (submitted || ImGui::Button("Rename")) {
            renameItem(m_modalTarget, m_renameText, diagnostics);
            m_openModal = OpenModal::None;
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                m_openModal = OpenModal::None;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal(deleteTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Move '%s' to the %s?", name.c_str(), trashName);
        ImGui::TextDisabled("It can be restored from there.");
        if (ImGui::Button("Move to Trash")) {
            moveItemToTrash(m_modalTarget, diagnostics);
            m_openModal = OpenModal::None;
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
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
    m_thumbnails.request(asset.absolutePath);
    const std::optional<CachedThumbnail> thumbnail = m_thumbnails.find(asset.absolutePath);
    if (!thumbnail || thumbnail->state != ThumbnailState::Ready) {
        return {};
    }
    return std::to_string(thumbnail->width) + "x" + std::to_string(thumbnail->height);
}
