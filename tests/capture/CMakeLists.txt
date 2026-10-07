# Renders each reference set's views and compares them with
# tests/references/<set> (docs/RENDERER_VALIDATION.md). Skips without a
# hardware Vulkan device; zenith_update_<set>_references regenerates one
# set and zenith_update_references regenerates all of them.
# The driver probes for a hardware device with the tests' shared Vulkan
# probe (its own volk instance; the engine's is private). Captures sample the cooked BC7/BC5 textures, so they must be
# current.
zenith_add_test_executable(zenith_reference_capture SOURCES ReferenceCaptureDriver.cpp
    LIBS zenith_engine zenith_test_support zenith_test_vulkan_probe
    DEPENDS zenith zenith_cooked_textures zenith_debug_draw_smoke_tests zenith_scene_gizmo_smoke_tests)
add_custom_target(zenith_update_references)
# Each set renders with the game, except debug_draw and scene_gizmos,
# whose fixed scenes are drawn by Engine-hosted smoke test executables.
foreach(reference_set IN ITEMS showcase irradiance_room materials debug_draw scene_gizmos)
    set(capture_executable zenith)
    if(reference_set STREQUAL "debug_draw")
        set(capture_executable zenith_debug_draw_smoke_tests)
    elseif(reference_set STREQUAL "scene_gizmos")
        set(capture_executable zenith_scene_gizmo_smoke_tests)
    endif()
    set(capture_manifest "${PROJECT_SOURCE_DIR}/tests/references/${reference_set}/captures.json")
    set(capture_arguments
        --executable "$<TARGET_FILE:${capture_executable}>"
        --manifest "${capture_manifest}"
        --output "${PROJECT_BINARY_DIR}/captures/${reference_set}")
    # One CTest per manifest view, so views capture in parallel and a
    # failure names its view. Editing the manifest re-runs configure.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${capture_manifest}")
    file(READ "${capture_manifest}" capture_manifest_json)
    string(JSON capture_view_count LENGTH "${capture_manifest_json}" views)
    math(EXPR capture_last_view "${capture_view_count} - 1")
    foreach(capture_view_index RANGE ${capture_last_view})
        string(JSON capture_view GET "${capture_manifest_json}" views ${capture_view_index} name)
        zenith_add_test(zenith_${reference_set}_capture_${capture_view}
            EXECUTABLE zenith_reference_capture COMMAND_ARGS ${capture_arguments} --view ${capture_view}
            LABELS vulkan capture SKIP_RETURN_CODE 77 TIMEOUT 120 PROCESSORS 2)
    endforeach()
    add_custom_target(zenith_update_${reference_set}_references
        COMMAND zenith_reference_capture ${capture_arguments} --update
        DEPENDS zenith_reference_capture ${capture_executable} zenith_cooked_textures
        COMMENT "Regenerating ${reference_set} reference captures"
        VERBATIM
    )
    add_dependencies(zenith_update_references zenith_update_${reference_set}_references)
endforeach()
