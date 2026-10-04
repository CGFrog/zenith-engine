zenith_add_test(zenith_application_lifecycle EXECUTABLE zenith_application_tests
    SOURCES ApplicationLifecycleTests.cpp LIBS zenith_demo NEEDS_ASSETS)
zenith_add_test(zenith_game_settings SOURCES GameSettingsTests.cpp LIBS zenith_demo)
zenith_add_test(zenith_demo_game_module SOURCES DemoGameModuleTests.cpp LIBS zenith_demo
    DEFINITIONS
        ZENITH_DEMO_GAME_MODULE="$<TARGET_FILE:zenith_game_demo>"
        ZENITH_PROJECT_DIR="${PROJECT_SOURCE_DIR}/projects"
    DEPENDS zenith_game_demo)
# The game's project-agnostic runtime; plays the engine's test game module
# (tests/engine) and the demo module.
zenith_add_test(zenith_project_runtime SOURCES ProjectRuntimeTests.cpp LIBS zenith_demo
    DEFINITIONS
        ZENITH_TEST_GAME_MODULE="$<TARGET_FILE:zenith_test_game_module>"
        ZENITH_DEMO_GAME_MODULE="$<TARGET_FILE:zenith_game_demo>"
    DEPENDS zenith_test_game_module zenith_game_demo)
zenith_add_test(zenith_capture_camera SOURCES CaptureCameraTests.cpp LIBS zenith_demo)
