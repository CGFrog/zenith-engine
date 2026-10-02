// A module built for a different module API version, which hosts must refuse
// before calling its registration. It does not link the engine.

#include "engine/module/EngineExport.hpp"

#include <cstdint>

class ModuleRegistrar;

extern "C" ZENITH_MODULE_EXPORT uint32_t zenith_module_api_version() {
    return 999;
}

extern "C" ZENITH_MODULE_EXPORT const char* zenith_module_build_hash() {
    return "future-build";
}

extern "C" ZENITH_MODULE_EXPORT const char* zenith_module_name() {
    return "zenith_test_mismatched";
}

extern "C" ZENITH_MODULE_EXPORT void zenith_module_register(ModuleRegistrar&) {}
