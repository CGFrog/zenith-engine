#pragma once

#include "editor/application/NativeFileDialog.hpp"
#include "editor/panels/DataTableView.hpp"
#include "editor/panels/EditorPanel.hpp"
#include "editor/project/MaterialDocument.hpp"
#include "editor/project/ProjectAssets.hpp"
#include "engine/scene/SceneEntityId.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

class DefinitionSchemaRegistry;
class EditorDiagnostics;
class FileDropQueue;

// What the browser asks of the editor when the user acts on an asset. Unset
// actions are simply not offered.
struct AssetBrowserActions {
    std::function<void(const std::filesystem::path&)> openScene;
    // Re-imports a model; called by the Reload Model button.
    std::function<void(const std::filesystem::path&)> reloadModel;
    // Opens a script in the user's IDE.
    std::function<void(const std::filesystem::path&)> openScript;
    // Opens a prefab as a document of its own (prefab.edit).
    std::function<void(const std::filesystem::path&)> openPrefab;
    // Reloads a material file the browser's material inspector saved, so
    // scenes using it update at once.
    std::function<void(const std::filesystem::path&)> reloadMaterial;
    // The ImGui texture id of an image's thumbnail (ProjectAssets::
    // thumbnailTexture), or 0 while there is none; unset shows kind icons.
    std::function<uint64_t(const std::filesystem::path&)> thumbnailTexture;
    // The entities of the open scene whose components reference an asset,
    // for Find References; unset lists project files only.
    std::function<std::vector<SceneEntityId>(const EditorAssetRecord&)> sceneUsers;
    // Selects entities of the open scene (Find References' Select button).
    std::function<void(std::span<const SceneEntityId>)> selectEntities;
    // Runs a registered operator by id (EditorContext::invoke), which reports
    // its own failures, and whether it can run now (EditorContext::canInvoke).
    // The folder menu's New Behaviour, Component, and System use them; unset
    // leaves those items disabled.
    std::function<void(std::string_view)> runOperator;
    std::function<bool(std::string_view)> canRunOperator;
    // OS file drops of this frame; the browser imports those dropped on it.
    // Null takes none.
    FileDropQueue* fileDrops{nullptr};
    // The game module's definition table schemas, which data tables are
    // validated against; null without a module. Valid for this frame only.
    const DefinitionSchemaRegistry* definitionSchemas{nullptr};
};

// The result of the browser's last Find References: the project files and
// open-scene entities using an asset (or anything in a folder).
struct AssetBrowserReferences {
    std::filesystem::path item;
    std::vector<AssetReferrer> files;
    std::vector<SceneEntityId> sceneEntities;
};

// Whether a browser's folder and view mode are remembered across sessions.
// At most one browser per project is Primary (see ProjectAssets::
// claimPrimaryView); the others keep their view state in memory only, so
// several browsers never fight over the file. The role moves: a hidden or
// destroyed primary gives it up, one that is no longer drawn (another
// workspace, a background tab) loses it, and the next browser drawn takes it.
enum class AssetBrowserRole {
    Primary,
    Secondary,
};

// Browses the project's asset and scene roots: a folder tree with favourites,
// breadcrumbs, a grid or list of the current folder, search and type filters,
// and file operations (create, rename, move, duplicate, move to trash, show in
// the OS file browser). Models drag into the viewport or inspector as
// "ZENITH_MODEL_ASSET" payloads, textures, sounds, and materials onto
// inspector asset fields with their own payloads (AssetDragPayloads.hpp), and
// any item drags onto a folder to move it; scenes open on double-click,
// prefabs open in prefab mode, and scripts (from the project's source root
// too, when the service lists one) open in the IDE. Image tiles show
// thumbnails, and a selected material (.zmat) is edited below the listing and
// saved as it changes. Files dropped from the OS on a browser (or picked with
// Import...) are copied into the folder under the drop, or the current one.
// The selected texture or data table shows its import settings, applied with
// a reimport; a data table also shows its entries and validation (see
// DataTableView). Find References lists the scenes, prefabs, and materials
// using an asset, and deleting one still in use names them first.
//
// The panel is one view over a shared ProjectAssets service, which owns the
// catalog, watchers, thumbnails, change dispatch, favourites, and the file
// operations. The panel holds only its view state (current folder,
// selection, search, filters, view mode, open dialogs), so any number of
// browsers can show the same project (each area of the multiple-instance
// "asset_browser" type has its own); each follows renames and moves made by
// any of them. It never polls or dispatches project changes itself.
class AssetBrowserPanel final : public EditorPanel {
  public:
    // project must outlive the panel. Every browser starts at the remembered
    // folder and view mode. Requesting Primary takes the role when no other
    // browser holds it; a Secondary browser keeps its changes to itself until
    // it is promoted.
    explicit AssetBrowserPanel(ProjectAssets& project, AssetBrowserRole role = AssetBrowserRole::Primary);
    // Gives up the primary role.
    ~AssetBrowserPanel() override;
    AssetBrowserPanel(const AssetBrowserPanel&) = delete;
    AssetBrowserPanel& operator=(const AssetBrowserPanel&) = delete;

