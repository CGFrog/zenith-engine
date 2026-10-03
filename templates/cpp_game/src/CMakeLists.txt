# The {{PROJECT_NAME}} game module: component types and systems registered
# through ZENITH_GAME_MODULE. Add new source files to this list.
add_library({{PROJECT_IDENTIFIER}} MODULE
    GameModule.cpp
    SpinnerGameplay.cpp
)
target_include_directories({{PROJECT_IDENTIFIER}} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
# Only the engine: a module shares the editor's (or game's) engine library.
target_link_libraries({{PROJECT_IDENTIFIER}} PRIVATE Zenith::Engine)
if(MSVC)
    target_compile_options({{PROJECT_IDENTIFIER}} PRIVATE /W4 /permissive-)
else()
    target_compile_options({{PROJECT_IDENTIFIER}} PRIVATE -Wall -Wextra -Wpedantic)
endif()

# The project file names modules/{{PROJECT_IDENTIFIER}}.zmodule. Each build
# copies the library there; the editor notices the change and hot reloads it.
set(ZENITH_PROJECT_MODULE "${PROJECT_SOURCE_DIR}/modules/{{PROJECT_IDENTIFIER}}.zmodule")
add_custom_command(TARGET {{PROJECT_IDENTIFIER}} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${PROJECT_SOURCE_DIR}/modules"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:{{PROJECT_IDENTIFIER}}>" "${ZENITH_PROJECT_MODULE}"
    VERBATIM)
