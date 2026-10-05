include_guard(GLOBAL)

# Exports from a shared library, on Windows, the functions and data its own
# objects define once (tools/export_definitions), so another binary links them
# through the library's import library like on other platforms, where every
# symbol is visible. CMake's WINDOWS_EXPORT_ALL_SYMBOLS would also export each
# inline function and template instance the objects hold, which overflows the
# import library's 65535-member limit once a library is large; those are left
# to every module that uses them. Data still needs ZENITH_ENGINE_API to be
# reached from another binary (engine/module/EngineExport.hpp).
#
# Only MSVC-compatible linkers read the generated module-definition file;
# other Windows toolchains keep WINDOWS_EXPORT_ALL_SYMBOLS.
if(MSVC)
    add_executable(zenith_export_definitions "${PROJECT_SOURCE_DIR}/tools/export_definitions/ExportDefinitions.cpp")
    zenith_target_warnings(zenith_export_definitions)
endif()

function(zenith_export_defined_symbols target)
    if(NOT WIN32)
        return()
    endif()
    if(NOT MSVC)
        set_target_properties(${target} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
        return()
    endif()
    # zenith_target_precompile_headers reads this to keep the precompiled
    # header's object out of the export scan.
    set_target_properties(${target} PROPERTIES ZENITH_EXPORT_DEFINED_SYMBOLS ON)
    set(exports_dir "${CMAKE_CURRENT_BINARY_DIR}/${target}_exports/$<CONFIG>")
    file(GENERATE OUTPUT "${exports_dir}/objects.txt" CONTENT "$<JOIN:$<TARGET_OBJECTS:${target}>,\n>\n")
    add_custom_command(TARGET ${target} PRE_LINK
        COMMAND zenith_export_definitions "${exports_dir}/exports.def" "${exports_dir}/objects.txt"
        COMMENT "Exporting the symbols ${target} defines"
        VERBATIM)
    target_link_options(${target} PRIVATE "/DEF:${exports_dir}/exports.def")
    add_dependencies(${target} zenith_export_definitions)
endfunction()
