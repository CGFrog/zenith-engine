#include "engine/assets/AssetStreaming.hpp"
#include "engine/assets/TextureMipStreaming.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
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

// Bytes of a 4:1 block-compressed square chain: each level a quarter of the
// one above, never below one 16-byte block.
std::vector<uint64_t> squareChainBytes(uint32_t levels, uint64_t baseBytes) {
    std::vector<uint64_t> bytes;
    for (uint32_t level = 0; level < levels; ++level) {
        bytes.push_back(std::max<uint64_t>(baseBytes >> (2 * level), 16));
    }
    return bytes;
}

void testTextureMipTail() {
    expect(textureMipTailFirstLevel(1024, 1024, 11, 64) == 4, "a 1024 texture starts with its 64-texel tail");
    expect(textureMipTailFirstLevel(1024, 256, 11, 64) == 4, "the tail starts where the larger dimension fits");
    expect(textureMipTailFirstLevel(32, 32, 6, 64) == 0, "a texture inside the tail is fully resident at once");
    expect(textureMipTailFirstLevel(4096, 4096, 3, 64) == 2, "a truncated chain still starts with its smallest level");
    expect(textureMipTailFirstLevel(16, 16, 0, 64) == 0, "an empty chain has no tail");
}

void testTextureStreamPriority() {
    const StreamPriority near = textureStreamPriority(2.0f, 1.0f);
    const StreamPriority far = textureStreamPriority(20.0f, 1.0f);
    const StreamPriority inside = textureStreamPriority(0.5f, 1.0f);
    expect(near.projectedVisibility > far.projectedVisibility && near.distance < far.distance,
           "nearer bounds project larger and closer");
    expect(inside.projectedVisibility == 1.0f && inside.distance == 0.0f,
           "a camera inside the bounds sees them at full size");
    const StreamPriority merged = mostDemandingStreamPriority(far, near);
    expect(merged.projectedVisibility == near.projectedVisibility && merged.distance == near.distance,
           "a texture used twice keeps its most demanding use");
    expect(mostDemandingStreamPriority(StreamPriority{}, far).projectedVisibility == far.projectedVisibility,
           "an unused texture's default priority never outranks a use");
}

void testTextureMipPlanning() {
    const std::vector<uint64_t> large = squareChainBytes(11, 1'048'576);
    const std::vector<uint64_t> small = squareChainBytes(9, 65'536);
    // Both start from their 64-texel tails (levels 4 and 2).
    const std::vector<TextureMipStreamCandidate> candidates{
        {.key = 1,
         .residency = {.mipCount = 11, .residentFirstMip = 4, .requestedFirstMip = 0},
         .levelBytes = large,
         .priority = textureStreamPriority(30.0f, 1.0f)},
        {.key = 2,
         .residency = {.mipCount = 9, .residentFirstMip = 2, .requestedFirstMip = 0},
         .levelBytes = small,
         .priority = textureStreamPriority(3.0f, 1.0f)}};

    const std::vector<TextureMipUpload> first = planTextureMipUploads(candidates, 40'000);
    expect(first.size() == 2 && first[0].key == 2 && first[1].key == 1,
           "the more visible texture's uploads come first");
    expect(first.size() == 2 && first[0].firstMip + first[0].levelCount == 2 &&
               first[1].firstMip + first[1].levelCount == 4,
           "uploaded ranges end where the resident levels begin");
    uint64_t firstBytes = 0;
    for (const TextureMipUpload& upload : first) {
        firstBytes += upload.bytes;
    }
    expect(firstBytes <= 40'000, "a frame's uploads stay within the byte budget");
    expect(first.size() == 2 && first[1].levelCount == 1 && first[1].bytes == large[3],
           "each texture sharpens one level per round before any takes a second");

    const std::vector<TextureMipUpload> oversized = planTextureMipUploads(candidates, 1'000);
    expect(oversized.size() == 1 && oversized[0].key == 2 && oversized[0].levelCount == 1 &&
               oversized[0].bytes == small[1],
           "a level larger than the budget still streams, alone, so streaming always progresses");

    const std::vector<TextureMipUpload> unbounded =
        planTextureMipUploads(candidates, std::numeric_limits<uint64_t>::max());
    expect(unbounded.size() == 2 && unbounded[0].firstMip == 0 && unbounded[0].levelCount == 2 &&
               unbounded[1].firstMip == 0 && unbounded[1].levelCount == 4,
           "an unbounded budget completes every texture in one frame");

    const std::vector<TextureMipStreamCandidate> complete{
        {.key = 3, .residency = {.mipCount = 4, .residentFirstMip = 0}, .levelBytes = small}};
    expect(planTextureMipUploads(complete, 1'000'000).empty(), "complete textures need no uploads");

    // Draining the plan converges to the requested levels within a bounded
    // number of frames and never exceeds the budget except for a lone level.
    std::vector<TextureMipStreamCandidate> streaming = candidates;
    uint32_t frames = 0;
    for (; frames < 64; ++frames) {
        const std::vector<TextureMipUpload> uploads = planTextureMipUploads(streaming, 300'000);
        if (uploads.empty()) {
            break;
        }
        uint64_t frameBytes = 0;
        for (const TextureMipUpload& upload : uploads) {
            frameBytes += upload.bytes;
            for (TextureMipStreamCandidate& candidate : streaming) {
                if (candidate.key == upload.key) {
                    candidate.residency.residentFirstMip = upload.firstMip;
                }
            }
        }
        expect(frameBytes <= 300'000 || uploads.size() == 1, "streaming frames respect the budget");
    }
    expect(streaming[0].residency.complete() && streaming[1].residency.complete() && frames <= 6,
           "streaming reaches full residency in a few frames");
}
} // namespace

int main() {
    testTextureMipTail();
    testTextureStreamPriority();
    testTextureMipPlanning();
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
