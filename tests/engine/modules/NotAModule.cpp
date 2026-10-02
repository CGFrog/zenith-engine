// A shared library without the game module entry points, which hosts must
// refuse with a missing-entry-point error.

#include "engine/module/EngineExport.hpp"

extern "C" ZENITH_MODULE_EXPORT int zenith_test_unrelated_function() {
    return 42;
}
