# The game's unit tests: doctest cases in one binary, zenith_game_tests, each
# case its own CTest test "game.<case>" (docs/TESTING.md). They cover the
# application session, game settings, the capture camera, the demo gameplay
# (linked and as the zenith_game_demo module), and the game's
# project-agnostic runtime playing the engine's test game module.
zenith_add_test_suite(game
    SOURCES
        ApplicationLifecycleTests.cpp
        CaptureCameraTests.cpp
        DemoGameModuleTests.cpp
        GameSettingsTests.cpp
        ProjectRuntimeTests.cpp
    LIBS
        zenith_demo
    DEFINITIONS
        ZENITH_DEMO_GAME_MODULE="$<TARGET_FILE:zenith_game_demo>"
        ZENITH_TEST_GAME_MODULE="$<TARGET_FILE:zenith_test_game_module>"
        ZENITH_PROJECT_DIR="${PROJECT_SOURCE_DIR}/projects"
    DEPENDS
        zenith_game_demo
        zenith_test_game_module
    NEEDS_ASSETS)
