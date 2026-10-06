#pragma once

// The editor's own operators, registered family by family from one table.
// Adding a family: write its register function and an adapter from
// EditorOperatorServices next to its operators, then add one line to the
// table in EditorOperatorFamilies.cpp (docs/editor/operators.md).

#include "editor/operators/OperatorRegistry.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

class AreaRegistry;
class ComponentInspectorLayout;
class EditorDocumentCommands;
class EditorMenus;
class EditorObjectModeCommands;
class EditorPlayCommands;
class EditorPlayMode;
class EditorPrefabModeCommands;
class EditorWindowCommands;
class EntityEyedropper;
class GameModuleHost;
class NavigationWorld;
class PackageRunner;
class PreferencesWindow;
class ProjectBuild;
class Scene;
class TextClipboard;
class ViewportRegistry;
class WatchList;
class WorkspaceSet;
struct EditorToolSettings;
struct NavMeshBuildSettings;

// Everything the editor's operator families refer to, built once by the
// session from what it owns. Every reference must outlive the registered
// operators; a family keeps what it needs in its own services.
struct EditorOperatorServices {
    EntityEyedropper& eyedropper;
    // Where the component copy and paste operators exchange values.
    TextClipboard& componentClipboard;
    ComponentInspectorLayout& componentLayout;
    EditorMenus& menus;
    ViewportRegistry& viewports;
    WorkspaceSet& workspaces;
    const AreaRegistry& areaTypes;
    EditorPlayMode& playMode;
    GameModuleHost& modules;
    PreferencesWindow& preferences;
    PackageRunner& packageRunner;
    // The session's build, which the Build panel shows.
    ProjectBuild& build;
    // The fields the Properties panel watches (debug.watch_field).
    WatchList& watches;
    // The preview navigation mesh the viewports draw.
    NavigationWorld& navigationPreview;
    // The project's navigation mesh settings, which navigation.bake uses;
    // changed by Project Settings.
    const NavMeshBuildSettings& navigationSettings;
    const EditorToolSettings& tools;
    // The open .zenith-project; empty when the editor runs without one.
    std::filesystem::path projectFile;
    // The C++ game template's code snippets (module.new_*); empty when the
    // editor found no template.
    std::filesystem::path codeSnippetDirectory;
    // Shows a project file in the primary Asset Browser; may be empty.
    std::function<void(const std::filesystem::path&)> revealAsset;
    // The Simulate system choices changed (to save them); may be empty.
    std::function<void()> simulateMaskChanged;
    // An operator wrote, removed (nullopt), or rewrote on undo or redo a
    // prefab file (absolute path): the session drops cached copies and notes
    // the content in its PrefabWriteLedger; origin is the scene of the
    // document it ran on (PrefabWriteHooks::fileWritten). May be empty.
    std::function<void(const std::filesystem::path&, const std::optional<std::string>&, const Scene*)> prefabWritten;
    // An operator built instances from a prefab file that held this content
    // (PrefabWriteHooks::fileRead). May be empty.
    std::function<void(const std::filesystem::path&, const std::optional<std::string>&)> prefabRead;

    // The running session's workflows (editor/application/
    // EditorSessionCommands.hpp), for the operators that start them.
    EditorDocumentCommands& documentCommands;
    EditorPlayCommands& playCommands;
    EditorObjectModeCommands& objectModeCommands;
    EditorWindowCommands& windowCommands;
    EditorPrefabModeCommands& prefabModeCommands;
};

using OperatorFamilyRegistration = OperatorRegistrationResult (*)(OperatorRegistry& registry,
                                                                  const EditorOperatorServices& services);

struct OperatorFamily {
    // Names the family in registration errors ("<name>: <error>").
    std::string_view name;
    // Registers the whole family; stops at its first failure.
    OperatorFamilyRegistration registerOperators;
};

// The editor's operator families in registration order, which is the
// registry's order and so the F3 search order among equal matches. Menus,
// keymaps, and other families name operators by id only when they run, so
// nothing else depends on the order.
std::span<const OperatorFamily> editorOperatorFamilies();