    const char* title() const override;
    void update(EditorContext& context, const InputState& input, float deltaSeconds) override;
    void draw(EditorContext& context) override;
    // Gives up the primary role, so a browser that is shown can take it.
    void hidden(EditorContext& context) override;

    // Draws the panel contents into the current ImGui window, first taking
    // the primary role when no browser holds it. draw() calls it with actions
    // bound to the editor session; it needs no session itself.
    void drawBrowser(EditorDiagnostics& diagnostics, const AssetBrowserActions& actions);

    // Rescans the project through the service, then syncs this view.
    void refresh(EditorDiagnostics& diagnostics);
    // Follows renames and moves made through the service since the last sync,
    // then falls back from a vanished folder to its nearest surviving
    // ancestor and clears a vanished selection. update() and drawBrowser()
    // call it; after a scan has listed the project, state is always valid.
    void syncWithProject();

    const AssetCatalog& catalog() const;
    AssetBrowserRole role() const;
    const std::filesystem::path& currentFolder() const;
    // Shows a catalog folder; unknown folders are refused with a diagnostic.
    void navigateTo(const std::filesystem::path& folder, EditorDiagnostics& diagnostics);
    // Opens the folder holding item (an absolute path) and selects item.
    // drawBrowser does this for the reveal the project hands the primary
    // browser (ProjectAssets::requestReveal).
    void revealAsset(const std::filesystem::path& item, EditorDiagnostics& diagnostics);
    AssetBrowserViewMode viewMode() const;
    void setViewMode(AssetBrowserViewMode mode, EditorDiagnostics& diagnostics);
    // Favourites are shared by every browser on the project.
    bool isFavourite(const std::filesystem::path& folder) const;
    void toggleFavourite(const std::filesystem::path& folder, EditorDiagnostics& diagnostics);
    const std::filesystem::path& selectedPath() const;
    // Search text matches file names in the current folder and below.
    void setSearchText(std::string text);
    void setKindShown(EditorAssetKind kind, bool shown);
    // Opens a file the way a double-click does: a scene in the editor, a
    // script in the IDE. Returns whether actions had a way to open it.
    bool openAsset(const std::filesystem::path& item, const AssetBrowserActions& actions);

    // File commands, run through the service. Each reports its outcome
    // through diagnostics and selects what it produced.
    void createFolder(EditorDiagnostics& diagnostics);
    void createScene(EditorDiagnostics& diagnostics);
    // Writes "New Material.zmat" into the current folder.
    void createMaterial(EditorDiagnostics& diagnostics);
    // Writes "New Prefab.prefab", an empty prefab, into the current folder.
    void createPrefab(EditorDiagnostics& diagnostics);
    void renameItem(const std::filesystem::path& item, const std::string& utf8NewName, EditorDiagnostics& diagnostics);
    // Moves an item into another folder of the same root.
    void moveItem(const std::filesystem::path& item, const std::filesystem::path& folder,
                  EditorDiagnostics& diagnostics);
    void duplicateItem(const std::filesystem::path& item, EditorDiagnostics& diagnostics);
    void moveItemToTrash(const std::filesystem::path& item, EditorDiagnostics& diagnostics);
    void revealItem(const std::filesystem::path& item, EditorDiagnostics& diagnostics);
    // Copies files from outside the project into folder and selects the
    // last copy.
    void importFiles(std::span<const std::filesystem::path> sources, const std::filesystem::path& folder,
                     EditorDiagnostics& diagnostics);
    // Reads the asset again at the next dispatch (ProjectAssets::queueReimport).
    void reimport(const std::filesystem::path& item, EditorDiagnostics& diagnostics);
    // Lists what uses item (see AssetBrowserReferences) and opens the
    // References window.
    void findReferences(const std::filesystem::path& item, const AssetBrowserActions& actions,
                        EditorDiagnostics& diagnostics);
    // Asks to move item to the trash; the confirmation names the files that
    // still reference it.
    void requestDelete(const std::filesystem::path& item, EditorDiagnostics& diagnostics);
    // The last Find References result.
    const AssetBrowserReferences& references() const;
    // The files referencing the item the delete confirmation is open for.
    std::span<const AssetReferrer> deleteReferrers() const;

  private:
    struct BrowserEntry {
        std::filesystem::path path;
        std::string label;
        const EditorAssetRecord* asset{nullptr};
    };

    enum class CommandKind {
        Navigate,
        CreateFolder,
        CreateScene,
        CreateMaterial,
        CreatePrefab,
        RunOperator,
        BeginRename,
        Move,
        Duplicate,
        BeginDelete,
        Reveal,
        ToggleFavourite,
        Import,
        Reimport,
        FindReferences,
    };

