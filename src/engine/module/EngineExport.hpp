#pragma once

// zenith_engine is a shared library so that an executable and every game
// module it loads share one copy of the engine and of its third-party state
// (SDL, Dear ImGui, Jolt, volk). On Windows the build exports every function
// the engine defines out of line (zenith_export_defined_symbols), but data is only
// reachable from another binary through __declspec(dllimport):
//
//   ZENITH_ENGINE_API extern int exportedEngineValue;
//
// Prefer functions to exported data; use this only when data must cross the
// boundary. ZENITH_ENGINE_BUILDING is defined only while compiling the engine.
#if defined(_WIN32)
#if defined(ZENITH_ENGINE_BUILDING)
#define ZENITH_ENGINE_API __declspec(dllexport)
#else
#define ZENITH_ENGINE_API __declspec(dllimport)
#endif
#else
#define ZENITH_ENGINE_API __attribute__((visibility("default")))
#endif

// Marks a game module's entry points (see engine/module/ModuleApi.hpp) for
// export from the module's shared library.
#if defined(_WIN32)
#define ZENITH_MODULE_EXPORT __declspec(dllexport)
#else
#define ZENITH_MODULE_EXPORT __attribute__((visibility("default")))
#endif
