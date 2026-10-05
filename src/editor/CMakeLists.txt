# The editor: the zenith_editor_app library (everything but main) that the
# editor tests link, the zenith_editor executable, and its runtime resources.

add_library(zenith_editor_app STATIC
    application/EditorApplication.cpp
    application/EditorAreaTypes.cpp
    application/EditorAppearance.cpp
    application/DocumentStack.cpp
    application/EditorAreas.cpp
    application/EditorDiagnostics.cpp
    application/EditorModuleCommands.cpp
    application/EditorObjectCommands.cpp
    application/NewCodeDialog.cpp
    application/EditorProjectSettings.cpp
    interaction/WatchList.cpp
    application/EditorViewports.cpp
    application/EditorPlayMode.cpp
    application/EditorPrefabMode.cpp
    application/EditorPlayback.cpp
    application/EditorPreferences.cpp
    application/NativeFileDialog.cpp
    application/PlayTargets.cpp
    application/PrefabInstanceRefresh.cpp
    application/EditorSession.cpp
    application/EditorFileDialogs.cpp
    application/EditorMainMenu.cpp
    application/EditorRecovery.cpp
    application/EditorStatusBar.cpp
    application/EditorSessionOperators.cpp
    application/EditorSessionSmoke.cpp
    application/EntityCommands.cpp
    application/SceneEditing.cpp
    application/SystemClipboard.cpp
    areas/AreaRegistry.cpp
    areas/AreaWindows.cpp
    areas/DefaultWorkspaces.cpp
    areas/Workspace.cpp
    areas/WorkspaceFile.cpp
    document/CommandHistory.cpp
    document/DefaultScene.cpp
    document/EditorDocument.cpp
    document/EditorRecovery.cpp
    document/PrefabAuthoring.cpp
    document/PrefabEditDocument.cpp
    document/PrefabWriteCommand.cpp
    document/SceneEditorData.cpp
    document/RuntimeApply.cpp
    document/SceneFragment.cpp
    interaction/ComponentEditorOverrides.cpp
    interaction/ComponentEditors.cpp
    interaction/EditorCamera.cpp
    interaction/EntityEyedropper.cpp
    interaction/EntityPropertiesView.cpp
    interaction/AssetReferenceField.cpp
    interaction/EntityReferenceField.cpp
    interaction/ModalTransform.cpp
    interaction/PickQueries.cpp
    interaction/GraphicsSettingsEditor.cpp
    interaction/ReflectedFieldEditor.cpp
    interaction/Selection.cpp
    menus/AddComponentMenu.cpp
    menus/DefaultMenus.cpp
    menus/EditorMenus.cpp
    menus/MenuPopup.cpp
    menus/MenuRegistry.cpp
    menus/PieMenu.cpp
    modules/GameModuleHost.cpp
    operators/ComponentOperators.cpp
    operators/BuildOperators.cpp
    operators/CodeOperators.cpp
    operators/DebugOperators.cpp
    operators/CollectionOperators.cpp
    operators/DefaultKeymap.cpp
    operators/EditorOperatorFamilies.cpp
    operators/Keymap.cpp
    operators/KeymapDispatch.cpp
    operators/KeymapEditor.cpp
    operators/LastOperatorPanel.cpp
    operators/EyedropperOperators.cpp
    operators/ModelOperators.cpp
    operators/ObjectOperators.cpp
    operators/PrefabOperators.cpp
    operators/Operator.cpp
    operators/OperatorHistory.cpp
    operators/OperatorRegistry.cpp
    operators/OperatorRunner.cpp
    operators/OperatorSearch.cpp
    operators/NavigationOperators.cpp
    operators/PlayOperators.cpp
    operators/OutlinerOperators.cpp
    operators/PackageOperators.cpp
    operators/PlacementOperators.cpp
    operators/PreferencesOperators.cpp
    operators/SelectOperators.cpp
    operators/VisibilityOperators.cpp
    panels/AssetBrowserPanel.cpp
    panels/BuildPanel.cpp
    panels/ProjectSettingsWindow.cpp
    panels/DiagnosticsPanel.cpp
    panels/EditorContext.cpp
    panels/GameModulePanel.cpp
    panels/GraphicsPanel.cpp
    panels/HierarchyPanel.cpp
    panels/InfoPanel.cpp
    panels/InspectorPanel.cpp
    panels/PreferencesWindow.cpp
    panels/PropertiesPanel.cpp
    panels/RenderSettingsControls.cpp
    panels/RenderStatisticsPanel.cpp
    panels/SimulateSystemPicker.cpp
    panels/SystemProfilerPanel.cpp
    panels/ViewportChrome.cpp
    panels/ViewportOperators.cpp
    panels/ViewportPanel.cpp
    panels/ViewportRegistry.cpp
    panels/outliner/OrphanData.cpp
    panels/outliner/OutlinerRowWidgets.cpp
    panels/outliner/OutlinerTree.cpp
    project/AssetCatalog.cpp
    project/AssetRelocation.cpp
    project/BuildRunner.cpp
    project/DeveloperEnvironment.cpp
    project/FastProjectBuild.cpp
    project/ProjectBuild.cpp
    project/TemplateText.cpp
    project/CodeGenerator.cpp
    project/CompilerOutputParser.cpp
    project/DerivedAssetInspector.cpp
    project/ExternalEditor.cpp
    project/IrradianceVolumeBaker.cpp
    project/PackageRunner.cpp
    project/MaterialDocument.cpp
    project/ProjectAssets.cpp
    project/ProjectFileOperations.cpp
    project/ProjectHub.cpp
    project/ProjectTemplate.cpp
    project/RecentProjects.cpp
    project/ReflectionProbeBaker.cpp
    project/ThumbnailCache.cpp
    project/ThumbnailTextures.cpp
    settings/AssetBrowserSettings.cpp
    settings/ComponentInspectorLayout.cpp
    settings/EditorRenderSettings.cpp
    settings/EditorSettings.cpp
    settings/RenderSettingsController.cpp
    settings/SimulateSystemSettings.cpp
    smoke/ComponentSmokeStep.cpp
    smoke/CursorSmokeStep.cpp
    smoke/EditorSmokeScenario.cpp
    smoke/GeneratedPreviewSmokeStep.cpp
    smoke/HideSmokeStep.cpp
    smoke/LiveEditSmokeStep.cpp
    smoke/MenuSmokeStep.cpp
    smoke/ModuleReloadSmokeStep.cpp
    smoke/MultiViewportSmokeStep.cpp
    smoke/OutlinerSmokeStep.cpp
    smoke/PickingSmokeStep.cpp
    smoke/PieMenuSmokeStep.cpp
    smoke/PlayTargetSmokeStep.cpp
    smoke/PrefabEditSmokeStep.cpp
    smoke/PrefabSmokeStep.cpp
    smoke/SimulateSmokeStep.cpp
    smoke/SmokeRunner.cpp
    smoke/StartupSmokeSteps.cpp
    smoke/StreamingSmokeSteps.cpp
    smoke/ViewportChromeSmokeStep.cpp
    smoke/WorkspaceSmokeStep.cpp
    theme/EditorPalette.cpp
    theme/EditorTheme.cpp
    theme/EditorWidgets.cpp
    theme/EntityIcon.cpp
    theme/PixelIconBitmaps.cpp
    theme/PixelIcons.cpp
    theme/PixelPainter.cpp
    theme/ThemeChrome.cpp
    viewport/SceneGizmoDraw.cpp
    viewport/BoxSelect.cpp
    viewport/Cursor3D.cpp
    viewport/GameUiOverlay.cpp
    viewport/IconPicking.cpp
    viewport/LassoSelect.cpp
    viewport/MultiGizmo.cpp
    viewport/RegionSelect.cpp
    viewport/SnapQueries.cpp
    viewport/ViewFraming.cpp
    viewport/ViewportDrop.cpp
    viewport/ViewportPickProvider.cpp
    viewport/ViewportPicker.cpp
    viewport/ViewportSelectOperators.cpp
    theme/ThemeParameters.cpp
)
target_link_libraries(zenith_editor_app PUBLIC zenith_engine)
if(WIN32)
    # Shell file operations (Recycle Bin, Explorer) in ProjectFileOperations.
    target_link_libraries(zenith_editor_app PRIVATE ole32 shell32)