    // Commands chosen while drawing run after the catalog is no longer being
    // iterated, since they may rescan it.
    struct DeferredCommand {
        CommandKind kind{};
        std::filesystem::path path;
        // Move only: the folder to move path into.
        std::filesystem::path destination;
        // RunOperator only: the registered operator to invoke.
        std::string operatorId;
    };

    enum class OpenModal {
        None,
        Rename,
        Delete,
        References,
    };

    void followItemMoves();
    void keepStateValid();
    void select(const std::optional<std::filesystem::path>& produced);
    void rememberView(EditorDiagnostics& diagnostics);
    AssetBrowserActions sessionActions(EditorContext& context);
    void acceptItemDrop(const std::filesystem::path& folder);

    std::string rootLabel(const std::filesystem::path& root) const;
    bool isRoot(const std::filesystem::path& path) const;

    void drawToolbar(EditorDiagnostics& diagnostics, const AssetBrowserActions& actions);
    // OS drops: the folder item under a pending drop becomes the target, and
    // the drops inside this browser's window are imported there (or into
    // the current folder).
    void noteDropTarget(const std::filesystem::path& folder);
    void importDroppedFiles(EditorDiagnostics& diagnostics);
    void pollImportDialog(EditorDiagnostics& diagnostics);
    void drawBreadcrumbs();
    void drawFolderPane();
    void drawFolderNode(const EditorAssetFolder& folder, const std::string& label);
    void drawContents(const AssetBrowserActions& actions);
    std::vector<BrowserEntry> visibleEntries() const;
    void drawList(const std::vector<BrowserEntry>& entries, const AssetBrowserActions& actions);
    void drawGrid(const std::vector<BrowserEntry>& entries, const AssetBrowserActions& actions);
    void handleEntryInteraction(const BrowserEntry& entry, bool pressed, const AssetBrowserActions& actions);
    void drawItemMenu(const std::filesystem::path& item, bool folder);
    void drawFolderMenu(const AssetBrowserActions& actions);
    void drawSelectedAsset(const AssetBrowserActions& actions, EditorDiagnostics& diagnostics);
    // A texture's usage or a data table's schema, applied with a reimport.
    void drawImportSettings(const EditorAssetRecord& asset, const AssetBrowserActions& actions,
                            EditorDiagnostics& diagnostics);
    void drawReferencesWindow(const AssetBrowserActions& actions, EditorDiagnostics& diagnostics);
    // The selected material's fields; saved once no widget is active.
    void drawMaterialInspector(const EditorAssetRecord& asset, const AssetBrowserActions& actions,
                               EditorDiagnostics& diagnostics);
    // The drag tooltip's verb: "Move" over a folder, "Assign" over a field
    // taking the asset, else "Place" for models and prefabs.
    const char* dragVerb(bool placeable) const;
    void runDeferredCommand(const AssetBrowserActions& actions, EditorDiagnostics& diagnostics);
    void beginRename(const std::filesystem::path& item);
    // Runs a creating command, then opens the rename dialog on what it
    // selected so the new item is named at once.
    void createAndRename(const std::function<void()>& create);
    void drawModals(EditorDiagnostics& diagnostics);
    std::string textureDetail(const EditorAssetRecord& asset);

    ProjectAssets& m_project;
    // This browser's id with the project, for the primary role.
    uint64_t m_viewId;
    AssetBrowserViewMode m_viewMode;
    std::filesystem::path m_currentFolder;
    std::filesystem::path m_selected;
    std::string m_search;
    std::array<bool, 8> m_shownKinds{true, true, true, true, true, true, true, true};
    // The material the inspector edits: its file, the values shown, and
    // whether they changed since the last save.
    std::filesystem::path m_materialPath;
    std::filesystem::file_time_type m_materialWriteTime{};
    std::optional<MaterialDocument> m_material;
    std::string m_materialError;
    bool m_materialUnsaved{false};
    // The ImGui frame a dragged item last hovered a folder of this browser.
    int m_folderDropHoverFrame{-1};
    std::optional<DeferredCommand> m_deferred;
    // The last service move this view has followed.
    uint64_t m_followedItemMove{0};
    OpenModal m_openModal{OpenModal::None};
    bool m_modalNeedsOpening{false};
    std::filesystem::path m_modalTarget;
    std::string m_renameText;
    bool m_revealCurrentInTree{true};
    // This frame's OS drops and the folder item under them, if any.
    FileDropQueue* m_frameDrops{nullptr};
    std::optional<std::filesystem::path> m_dropFolder;
    NativeFileDialog m_importDialog;
    std::filesystem::path m_importFolder;
    // The import settings shown for the selected asset, edited until Apply.
    std::filesystem::path m_settingsPath;
    nlohmann::json m_settings = nlohmann::json::object();
    DataTableView m_dataTable;
    AssetBrowserReferences m_references;
    std::vector<AssetReferrer> m_deleteReferrers;
};
