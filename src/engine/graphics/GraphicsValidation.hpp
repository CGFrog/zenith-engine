#pragma once

// Validation-layer configuration requested by the composition root or the environment.

#include <cstdint>

// Validation-layer configuration requested by the composition root or the
// environment. Strict mode turns validation into a pass/fail gate for bounded
// smoke runs; see docs/RENDERER_VALIDATION.md.
struct GraphicsValidationOptions {
    // Requires VK_LAYER_KHRONOS_validation, enables synchronization validation,
    // and makes any counted message fail the process (validationExitCode).
    bool strict{false};
};

// Reads ZENITH_VALIDATION_STRICT; any value other than unset, empty, or "0"
// requests strict validation.
GraphicsValidationOptions graphicsValidationOptionsFromEnvironment();

// Message counts reported by the Vulkan validation layer over one renderer
// lifetime, including instance and device teardown.
struct GraphicsValidationReport {
    bool strict{false};
    bool layerActive{false};
    bool synchronizationValidation{false};
    // Counted toward strict failure: every error, and warnings from the
    // validation or general message types.
    uint32_t errorCount{0};
    uint32_t warningCount{0};
    // Reported but never fatal: performance hints depend on the driver and are
    // advisory rather than API-usage violations.
    uint32_t performanceWarningCount{0};
    // Reported but never fatal: Vulkan loader diagnostics describe the
    // machine's layer registry (stale or third-party overlay manifests), not
    // the application's API usage.
    uint32_t loaderMessageCount{0};

    uint32_t failingMessageCount() const {
        return errorCount + warningCount;
    }
};

// Exit code CTest treats as "skipped" (SKIP_RETURN_CODE) when strict
// validation is requested but the layer is not installed.
inline constexpr int validationSkippedExitCode = 77;

// Applies the strict-validation policy to an application exit code and prints a
// one-line summary to stderr when validation ran. Without strict mode the
// application exit code is returned unchanged. In strict mode an absent layer
// yields validationSkippedExitCode and any failing message yields a failure.
int validationExitCode(const GraphicsValidationReport& report, int applicationExitCode);
