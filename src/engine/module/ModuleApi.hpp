#pragma once

#include "EngineExport.hpp"

#include <cstdint>
#include <string>
#include <string_view>

// The binary contract between the engine and a game module: a shared library
// that adds component types and systems to a host (the editor or a game
// executable). A module exports C-linkage entry points so they can be found by
// name; ZENITH_GAME_MODULE defines all of them except the registration
// function, which the module writes:
//
//   ZENITH_GAME_MODULE("my_game", "1.2.0");
//
//   void zenith_module_register(ModuleRegistrar& registrar) {
//       registrar.addComponentType(ComponentTypeBuilder<Spinner>("spinner", "Spinner").build());
//       registrar.addSystem({.name = "spin", .run = spin});
//   }
//
// The registration function must not throw: it is called through a C-linkage
// pointer. Everything a module registers holds code from the module, so the
// host must release it before unloading (see GameModule).

class ModuleRegistrar;

// Bump whenever a change to the engine would break modules built against the
// previous engine headers: ModuleRegistrar, the registered descriptor types,
// or the layout of anything a module reads or writes through them. A macro so
// the build hash below can spell it.
#define ZENITH_MODULE_API_VERSION 7
inline constexpr uint32_t moduleApiVersion = ZENITH_MODULE_API_VERSION;

#define ZENITH_MODULE_STRINGIFY_VALUE(value) #value
#define ZENITH_MODULE_STRINGIFY(value) ZENITH_MODULE_STRINGIFY_VALUE(value)

#if defined(_MSC_VER) && !defined(__clang__)
#define ZENITH_MODULE_COMPILER_ID "msvc" ZENITH_MODULE_STRINGIFY(_MSC_VER)
#elif defined(__clang__)
#define ZENITH_MODULE_COMPILER_ID "clang" ZENITH_MODULE_STRINGIFY(__clang_major__)
#elif defined(__GNUC__)
#define ZENITH_MODULE_COMPILER_ID "gcc" ZENITH_MODULE_STRINGIFY(__GNUC__)
#else
#define ZENITH_MODULE_COMPILER_ID "unknown-compiler"
#endif

// Standard library settings that change the layout of std types.
#if defined(_ITERATOR_DEBUG_LEVEL)
#define ZENITH_MODULE_LIBRARY_ID "idl" ZENITH_MODULE_STRINGIFY(_ITERATOR_DEBUG_LEVEL)
#elif defined(_GLIBCXX_USE_CXX11_ABI) && defined(_GLIBCXX_DEBUG)
#define ZENITH_MODULE_LIBRARY_ID "glibcxx" ZENITH_MODULE_STRINGIFY(_GLIBCXX_USE_CXX11_ABI) "-debug"
#elif defined(_GLIBCXX_USE_CXX11_ABI)
#define ZENITH_MODULE_LIBRARY_ID "glibcxx" ZENITH_MODULE_STRINGIFY(_GLIBCXX_USE_CXX11_ABI)
#elif defined(_LIBCPP_VERSION)
#define ZENITH_MODULE_LIBRARY_ID "libcxx" ZENITH_MODULE_STRINGIFY(_LIBCPP_ABI_VERSION)
#else
#define ZENITH_MODULE_LIBRARY_ID "unknown-library"
#endif

// The C runtime heap must be shared: a module allocates strings and functions
// the engine later frees.
#if defined(_MSC_VER) && !defined(_DLL)
#define ZENITH_MODULE_RUNTIME_LINKAGE "static"
#else
#define ZENITH_MODULE_RUNTIME_LINKAGE "shared"
#endif
#if defined(_DEBUG)
#define ZENITH_MODULE_RUNTIME_ID "debug-" ZENITH_MODULE_RUNTIME_LINKAGE "-runtime"
#else
#define ZENITH_MODULE_RUNTIME_ID "release-" ZENITH_MODULE_RUNTIME_LINKAGE "-runtime"
#endif

#if defined(_WIN64) || defined(__LP64__) || defined(_LP64)
#define ZENITH_MODULE_POINTER_BITS 64
#else
#define ZENITH_MODULE_POINTER_BITS 32
#endif

// Identifies the module API and the toolchain and runtime ABI a binary was
// compiled with. A module is refused unless its build hash equals the
// engine's, because engine types cross the boundary by reference and must
// have identical layouts on both sides.
#define ZENITH_MODULE_BUILD_HASH                                                                                       \
    "api" ZENITH_MODULE_STRINGIFY(ZENITH_MODULE_API_VERSION) "-" ZENITH_MODULE_COMPILER_ID                             \
                                                             "-" ZENITH_MODULE_LIBRARY_ID "-" ZENITH_MODULE_RUNTIME_ID \
                                                             "-" ZENITH_MODULE_STRINGIFY(ZENITH_MODULE_POINTER_BITS)

// Entry point names a host resolves in a module.
inline constexpr const char* moduleApiVersionSymbol = "zenith_module_api_version";
inline constexpr const char* moduleBuildHashSymbol = "zenith_module_build_hash";
inline constexpr const char* moduleNameSymbol = "zenith_module_name";
inline constexpr const char* moduleVersionSymbol = "zenith_module_version";
inline constexpr const char* moduleRegisterSymbol = "zenith_module_register";

extern "C" {
using ZenithModuleApiVersionFunction = uint32_t (*)();
using ZenithModuleStringFunction = const char* (*)();
using ZenithModuleRegisterFunction = void (*)(ModuleRegistrar&);
}

// Defines a module's identification entry points and declares
// zenith_module_register, which the module then defines. Use it once, at
// global scope, in one source file of the module.
#define ZENITH_GAME_MODULE(name, version)                                                                              \
    extern "C" ZENITH_MODULE_EXPORT uint32_t zenith_module_api_version() {                                             \
        return moduleApiVersion;                                                                                       \
    }                                                                                                                  \
    extern "C" ZENITH_MODULE_EXPORT const char* zenith_module_build_hash() {                                           \
        return ZENITH_MODULE_BUILD_HASH;                                                                               \
    }                                                                                                                  \
    extern "C" ZENITH_MODULE_EXPORT const char* zenith_module_name() {                                                 \
        return name;                                                                                                   \
    }                                                                                                                  \
    extern "C" ZENITH_MODULE_EXPORT const char* zenith_module_version() {                                              \
        return version;                                                                                                \
    }                                                                                                                  \
    extern "C" ZENITH_MODULE_EXPORT void zenith_module_register(ModuleRegistrar& registrar)

// The engine's own build hash, compiled into the engine library.
std::string_view engineModuleBuildHash();

struct ModuleCompatibilityResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// Whether a module reporting apiVersion and buildHash may be loaded by this
// engine. moduleName only labels the error.
ModuleCompatibilityResult checkModuleCompatibility(std::string_view moduleName, uint32_t apiVersion,
                                                   std::string_view buildHash);
