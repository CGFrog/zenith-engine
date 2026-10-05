# The demo: its gameplay, its game module, and the application composition
# the zenith executable (src/CMakeLists.txt) runs.

# The demo's gameplay: component types and systems registered through
# registerDemoGameplay. Linked into the zenith_game_demo module the editor and
# the game load for the demo project, and statically into the zenith
# executable for runs without a game module (the built-in demo room and
# module-less projects), so every host runs the same registration.
add_library(zenith_demo_gameplay STATIC
    gameplay/DemoGameplay.cpp
    gameplay/ItemCollection.cpp
    input/DemoInput.cpp
)
set_target_properties(zenith_demo_gameplay PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(zenith_demo_gameplay PUBLIC zenith_engine)

# The demo game module. Each build also copies it to
# projects/modules/zenith_game_demo.zmodule, the platform-independent,
# project-relative path projects/demo.zenith-project names, so the editor can
# open the checked-in project from any build directory. The copy is the most
# recently built configuration; a module built for another configuration is
# refused by its build hash. Release trees made by Build and Package turn the
# copy off, so they never replace the module the editor has loaded.
add_library(zenith_game_demo MODULE module/DemoGameModule.cpp)
target_link_libraries(zenith_game_demo PRIVATE zenith_demo_gameplay)
option(ZENITH_COPY_PROJECT_MODULE "Copy the built demo module beside projects/demo.zenith-project" ON)
if(ZENITH_COPY_PROJECT_MODULE)
    set(ZENITH_DEMO_PROJECT_MODULE "${PROJECT_SOURCE_DIR}/projects/modules/zenith_game_demo.zmodule")
    add_custom_command(TARGET zenith_game_demo POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${PROJECT_SOURCE_DIR}/projects/modules"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:zenith_game_demo>" "${ZENITH_DEMO_PROJECT_MODULE}"
        VERBATIM)
endif()

# Application composition for the zenith executable: its command line,
# package verification, the gameplay it plays (a project's game module, or
# the demo gameplay linked in for runs without one), the project-agnostic
# ProjectApplication, and the demo's menus, settings, audio, and scenes.
add_library(zenith_demo STATIC
    application/DebugLabelOverlay.cpp
    application/DemoApplication.cpp
    application/GameLaunch.cpp
    application/GameSettings.cpp
    application/LinkedGameplay.cpp
    application/PackageVerification.cpp
    application/ProjectApplication.cpp
    application/RuntimeGameplay.cpp
    scene/CaptureCamera.cpp
    scene/DemoScene.cpp
    scene/GameplayScene.cpp
)
target_link_libraries(zenith_demo PUBLIC zenith_demo_gameplay)

foreach(demo_target IN ITEMS zenith_demo_gameplay zenith_game_demo zenith_demo)
    zenith_target_warnings(${demo_target})
endforeach()
