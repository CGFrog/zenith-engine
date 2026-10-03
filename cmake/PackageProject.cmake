# Reads the .zenith-project a release package ships and works out where its
# files go in the package. The package layout is fixed: the project file at the
# package root, its scenes under its sceneRoot, its game module at the path it
# names, and its assets in assets/ (where the runtime looks for them).
#
# zenith_read_package_project(<project file>) sets, in the caller's scope:
#   ZENITH_PACKAGE_PROJECT_INSTALL_FILE  the project file to install; a copy in
#                                        the build tree when its assetRoot had
#                                        to be pointed at the packaged assets/
#   ZENITH_PACKAGE_PROJECT_FILE_NAME     its name at the package root
#   ZENITH_PACKAGE_ASSET_DIR             the project's asset root (absolute)
#   ZENITH_PACKAGE_STARTUP_SCENE         the startup scene file (absolute)
#   ZENITH_PACKAGE_SCENE_DESTINATION     its folder in the package
#   ZENITH_PACKAGE_MODULE                the game module file, or empty
#   ZENITH_PACKAGE_MODULE_TARGET         the MODULE target that builds it, or
#                                        empty when the module comes prebuilt
#   ZENITH_PACKAGE_MODULE_DESTINATION    the module's folder in the package
#   ZENITH_PACKAGE_MODULE_NAME           the module's file name in the package
#   ZENITH_PACKAGE_PROJECT_PATHS         package paths ValidatePackage requires
# A project that cannot be packaged stops the configure with the reason.

function(_zenith_package_project_path json key output)
    string(JSON value ERROR_VARIABLE error GET "${json}" "${key}")
    if(error)
        set(value "")
    endif()
    set(${output} "${value}" PARENT_SCOPE)
endfunction()

# A path relative to the project folder that stays inside it, so it can be
# reproduced inside the package.
function(_zenith_require_contained key value project_file)
    if(value STREQUAL "" OR IS_ABSOLUTE "${value}" OR value MATCHES "(^|[/\\\\])[.][.]([/\\\\]|$)")
        message(FATAL_ERROR
            "Cannot package ${project_file}: its ${key} '${value}' must be a relative path inside the project's "
            "folder, so it can be reproduced inside the package.")
    endif()
endfunction()

