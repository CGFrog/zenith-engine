add_subdirectory(engine)
add_subdirectory(game)
add_subdirectory(editor)

# The game executable; main.cpp is the composition root.
add_executable(zenith main.cpp)
target_link_libraries(zenith PRIVATE zenith_demo)
# The demo project plays through the zenith_game_demo module, so a build of
# the game also builds the module (and refreshes the copy the project names).
add_dependencies(zenith zenith_game_demo)
if(APPLE)
    set_target_properties(zenith PROPERTIES MACOSX_BUNDLE TRUE
        OUTPUT_NAME "Zenith"
        MACOSX_BUNDLE_BUNDLE_NAME "Zenith"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.zenith.demo"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
endif()

# The offline asset tool; the build runs it to cook textures and meshes (root
# CMakeLists.txt, zenith_cooked_textures and zenith_cooked_meshes).
add_executable(zenith_asset_build asset_build_main.cpp)
target_link_libraries(zenith_asset_build PRIVATE zenith_engine)
