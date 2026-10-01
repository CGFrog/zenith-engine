#include "engine/assets/AssetStreaming.hpp"

#include <algorithm>
#include <cmath>
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

StreamAssetRequest texture(std::string key, uint64_t deadline, float visibility, float distance,
                           float importance = 0.0f) {
    return {.key = std::move(key),
            .cpuBytes = 400,
            .priority = {.projectedVisibility = visibility,
                         .distance = distance,
                         .gameplayImportance = importance,
                         .deadlineFrame = deadline},
            .segments = {{.kind = StreamSegmentKind::TextureMipRange,
                          .first = 2,
                          .count = 3,
                          .decodedBytes = 100,
                          .uploadBytes = 100,
                          .required = true},
                         {.kind = StreamSegmentKind::TextureMipRange,
                          .first = 0,
                          .count = 2,
                          .decodedBytes = 300,
                          .uploadBytes = 300,
                          .required = false}}};
}

void finishFrame(DerivedAssetResidencyManager& residency) {
    for (const StreamWorkItem& decode : residency.takeDecodeWork()) {
        residency.completeDecode(decode.asset, decode.bytes);
    }
    for (const StreamWorkItem& upload : residency.takeUploadWork()) {
        residency.completeUpload(upload.asset, upload.segment, upload.bytes);
    }
}

double percentile(std::vector<double> samples, double fraction) {
    std::ranges::sort(samples);
    return samples[static_cast<size_t>(std::ceil(fraction * samples.size())) - 1];
}
} // namespace

