#include "engine/graphics/RenderSubmission.hpp"

#include <iostream>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}
} // namespace

int main() {
    std::vector<RenderBatch> batches(2000, {.indexCount = 36, .instanceCount = 1});
    RenderCapabilities unsupported;
    expect(buildIndirectPrototype(batches, unsupported).empty(),
           "the indirect prototype must remain behind all capability checks");
    expect(chooseRenderSubmissionPath(unsupported, {1.0, 0.1}) == RenderSubmissionPath::CompatibleInstanced,
           "unsupported devices retain the compatible path");

    RenderCapabilities supported{.descriptorIndexing = true, .multiDrawIndirect = true, .indirectDrawCount = true};
    expect(buildIndirectPrototype(batches, supported).size() == 2000,
           "the capability-gated prototype should encode the 2,000-object workload");
    expect(chooseRenderSubmissionPath(supported, {1.0, 0.95}) == RenderSubmissionPath::CompatibleInstanced,
           "a non-material benchmark improvement must not select the prototype");
    expect(chooseRenderSubmissionPath(supported, {1.0, 0.80}) == RenderSubmissionPath::IndirectPrototype,
           "a measured material improvement may select the prototype");
    return failures == 0 ? 0 : 1;
}