endif()
target_compile_definitions(zenith_editor_app PRIVATE GLM_ENABLE_EXPERIMENTAL)

add_executable(zenith_editor main.cpp)
target_link_libraries(zenith_editor PRIVATE zenith_editor_app)

# Editor-only runtime resources live beside assets/ under <resource root>/editor.
set(ZENITH_EDITOR_FONT_BINARY_DIR "${PROJECT_BINARY_DIR}/editor/fonts")
add_custom_target(zenith_editor_resources
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${ZENITH_EDITOR_FONT_BINARY_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${inter_SOURCE_DIR}/extras/ttf/Inter-Regular.ttf"
        "${inter_SOURCE_DIR}/LICENSE.txt"
        "${ZENITH_PIXEL_FONT_DIR}/VT323-Regular.ttf"
        "${ZENITH_PIXEL_FONT_DIR}/VT323-OFL.txt"
        "${ZENITH_EDITOR_FONT_BINARY_DIR}"
    COMMENT "Copying Zenith editor resources"
)
add_dependencies(zenith_editor zenith_editor_resources)

# Project templates for the editor's Project Hub (New from Template). The
# editor looks in <resource root>/templates, where the build copies them, and
# then in the source tree's templates/. Generated projects build their game
# module against this build directory (ZenithEngineConfig.cmake, generated by
# the root CMakeLists.txt) in the editor's own configuration.
set(ZENITH_TEMPLATE_SOURCE_DIR "${PROJECT_SOURCE_DIR}/templates")
add_custom_target(zenith_editor_templates
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${ZENITH_TEMPLATE_SOURCE_DIR}" "${PROJECT_BINARY_DIR}/templates"
    COMMENT "Copying Zenith project templates"
)
add_dependencies(zenith_editor zenith_editor_templates)
target_compile_definitions(zenith_editor PRIVATE
    ZENITH_SOURCE_TEMPLATE_DIR="${ZENITH_TEMPLATE_SOURCE_DIR}"
    ZENITH_ENGINE_BUILD_DIR="${PROJECT_BINARY_DIR}"
    ZENITH_EDITOR_CONFIGURATION="$<CONFIG>"
)

zenith_target_warnings(zenith_editor_app)
zenith_target_precompile_headers(zenith_editor_app ZenithEditorPch.hpp)
