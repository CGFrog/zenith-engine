cmake_policy(VERSION 3.24)

if(NOT DEFINED ZENITH_PACKAGE_ROOT OR ZENITH_PACKAGE_ROOT STREQUAL "")
    message(FATAL_ERROR "ZENITH_PACKAGE_ROOT is required")
endif()
if(NOT DEFINED ZENITH_PACKAGE_VERSION)
    set(ZENITH_PACKAGE_VERSION "unknown")
endif()
# The packaged project's files (project file, startup scene, game module), as
# cmake/PackageProject.cmake lists them.
if(NOT DEFINED ZENITH_PACKAGE_PROJECT_PATHS OR ZENITH_PACKAGE_PROJECT_PATHS STREQUAL "")
    message(FATAL_ERROR "ZENITH_PACKAGE_PROJECT_PATHS is required")
endif()

cmake_path(ABSOLUTE_PATH ZENITH_PACKAGE_ROOT NORMALIZE OUTPUT_VARIABLE package_root)
set(required_paths
    "assets"
    "assets/basic_map.gltf"
    "assets/audio/ambient.ogg"
    "assets/audio/interaction.wav"
    "shaders"
    "licenses"
    ${ZENITH_PACKAGE_PROJECT_PATHS}
    "shaders/mesh.vert.spv"
    "shaders/mesh.frag.spv"
    "shaders/post.frag.spv"
)
if(WIN32)
    list(APPEND required_paths "zenith.exe")
elseif(APPLE)
    list(APPEND required_paths "Zenith.app/Contents/MacOS/Zenith")
else()
    list(APPEND required_paths "zenith")
endif()
foreach(required_path IN LISTS required_paths)
    if(NOT EXISTS "${package_root}/${required_path}")
        message(FATAL_ERROR "Package is missing required path: ${required_path}")
    endif()
endforeach()

file(GLOB_RECURSE package_entries RELATIVE "${package_root}" LIST_DIRECTORIES false "${package_root}/*")
list(SORT package_entries)
set(case_folded_paths)
set(manifest_entries)
foreach(relative_path IN LISTS package_entries)
    if(relative_path STREQUAL "content-manifest.json")
        continue()
    endif()
    if(IS_ABSOLUTE "${relative_path}" OR relative_path MATCHES "(^|/)[.][.](/|$)")
        message(FATAL_ERROR "Package contains an escaping path: ${relative_path}")
    endif()
    file(REAL_PATH "${package_root}/${relative_path}" resolved_path)
    cmake_path(IS_PREFIX package_root "${resolved_path}" NORMALIZE is_contained)
    if(NOT is_contained)
        message(FATAL_ERROR "Package entry resolves outside the package: ${relative_path}")
    endif()
    string(TOLOWER "${relative_path}" folded_path)
    list(FIND case_folded_paths "${folded_path}" conflicting_path_index)
    if(NOT conflicting_path_index EQUAL -1)
        message(FATAL_ERROR "Package contains duplicate or case-conflicting path: ${relative_path}")
    endif()
    list(APPEND case_folded_paths "${folded_path}")

    file(SHA256 "${package_root}/${relative_path}" digest)
    string(REPLACE "\\" "/" manifest_path "${relative_path}")
    string(REPLACE "\"" "\\\"" manifest_path "${manifest_path}")
    list(APPEND manifest_entries "    {\"path\": \"${manifest_path}\", \"sha256\": \"${digest}\"}")
endforeach()

list(JOIN manifest_entries ",\n" manifest_files)
file(WRITE "${package_root}/content-manifest.json"
    "{\n  \"formatVersion\": 1,\n  \"productVersion\": \"${ZENITH_PACKAGE_VERSION}\",\n  \"files\": [\n${manifest_files}\n  ]\n}\n")
file(SHA256 "${package_root}/content-manifest.json" manifest_digest)
file(WRITE "${package_root}/release-provenance.json"
    "{\n"
    "  \"formatVersion\": 1,\n"
    "  \"sourceRevision\": \"${ZENITH_SOURCE_REVISION}\",\n"
    "  \"dependencyRevision\": \"${ZENITH_DEPENDENCY_REVISION}\",\n"
    "  \"toolchain\": \"${ZENITH_TOOLCHAIN}\",\n"
    "  \"contentManifestDigest\": \"${manifest_digest}\",\n"
    "  \"targetPlatform\": \"${ZENITH_TARGET_PLATFORM}\"\n"
    "}\n")
file(WRITE "${package_root}/RELEASE-NOTES.txt"
    "Zenith ${ZENITH_PACKAGE_VERSION}\n"
    "Source revision: ${ZENITH_SOURCE_REVISION}\n"
    "Target: ${ZENITH_TARGET_PLATFORM}\n"
    "Content manifest: ${manifest_digest}\n")
message(STATUS "Validated package content and wrote ${package_root}/content-manifest.json")
