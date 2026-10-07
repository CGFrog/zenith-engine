#pragma once

// The exit code a standalone test returns when its environment cannot run it (no Vulkan device, no window).

namespace testsupport {

// CTest reports a test that exits with it as Skipped, not Failed:
// zenith_add_test registers SKIP_RETURN_CODE 77 for every `vulkan` test, and
// strict validation without the layer exits with it too
// (validationSkippedExitCode).
inline constexpr int skipExitCode = 77;

} // namespace testsupport
