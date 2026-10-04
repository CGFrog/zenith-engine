#pragma once

// The editor's own operators, registered family by family from one table.
// Adding a family: write its register function and an adapter from
// EditorOperatorServices next to its operators, then add one line to the
// table in EditorOperatorFamilies.cpp (docs/editor/operators.md).

#include "editor/operators/OperatorRegistry.hpp"

#include <filesystem>
#include <functional>
#include <span>
#include <string_view>

class AreaRegistry;
class CommandHistory;
class ComponentInspectorLayout;
class EditorDocument;
class EditorDocumentCommands;
class EditorMenus;
class EditorObjectModeCommands;
class EditorPlayCommands;
class EditorPlayMode;
class EditorWindowCommands;
class EntityEyedropper;
class GameModuleHost;
class NavigationWorld;
class PackageRunner;
class PreferencesWindow;
class ProjectBuild;
class TextClipboard;
class ViewportRegistry;
class WorkspaceSet;
struct EditorToolSettings;

// Everything the editor's operator families refer to, built once by the
// session from what it owns. Every reference must outlive the registered
// operators; a family keeps what it needs in its own services.
struct EditorOperatorServices {
    EditorDocument& document;
    CommandHistory& history;
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
    // The preview navigation mesh the viewports draw.
    NavigationWorld& navigationPreview;
    const EditorToolSettings& tools;
    // The open .zenith-project; empty when the editor runs without one.
    std::filesystem::path projectFile;
    // Shows a project file in the primary Asset Browser; may be empty.
    std::function<void(const std::filesystem::path&)> revealAsset;
    // The Simulate system choices changed (to save them); may be empty.
    std::function<void()> simulateMaskChanged;

    // The running session's workflows (editor/application/
    // EditorSessionCommands.hpp), for the operators that start them.
    EditorDocumentCommands& documentCommands;
    EditorPlayCommands& playCommands;
    EditorObjectModeCommands& objectModeCommands;
    EditorWindowCommands& windowCommands;
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
