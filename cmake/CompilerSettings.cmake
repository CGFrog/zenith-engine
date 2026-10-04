include_guard(GLOBAL)

# The project's warning level for its own targets (never third-party code).
function(zenith_target_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# Build speed settings. This file is included after project() and before the
# dependencies, so they cover third-party targets too.

# Compiler cache: auto uses sccache, then ccache, when one is on PATH; sccache
# or ccache requires that program; off disables caching. CMake runs a compiler
# launcher only with the Ninja and Makefile generators, so the Visual Studio
# generator builds uncached (configure from a developer prompt with -G Ninja
# to use one). MSVC caches only objects that embed their debug information
# (/Z7), so a cached MSVC build switches from /Zi to /Z7.
set(ZENITH_COMPILER_CACHE "auto" CACHE STRING "Compiler cache to launch compilers through: auto, sccache, ccache, or off")
set_property(CACHE ZENITH_COMPILER_CACHE PROPERTY STRINGS auto sccache ccache off)

set(ZENITH_COMPILER_CACHE_PROGRAM "")
if(ZENITH_COMPILER_CACHE STREQUAL "auto")
    find_program(ZENITH_SCCACHE_EXECUTABLE sccache)
    find_program(ZENITH_CCACHE_EXECUTABLE ccache)
    if(ZENITH_SCCACHE_EXECUTABLE)
        set(ZENITH_COMPILER_CACHE_PROGRAM "${ZENITH_SCCACHE_EXECUTABLE}")
    elseif(ZENITH_CCACHE_EXECUTABLE)
        set(ZENITH_COMPILER_CACHE_PROGRAM "${ZENITH_CCACHE_EXECUTABLE}")
    endif()
elseif(ZENITH_COMPILER_CACHE MATCHES "^(sccache|ccache)$")
    string(TOUPPER "${ZENITH_COMPILER_CACHE}" cache_name)
    find_program(ZENITH_${cache_name}_EXECUTABLE ${ZENITH_COMPILER_CACHE})
    if(NOT ZENITH_${cache_name}_EXECUTABLE)
        message(FATAL_ERROR "ZENITH_COMPILER_CACHE is ${ZENITH_COMPILER_CACHE}, which is not on PATH")
    endif()
    set(ZENITH_COMPILER_CACHE_PROGRAM "${ZENITH_${cache_name}_EXECUTABLE}")
elseif(NOT ZENITH_COMPILER_CACHE STREQUAL "off")
    message(FATAL_ERROR "ZENITH_COMPILER_CACHE must be auto, sccache, ccache, or off (got ${ZENITH_COMPILER_CACHE})")
endif()

if(ZENITH_COMPILER_CACHE_PROGRAM AND CMAKE_GENERATOR MATCHES "Visual Studio")
    message(STATUS "Compiler cache ${ZENITH_COMPILER_CACHE_PROGRAM} not used: "
        "the ${CMAKE_GENERATOR} generator does not run compiler launchers (use Ninja)")
    set(ZENITH_COMPILER_CACHE_PROGRAM "")
endif()
if(ZENITH_COMPILER_CACHE_PROGRAM)
    message(STATUS "Compiler cache: ${ZENITH_COMPILER_CACHE_PROGRAM}")
    set(CMAKE_C_COMPILER_LAUNCHER "${ZENITH_COMPILER_CACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${ZENITH_COMPILER_CACHE_PROGRAM}")
    if(MSVC)
        set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
    endif()
endif()

# Precompiled headers for the engine and editor libraries. MSVC compiler
# caches cannot cache translation units that use a precompiled header, so the
# default is off when one is in use; set ZENITH_USE_PCH explicitly to choose.
if(MSVC AND ZENITH_COMPILER_CACHE_PROGRAM)
    set(zenith_use_pch_default OFF)
else()
    set(zenith_use_pch_default ON)
endif()
option(ZENITH_USE_PCH "Precompile common standard and third-party headers" ${zenith_use_pch_default})

# Uses <header> (relative to the calling directory) as <target>'s
# precompiled header for its C++ sources when ZENITH_USE_PCH is on. The header
# must hold only standard and third-party includes that no source depends on
# implicitly: every source still includes what it uses, and the tree builds
# with the option off.
#
# A Windows shared library that exports every symbol (WINDOWS_EXPORT_ALL_SYMBOLS)
# would also export what the header's own object file defines: on MSVC a
# __@@_PchSym_ marker the linker cannot resolve as an export, and template
# instances from the headers. Such a target instead reuses the header compiled
# in <target>_pch, a static library with the same compile settings, so only
# the target's own objects are scanned for exports.
function(zenith_target_precompile_headers target header)
    if(NOT ZENITH_USE_PCH)
        return()
    endif()
    get_filename_component(header_path "${header}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    set(pch_header "$<$<COMPILE_LANGUAGE:CXX>:${header_path}>")

    get_target_property(target_type ${target} TYPE)
    get_target_property(exports_all_symbols ${target} WINDOWS_EXPORT_ALL_SYMBOLS)
    if(NOT (WIN32 AND target_type STREQUAL "SHARED_LIBRARY" AND exports_all_symbols))
        target_precompile_headers(${target} PRIVATE "${pch_header}")
        return()
    endif()

    set(pch_target ${target}_pch)
    set(pch_source "${CMAKE_CURRENT_BINARY_DIR}/${pch_target}.cpp")
    file(CONFIGURE OUTPUT "${pch_source}" CONTENT "// Compiles ${target}'s precompiled header.\n")
    add_library(${pch_target} STATIC "${pch_source}")
    # Compile exactly as <target> does: its definitions (with the export macro
    # CMake and Visual Studio define for a shared library), include
    # directories, and options, all including those from its dependencies.
    get_target_property(define_symbol ${target} DEFINE_SYMBOL)
    if(NOT define_symbol)
        set(define_symbol ${target}_EXPORTS)
    endif()
    set(library_definitions ${define_symbol})
    if(CMAKE_GENERATOR MATCHES "Visual Studio")
        list(APPEND library_definitions _WINDLL)
    endif()
    set_target_properties(${pch_target} PROPERTIES
        COMPILE_DEFINITIONS "$<TARGET_PROPERTY:${target},COMPILE_DEFINITIONS>;${library_definitions}"
        INCLUDE_DIRECTORIES "$<TARGET_PROPERTY:${target},INCLUDE_DIRECTORIES>"
        COMPILE_OPTIONS "$<TARGET_PROPERTY:${target},COMPILE_OPTIONS>")
    target_precompile_headers(${pch_target} PRIVATE "${pch_header}")
    target_precompile_headers(${target} REUSE_FROM ${pch_target})
endfunction()

# The Visual Studio generator otherwise compiles one project's sources one at
# a time, leaving zenith_engine's and the larger dependencies' sources to a
# single core. MultiToolTask runs them in parallel, and
# EnforceProcessCountAcrossBuilds keeps the total across the projects MSBuild
# builds at once (cmake --build --parallel) to one compiler per logical core.
option(ZENITH_VS_PARALLEL_COMPILE "Compile a Visual Studio project's sources in parallel (MultiToolTask)" ON)
if(ZENITH_VS_PARALLEL_COMPILE AND CMAKE_GENERATOR MATCHES "Visual Studio")
    list(APPEND CMAKE_VS_GLOBALS "UseMultiToolTask=true" "EnforceProcessCountAcrossBuilds=true")
endif()