int main() {
    const StreamingBudgets bounded{.decodeBytesPerFrame = 500,
                                   .uploadBytesPerFrame = 300,
                                   .decodeJobsPerFrame = 1,
                                   .uploadJobsPerFrame = 1,
                                   .cpuResidentBytes = 500,
                                   .gpuResidentBytes = 350,
                                   .unusedFrameThreshold = 2};
    DerivedAssetResidencyManager residency(bounded);
    const StreamAssetHandle distant = residency.request(texture("distant", 100, 0.1f, 100.0f));
    const StreamAssetHandle important = residency.request(texture("important", 20, 0.8f, 10.0f, 1.0f));
    residency.beginFrame(1);
    const auto firstDecode = residency.takeDecodeWork();
    expect(firstDecode.size() == 1 && firstDecode.front().asset == important && firstDecode.front().bytes == 400,
           "decode scheduling combines deadline, projected visibility, distance, and gameplay importance");
    expect(residency.stats().decodeBacklogBytes == 400 && residency.stats().decodeJobs == 1,
           "decode byte and job budgets defer lower-priority work without exceeding the frame budget");
    expect(residency.completeDecode(important, 400), "the selected generation completes CPU residency");
    const auto firstUpload = residency.takeUploadWork();
    expect(firstUpload.size() == 1 && firstUpload.front().bytes == 100,
           "required coarse texture mips upload before optional high-detail mips");
    expect(residency.completeUpload(firstUpload.front().asset, firstUpload.front().segment, firstUpload.front().bytes),
           "an independently uploaded mip range becomes resident");
    expect(residency.snapshot(important)->gpuState == ResidencyState::UploadPending &&
               residency.snapshot(important)->residentSegments == 1,
           "partial texture residency remains explicit until all ranges arrive");

    residency.beginFrame(2);
    const auto secondUpload = residency.takeUploadWork();
    expect(secondUpload.size() == 1 && secondUpload.front().bytes == 300,
           "the optional mip range consumes a later bounded upload frame");
    residency.completeUpload(secondUpload.front().asset, secondUpload.front().segment, secondUpload.front().bytes);
    expect(residency.snapshot(important)->gpuState == ResidencyState::Resident,
           "all texture ranges transition the GPU generation to resident");

    StreamAssetRequest meshRequest{
        .key = "chunked-mesh",
        .cpuBytes = 200,
        .priority = {.projectedVisibility = 1.0f, .distance = 2.0f, .gameplayImportance = 0.5f, .deadlineFrame = 3},
        .segments = {{.kind = StreamSegmentKind::MeshChunk,
                      .first = 0,
                      .count = 1,
                      .decodedBytes = 100,
                      .uploadBytes = 100,
                      .required = true},
                     {.kind = StreamSegmentKind::MeshChunk,
                      .first = 1,
                      .count = 1,
                      .decodedBytes = 100,
                      .uploadBytes = 100,
                      .required = false}}};
    const StreamAssetHandle chunkedMesh = residency.request(std::move(meshRequest));
    residency.beginFrame(3);
    finishFrame(residency);
    expect(residency.snapshot(chunkedMesh)->residentSegments == 1,
           "large meshes expose independently resident chunks under the upload-job budget");

    const StreamAssetHandle cancelled = residency.request(texture("cancelled", 3, 1.0f, 1.0f));
    residency.beginFrame(4);
    const auto cancelledWork = residency.takeDecodeWork();
    expect(cancelledWork.size() == 1 && cancelledWork.front().asset == cancelled,
           "the imminent request enters decoding");
    expect(residency.cancel(cancelled), "in-flight generations can be cancelled");
    expect(!residency.completeDecode(cancelledWork.front().asset, cancelledWork.front().bytes) &&
               residency.stats().cancelledCompletions == 1,
           "a late decode completion cannot resurrect an unloaded generation");

    DerivedAssetResidencyManager impossible({.decodeBytesPerFrame = 500,
                                             .uploadBytesPerFrame = 500,
                                             .decodeJobsPerFrame = 1,
                                             .uploadJobsPerFrame = 1,
                                             .cpuResidentBytes = 10,
                                             .gpuResidentBytes = 10,
                                             .unusedFrameThreshold = 100});
    impossible.request(texture("pinned", 0, 1.0f, 0.0f));
    impossible.beginFrame(1);
    finishFrame(impossible);
    expect(!impossible.enforceMemoryBudgets(1) && impossible.stats().hardBudgetFailures == 1,
           "memory pressure reports an explicit hard-budget failure after ordered recovery is exhausted");

    residency.beginFrame(4);
    finishFrame(residency);
    residency.beginFrame(8);
    finishFrame(residency);
    residency.setTemporaryCacheBytes(250);
    expect(residency.enforceMemoryBudgets(12),
           "memory pressure is satisfiable after temporary caches, distant mips, and unused assets are considered");
    expect(residency.stats().temporaryCacheBytesDiscarded == 250 && residency.stats().distantMipBytesDropped > 0,
           "memory pressure follows the documented cache-then-mip response order");
    residency.retire(11);
    expect(residency.snapshot(distant).has_value(), "GPU generations remain alive until their retirement serial");
    residency.retire(12);
    expect(!residency.snapshot(distant).has_value() && residency.stats().evictedAssets > 0,
           "unused assets retire only after the completion serial");

    const StreamingBudgets replayBudgets{.decodeBytesPerFrame = 800,
                                         .uploadBytesPerFrame = 400,
                                         .decodeJobsPerFrame = 2,
                                         .uploadJobsPerFrame = 2,
                                         .cpuResidentBytes = 10'000,
                                         .gpuResidentBytes = 10'000};
    DerivedAssetResidencyManager cold(replayBudgets);
    std::vector<StreamAssetHandle> pathAssets;
    for (uint32_t zone = 0; zone < 6; ++zone) {
        pathAssets.push_back(cold.request(texture("zone-" + std::to_string(zone), zone * 4 + 2, 0.0f, 100.0f)));
    }
    std::vector<double> coldFrames;
    for (uint64_t frame = 0; frame < 30; ++frame) {
        cold.beginFrame(frame);
        const uint32_t zone = std::min<uint32_t>(static_cast<uint32_t>(frame / 5), 5);
        for (uint32_t index = 0; index < pathAssets.size(); ++index) {
            const float distance = static_cast<float>(std::abs(static_cast<int>(index) - static_cast<int>(zone)) * 20);
            cold.touch(pathAssets[index], {.projectedVisibility = index == zone ? 1.0f : 0.1f,
                                           .distance = distance,
                                           .gameplayImportance = index == zone ? 1.0f : 0.0f,
                                           .deadlineFrame = frame + (index == zone ? 0 : 20)});
            if (index == zone) {
                cold.noteResidencyMiss(pathAssets[index]);
            }
        }
        finishFrame(cold);
        coldFrames.push_back(5.0 + static_cast<double>(cold.stats().decodedBytes) / 200.0 +
                             static_cast<double>(cold.stats().uploadedBytes) / 200.0);
    }
    const uint64_t coldDecodedBytes = cold.stats().peakCpuResidentBytes;
    const uint64_t coldUploadedBytes = cold.stats().peakGpuResidentBytes;
    const uint32_t coldMisses = cold.stats().residencyMisses;

    std::vector<double> warmFrames;
    for (uint64_t frame = 30; frame < 60; ++frame) {
        cold.beginFrame(frame);
        finishFrame(cold);
        warmFrames.push_back(5.0 + static_cast<double>(cold.stats().decodedBytes) / 200.0 +
                             static_cast<double>(cold.stats().uploadedBytes) / 200.0);
    }
    const uint32_t coldOverBudget = static_cast<uint32_t>(
        std::ranges::count_if(coldFrames, [](double milliseconds) { return milliseconds > 7.0; }));
    const uint32_t warmOverBudget = static_cast<uint32_t>(
        std::ranges::count_if(warmFrames, [](double milliseconds) { return milliseconds > 7.0; }));
    expect(percentile(coldFrames, 0.99) <= 12.0 && *std::ranges::max_element(coldFrames) <= 12.0 && coldOverBudget > 0,
           "cold traversal reports bounded p99/max frame cost and frames over budget");
    expect(percentile(warmFrames, 0.99) == 5.0 && *std::ranges::max_element(warmFrames) == 5.0 && warmOverBudget == 0,
           "the deterministic warm-cache replay performs no decode or upload work");
    expect(coldDecodedBytes == 2'400 && coldUploadedBytes == 2'400 && coldMisses > 0 &&
               cold.stats().decodeBacklogBytes == 0 && cold.stats().uploadBacklogBytes == 0,
           "replay telemetry accounts for decoded/uploaded peaks, misses, and drained backlog");

    return failures == 0 ? 0 : 1;
}
