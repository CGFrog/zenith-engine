include_guard(GLOBAL)

# The gameplay API: the engine headers a game module may include, and a
# compile-only check that keeps them free of SDL, Vulkan, Jolt, ImGui and
# editor types (docs/engine/gameplay-api.md). Paths are relative to src/.
# Adding a header here makes it part of the gameplay API.
set(ZENITH_GAMEPLAY_API_HEADERS
    engine/module/ModuleApi.hpp
    engine/module/ModuleRegistrar.hpp
    engine/module/EngineExport.hpp
    engine/module/SystemScheduler.hpp
    engine/module/GameServices.hpp
    engine/module/GameServiceGroups.hpp
    engine/module/GameEvents.hpp
    engine/module/EventQueue.hpp
    engine/module/EntityCommandBuffer.hpp
    engine/module/Effects.hpp
    engine/module/Timers.hpp
    engine/module/SessionResources.hpp
    engine/behaviour/Behaviour.hpp
    engine/scene/Scene.hpp
    engine/scene/SceneEntityId.hpp
    engine/scene/ComponentTypes.hpp
    engine/scene/Tags.hpp
    engine/scene/Prefab.hpp
    engine/physics/PhysicsWorld.hpp
    engine/physics/PhysicsComponents.hpp
    engine/physics/CharacterController.hpp
    engine/gameplay/FirstPerson.hpp
    engine/gameplay/Random.hpp
    engine/gameplay/DefinitionTable.hpp
    engine/assets/Assets.hpp
    engine/assets/MeshBuilder.hpp
    engine/ui/GameUi.hpp
    engine/ui/GameMenus.hpp
    engine/input/InputActions.hpp
    engine/input/InputState.hpp
    engine/graphics/DebugDraw.hpp
)

# Compiles each gameplay API header on its own, seeing only src/ and the
# third-party libraries the gameplay API allows (EnTT, glm, nlohmann_json), so
# a header that starts including SDL, Vulkan, Jolt or ImGui fails the build.
function(zenith_add_gameplay_api_check)
    set(check_dir "${CMAKE_CURRENT_BINARY_DIR}/gameplay_api_check")
    set(check_sources "")
    foreach(header IN LISTS ZENITH_GAMEPLAY_API_HEADERS)
        string(MAKE_C_IDENTIFIER "${header}" source_name)
        set(source "${check_dir}/${source_name}.cpp")
        file(GENERATE OUTPUT "${source}" CONTENT "#include \"${header}\"\n")
        list(APPEND check_sources "${source}")
    endforeach()

    add_library(zenith_gameplay_api_check OBJECT ${check_sources})
    target_include_directories(zenith_gameplay_api_check PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(zenith_gameplay_api_check PRIVATE EnTT::EnTT glm::glm nlohmann_json::nlohmann_json)
    zenith_target_warnings(zenith_gameplay_api_check)
endfunction()