function(zenith_read_package_project project_file)
    if(NOT project_file MATCHES "[.]zenith-project$")
        message(FATAL_ERROR "ZENITH_PACKAGE_PROJECT must name a .zenith-project file, not '${project_file}'")
    endif()
    if(NOT EXISTS "${project_file}" OR IS_DIRECTORY "${project_file}")
        message(FATAL_ERROR "ZENITH_PACKAGE_PROJECT names a project file that does not exist: ${project_file}")
    endif()
    # In a release tree (which never copies the module beside the project),
    # editing the project file reconfigures so the package follows it. A
    # development tree does not watch it: the editor saves render settings
    # into the project file, which would reconfigure every Debug build.
    if(NOT ZENITH_COPY_PROJECT_MODULE)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${project_file}")
    endif()
    file(READ "${project_file}" project_json)
    string(JSON project_type ERROR_VARIABLE json_error TYPE "${project_json}")
    if(json_error OR NOT project_type STREQUAL "OBJECT")
        message(FATAL_ERROR "Cannot package ${project_file}: it is not a JSON object (${json_error})")
    endif()
    get_filename_component(project_directory "${project_file}" DIRECTORY)
    get_filename_component(project_name "${project_file}" NAME)

    _zenith_package_project_path("${project_json}" assetRoot asset_root)
    _zenith_package_project_path("${project_json}" sceneRoot scene_root)
    _zenith_package_project_path("${project_json}" startupScene startup_scene)
    _zenith_package_project_path("${project_json}" gameModule game_module)
    if(asset_root STREQUAL "")
        message(FATAL_ERROR "Cannot package ${project_file}: it has no assetRoot")
    endif()
    if(startup_scene STREQUAL "")
        message(FATAL_ERROR "Cannot package ${project_file}: it has no startupScene, and the packaged game starts "
            "the project's startup scene")
    endif()
    _zenith_require_contained(sceneRoot "${scene_root}" "${project_file}")
    _zenith_require_contained(startupScene "${startup_scene}" "${project_file}")

    cmake_path(ABSOLUTE_PATH asset_root BASE_DIRECTORY "${project_directory}" NORMALIZE OUTPUT_VARIABLE asset_dir)
    string(REGEX REPLACE "/$" "" asset_dir "${asset_dir}")
    if(NOT IS_DIRECTORY "${asset_dir}")
        message(FATAL_ERROR "Cannot package ${project_file}: its asset root does not exist: ${asset_dir}")
    endif()
    cmake_path(APPEND project_directory "${scene_root}" "${startup_scene}" OUTPUT_VARIABLE scene_file)
    cmake_path(NORMAL_PATH scene_file)
    if(NOT EXISTS "${scene_file}")
        message(FATAL_ERROR "Cannot package ${project_file}: its startup scene does not exist: ${scene_file}")
    endif()
    cmake_path(APPEND scene_root "${startup_scene}" OUTPUT_VARIABLE scene_package_path)
    cmake_path(NORMAL_PATH scene_package_path)
    cmake_path(GET scene_package_path PARENT_PATH scene_destination)
    if(scene_destination STREQUAL "")
        set(scene_destination ".")
    endif()
    set(required_paths "${project_name}" "${scene_package_path}")

    # The runtime reads assets from assets/ beside it, so the packaged project
    # must name that folder; only the assetRoot value is rewritten.
    set(install_file "${project_file}")
    if(NOT asset_root STREQUAL "assets")
        string(REGEX REPLACE "(\"assetRoot\"[ \t\r\n]*:[ \t\r\n]*)\"[^\"]*\"" "\\1\"assets\"" packaged_json
            "${project_json}")
        string(JSON packaged_root ERROR_VARIABLE json_error GET "${packaged_json}" assetRoot)
        if(json_error OR NOT packaged_root STREQUAL "assets")
            message(FATAL_ERROR "Cannot package ${project_file}: could not point its assetRoot at the packaged assets")
        endif()
        set(install_file "${CMAKE_CURRENT_BINARY_DIR}/package-project/${project_name}")
        # Written through a scratch file so an unchanged copy keeps its time.
        file(WRITE "${install_file}.in" "${packaged_json}")
        configure_file("${install_file}.in" "${install_file}" COPYONLY)
    endif()

    set(module "")
    set(module_target "")
    set(module_destination "")
    set(module_name "")
    if(NOT game_module STREQUAL "")
        _zenith_require_contained(gameModule "${game_module}" "${project_file}")
        cmake_path(ABSOLUTE_PATH game_module BASE_DIRECTORY "${project_directory}" NORMALIZE OUTPUT_VARIABLE module)
        cmake_path(NORMAL_PATH game_module OUTPUT_VARIABLE module_package_path)
        cmake_path(GET module_package_path PARENT_PATH module_destination)
        if(module_destination STREQUAL "")
            set(module_destination ".")
        endif()
        cmake_path(GET module_package_path FILENAME module_name)
        cmake_path(GET module_package_path STEM LAST_ONLY module_stem)
        # A module this tree builds is packaged from the configuration being
        # built, never from the copy beside the project, which may be another
        # configuration's.
        if(TARGET "${module_stem}")
            get_target_property(module_type "${module_stem}" TYPE)
            if(module_type STREQUAL "MODULE_LIBRARY")
                set(module_target "${module_stem}")
            endif()
        endif()
        if(module_target STREQUAL "" AND NOT EXISTS "${module}")
            message(FATAL_ERROR "Cannot package ${project_file}: its game module ${module} does not exist and no "
                "MODULE target named ${module_stem} builds it")
        endif()
        list(APPEND required_paths "${module_package_path}")
    endif()

    set(ZENITH_PACKAGE_PROJECT_INSTALL_FILE "${install_file}" PARENT_SCOPE)
    set(ZENITH_PACKAGE_PROJECT_FILE_NAME "${project_name}" PARENT_SCOPE)
    set(ZENITH_PACKAGE_ASSET_DIR "${asset_dir}" PARENT_SCOPE)
    set(ZENITH_PACKAGE_STARTUP_SCENE "${scene_file}" PARENT_SCOPE)
    set(ZENITH_PACKAGE_SCENE_DESTINATION "${scene_destination}" PARENT_SCOPE)
    set(ZENITH_PACKAGE_MODULE "${module}" PARENT_SCOPE)
    set(ZENITH_PACKAGE_MODULE_TARGET "${module_target}" PARENT_SCOPE)
    set(ZENITH_PACKAGE_MODULE_DESTINATION "${module_destination}" PARENT_SCOPE)
    set(ZENITH_PACKAGE_MODULE_NAME "${module_name}" PARENT_SCOPE)
    set(ZENITH_PACKAGE_PROJECT_PATHS "${required_paths}" PARENT_SCOPE)
endfunction()
