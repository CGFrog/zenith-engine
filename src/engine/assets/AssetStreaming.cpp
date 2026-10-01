#include "AssetStreaming.hpp"

#include <algorithm>
#include <cmath>
#include <ranges>

struct DerivedAssetResidencyManager::Segment {
    StreamSegmentDescription description;
    bool uploadQueued{false};
    bool resident{false};
};

struct DerivedAssetResidencyManager::Asset {
    uint32_t generation{1};
    bool occupied{false};
    std::string key;
    uint64_t requestedCpuBytes{0};
    StreamPriority priority;
    ResidencyState cpuState{ResidencyState::Requested};
    ResidencyState gpuState{ResidencyState::Requested};
    std::vector<Segment> segments;
    uint64_t decodedBytes{0};
    uint64_t gpuBytes{0};
    uint64_t lastUsedFrame{0};
    uint64_t retireAfterSerial{0};
};

DerivedAssetResidencyManager::DerivedAssetResidencyManager(StreamingBudgets budgets)
    : m_budgets(budgets) {}

DerivedAssetResidencyManager::~DerivedAssetResidencyManager() = default;

StreamAssetHandle DerivedAssetResidencyManager::request(StreamAssetRequest request) {
    if (request.key.empty() || request.segments.empty()) {
        return {};
    }
    if (const auto found = std::ranges::find_if(m_assets,
                                                [&](const Asset& asset) {
                                                    return asset.occupied && asset.key == request.key &&
                                                           asset.cpuState != ResidencyState::Retired;
                                                });
        found != m_assets.end()) {
        found->priority = request.priority;
        found->lastUsedFrame = m_stats.frame;
        return {static_cast<uint32_t>(std::distance(m_assets.begin(), found)), found->generation};
    }
    auto slot = std::ranges::find_if(m_assets, [](const Asset& asset) { return !asset.occupied; });
    if (slot == m_assets.end()) {
        m_assets.emplace_back();
        slot = std::prev(m_assets.end());
    }
    const uint32_t generation = std::max(slot->generation, 1u);
    *slot = {.generation = generation,
             .occupied = true,
             .key = std::move(request.key),
             .requestedCpuBytes = request.cpuBytes,
             .priority = request.priority,
             .lastUsedFrame = m_stats.frame};
    slot->segments.reserve(request.segments.size());
    for (StreamSegmentDescription& segment : request.segments) {
        slot->segments.push_back({std::move(segment)});
    }
    refreshBacklog();
    return {static_cast<uint32_t>(std::distance(m_assets.begin(), slot)), generation};
}

bool DerivedAssetResidencyManager::updatePriority(StreamAssetHandle asset, StreamPriority priority) {
    Asset* found = find(asset);
    if (!found) {
        return false;
    }
    found->priority = priority;
    return true;
}

bool DerivedAssetResidencyManager::cancel(StreamAssetHandle asset, uint64_t retireAfterSerial) {
    Asset* found = find(asset);
    if (!found) {
        return false;
    }
    if (found->gpuBytes == 0) {
        m_stats.cpuResidentBytes -= found->decodedBytes;
        found->decodedBytes = 0;
        found->cpuState = ResidencyState::Retired;
        found->gpuState = ResidencyState::Retired;
        found->occupied = false;
        ++found->generation;
    } else {
        markForEviction(*found, retireAfterSerial);
    }
    refreshBacklog();
    return true;
}

void DerivedAssetResidencyManager::touch(StreamAssetHandle asset, StreamPriority priority) {
    if (Asset* found = find(asset)) {
        found->priority = priority;
        found->lastUsedFrame = m_stats.frame;
    }
}

void DerivedAssetResidencyManager::beginFrame(uint64_t frame) {
    m_stats.frame = frame;
    m_stats.decodedBytes = 0;
    m_stats.uploadedBytes = 0;
    m_stats.decodeJobs = 0;
    m_stats.uploadJobs = 0;
    m_decodeBytesRemaining = m_budgets.decodeBytesPerFrame;
    m_uploadBytesRemaining = m_budgets.uploadBytesPerFrame;
    m_decodeJobsRemaining = m_budgets.decodeJobsPerFrame;
    m_uploadJobsRemaining = m_budgets.uploadJobsPerFrame;
    refreshBacklog();
}

std::vector<StreamWorkItem> DerivedAssetResidencyManager::takeDecodeWork() {
    std::vector<uint32_t> candidates;
    for (uint32_t index = 0; index < m_assets.size(); ++index) {
        if (m_assets[index].occupied && m_assets[index].cpuState == ResidencyState::Requested) {
            candidates.push_back(index);
        }
    }
    std::ranges::stable_sort(candidates, [&](uint32_t left, uint32_t right) {
        const double leftScore = priorityScore(m_assets[left]);
        const double rightScore = priorityScore(m_assets[right]);
        return leftScore == rightScore ? left < right : leftScore > rightScore;
    });
    std::vector<StreamWorkItem> work;
    for (const uint32_t index : candidates) {
        Asset& asset = m_assets[index];
        if (m_decodeJobsRemaining == 0 || asset.requestedCpuBytes > m_decodeBytesRemaining) {
            continue;
        }
        asset.cpuState = ResidencyState::Decoding;
        m_decodeBytesRemaining -= asset.requestedCpuBytes;
        --m_decodeJobsRemaining;
        ++m_stats.decodeJobs;
        work.push_back({{index, asset.generation}, std::numeric_limits<uint32_t>::max(), asset.requestedCpuBytes});
    }
    refreshBacklog();
    return work;
}

bool DerivedAssetResidencyManager::completeDecode(StreamAssetHandle asset, uint64_t decodedBytes) {
    Asset* found = find(asset);
    if (!found || found->cpuState != ResidencyState::Decoding) {
        ++m_stats.cancelledCompletions;
        return false;
    }
    if (decodedBytes > found->requestedCpuBytes) {
        found->cpuState = ResidencyState::Requested;
        ++m_stats.cancelledCompletions;
        refreshBacklog();
        return false;
    }
    found->decodedBytes = decodedBytes;
    found->cpuState = ResidencyState::Resident;
    found->gpuState = ResidencyState::UploadPending;
    m_stats.decodedBytes += decodedBytes;
    m_stats.cpuResidentBytes += decodedBytes;
    m_stats.peakCpuResidentBytes = std::max(m_stats.peakCpuResidentBytes, m_stats.cpuResidentBytes);
    refreshBacklog();
    return true;
}

std::vector<StreamWorkItem> DerivedAssetResidencyManager::takeUploadWork() {
    struct Candidate {
        uint32_t asset{0};
        uint32_t segment{0};
    };

    std::vector<Candidate> candidates;
    for (uint32_t assetIndex = 0; assetIndex < m_assets.size(); ++assetIndex) {
        const Asset& asset = m_assets[assetIndex];
        if (!asset.occupied || asset.gpuState != ResidencyState::UploadPending) {
            continue;
        }
        for (uint32_t segmentIndex = 0; segmentIndex < asset.segments.size(); ++segmentIndex) {
            const Segment& segment = asset.segments[segmentIndex];
            if (!segment.resident && !segment.uploadQueued) {
                candidates.push_back({assetIndex, segmentIndex});
            }
        }
    }
    std::ranges::stable_sort(candidates, [&](const Candidate& left, const Candidate& right) {
        const Segment& leftSegment = m_assets[left.asset].segments[left.segment];
        const Segment& rightSegment = m_assets[right.asset].segments[right.segment];
        if (leftSegment.description.required != rightSegment.description.required) {
            return leftSegment.description.required;
        }
        const double leftScore = priorityScore(m_assets[left.asset]);
        const double rightScore = priorityScore(m_assets[right.asset]);
        if (leftScore != rightScore) {
            return leftScore > rightScore;
        }
        if (left.asset != right.asset) {
            return left.asset < right.asset;
        }
        return left.segment < right.segment;
    });
    std::vector<StreamWorkItem> work;
    for (const Candidate candidate : candidates) {
        Segment& segment = m_assets[candidate.asset].segments[candidate.segment];
        if (m_uploadJobsRemaining == 0 || segment.description.uploadBytes > m_uploadBytesRemaining) {
            continue;
        }
        segment.uploadQueued = true;
        m_uploadBytesRemaining -= segment.description.uploadBytes;
        --m_uploadJobsRemaining;
        ++m_stats.uploadJobs;
        work.push_back({{candidate.asset, m_assets[candidate.asset].generation},
                        candidate.segment,
                        segment.description.uploadBytes});
    }
    refreshBacklog();
    return work;
}

bool DerivedAssetResidencyManager::completeUpload(StreamAssetHandle asset, uint32_t segmentIndex,
                                                  uint64_t uploadedBytes) {
    Asset* found = find(asset);
    if (!found || found->gpuState != ResidencyState::UploadPending || segmentIndex >= found->segments.size() ||
        !found->segments[segmentIndex].uploadQueued) {
        ++m_stats.cancelledCompletions;
        return false;
    }
    Segment& segment = found->segments[segmentIndex];
    if (uploadedBytes > segment.description.uploadBytes) {
        segment.uploadQueued = false;
        ++m_stats.cancelledCompletions;
        refreshBacklog();
        return false;
    }
    segment.uploadQueued = false;
    segment.resident = true;
    found->gpuBytes += uploadedBytes;
    m_stats.uploadedBytes += uploadedBytes;
    m_stats.gpuResidentBytes += uploadedBytes;
    m_stats.peakGpuResidentBytes = std::max(m_stats.peakGpuResidentBytes, m_stats.gpuResidentBytes);
    if (std::ranges::all_of(found->segments, [](const Segment& item) { return item.resident; })) {
        found->gpuState = ResidencyState::Resident;
    }
    refreshBacklog();
    return true;
}

void DerivedAssetResidencyManager::setTemporaryCacheBytes(uint64_t bytes) {
    m_stats.temporaryCacheBytes = bytes;
}

bool DerivedAssetResidencyManager::enforceMemoryBudgets(uint64_t retireAfterSerial) {
    if (m_stats.temporaryCacheBytes > 0 && (m_stats.cpuResidentBytes > m_budgets.cpuResidentBytes ||
                                            m_stats.gpuResidentBytes > m_budgets.gpuResidentBytes)) {
        m_stats.temporaryCacheBytesDiscarded += m_stats.temporaryCacheBytes;
        m_stats.temporaryCacheBytes = 0;
    }

    std::vector<Asset*> distant;
    for (Asset& asset : m_assets) {
        if (asset.occupied && asset.gpuState == ResidencyState::Resident) {
            distant.push_back(&asset);
        }
    }
    std::ranges::stable_sort(distant, [](const Asset* left, const Asset* right) {
        return left->priority.distance > right->priority.distance;
    });
    for (Asset* asset : distant) {
        if (m_stats.gpuResidentBytes <= m_budgets.gpuResidentBytes) {
            break;
        }
        for (auto segment = asset->segments.rbegin(); segment != asset->segments.rend(); ++segment) {
            if (!segment->resident || segment->description.required ||
                segment->description.kind != StreamSegmentKind::TextureMipRange) {
                continue;
            }
            segment->resident = false;
            asset->gpuBytes -= segment->description.uploadBytes;
            m_stats.gpuResidentBytes -= segment->description.uploadBytes;
            m_stats.distantMipBytesDropped += segment->description.uploadBytes;
            asset->gpuState = ResidencyState::UploadPending;
            if (m_stats.gpuResidentBytes <= m_budgets.gpuResidentBytes) {
                break;
            }
        }
    }

    uint64_t projectedCpuBytes = m_stats.cpuResidentBytes;
    uint64_t projectedGpuBytes = m_stats.gpuResidentBytes;
    std::vector<Asset*> unused;
    for (Asset& asset : m_assets) {
        if (asset.occupied && asset.cpuState != ResidencyState::EvictionPending &&
            m_stats.frame > asset.lastUsedFrame + m_budgets.unusedFrameThreshold) {
            unused.push_back(&asset);
        }
    }
    std::ranges::stable_sort(
        unused, [](const Asset* left, const Asset* right) { return left->lastUsedFrame < right->lastUsedFrame; });
    for (Asset* asset : unused) {
        if (projectedCpuBytes <= m_budgets.cpuResidentBytes && projectedGpuBytes <= m_budgets.gpuResidentBytes) {
            break;
        }
        projectedCpuBytes -= asset->decodedBytes;
        projectedGpuBytes -= asset->gpuBytes;
        markForEviction(*asset, retireAfterSerial);
    }

    const bool satisfied =
        projectedCpuBytes <= m_budgets.cpuResidentBytes && projectedGpuBytes <= m_budgets.gpuResidentBytes;
    if (!satisfied) {
        ++m_stats.hardBudgetFailures;
    }
    refreshBacklog();
    return satisfied;
}

void DerivedAssetResidencyManager::retire(uint64_t completedSerial) {
    for (Asset& asset : m_assets) {
        if (!asset.occupied || asset.cpuState != ResidencyState::EvictionPending ||
            asset.retireAfterSerial > completedSerial) {
            continue;
        }
        m_stats.cpuResidentBytes -= asset.decodedBytes;
        m_stats.gpuResidentBytes -= asset.gpuBytes;
        asset.decodedBytes = 0;
        asset.gpuBytes = 0;
        asset.cpuState = ResidencyState::Retired;
        asset.gpuState = ResidencyState::Retired;
        asset.occupied = false;
        ++asset.generation;
        ++m_stats.evictedAssets;
    }
    refreshBacklog();
}

void DerivedAssetResidencyManager::noteResidencyMiss(StreamAssetHandle asset) {
    const Asset* found = find(asset);
    if (found && found->gpuState != ResidencyState::Resident) {
        ++m_stats.residencyMisses;
    }
}

std::optional<AssetResidencySnapshot> DerivedAssetResidencyManager::snapshot(StreamAssetHandle asset) const {
    const Asset* found = find(asset);
    if (!found) {
        return std::nullopt;
    }
    const uint32_t residentSegments = static_cast<uint32_t>(
        std::ranges::count_if(found->segments, [](const Segment& segment) { return segment.resident; }));
    return AssetResidencySnapshot{.handle = asset,
                                  .key = found->key,
                                  .cpuState = found->cpuState,
                                  .gpuState = found->gpuState,
                                  .residentSegments = residentSegments,
                                  .totalSegments = static_cast<uint32_t>(found->segments.size()),
                                  .cpuBytes = found->decodedBytes,
                                  .gpuBytes = found->gpuBytes,
                                  .lastUsedFrame = found->lastUsedFrame};
}

std::vector<AssetResidencySnapshot> DerivedAssetResidencyManager::snapshots() const {
    std::vector<AssetResidencySnapshot> result;
    for (uint32_t index = 0; index < m_assets.size(); ++index) {
        const Asset& asset = m_assets[index];
        if (asset.occupied) {
            result.push_back(*snapshot({index, asset.generation}));
        }
    }
    return result;
}

const StreamingStats& DerivedAssetResidencyManager::stats() const {
    return m_stats;
}

const StreamingBudgets& DerivedAssetResidencyManager::budgets() const {
    return m_budgets;
}

DerivedAssetResidencyManager::Asset* DerivedAssetResidencyManager::find(StreamAssetHandle asset) {
    if (!asset || asset.index >= m_assets.size()) {
        return nullptr;
    }
    Asset& found = m_assets[asset.index];
    return found.occupied && found.generation == asset.generation ? &found : nullptr;
}

const DerivedAssetResidencyManager::Asset* DerivedAssetResidencyManager::find(StreamAssetHandle asset) const {
    if (!asset || asset.index >= m_assets.size()) {
        return nullptr;
    }
    const Asset& found = m_assets[asset.index];
    return found.occupied && found.generation == asset.generation ? &found : nullptr;
}

double DerivedAssetResidencyManager::priorityScore(const Asset& asset) const {
    const double deadline = asset.priority.deadlineFrame <= m_stats.frame
                                ? 1'000'000.0
                                : 10'000.0 / static_cast<double>(asset.priority.deadlineFrame - m_stats.frame + 1);
    const double distance = std::isfinite(asset.priority.distance) ? 1'000.0 / (1.0 + asset.priority.distance) : 0.0;
    return deadline + static_cast<double>(asset.priority.gameplayImportance) * 100'000.0 +
           static_cast<double>(asset.priority.projectedVisibility) * 10'000.0 + distance;
}

void DerivedAssetResidencyManager::refreshBacklog() {
    m_stats.decodeBacklogBytes = 0;
    m_stats.uploadBacklogBytes = 0;
    for (const Asset& asset : m_assets) {
        if (!asset.occupied) {
            continue;
        }
        if (asset.cpuState == ResidencyState::Requested) {
            m_stats.decodeBacklogBytes += asset.requestedCpuBytes;
        }
        if (asset.gpuState == ResidencyState::UploadPending) {
            for (const Segment& segment : asset.segments) {
                if (!segment.resident) {
                    m_stats.uploadBacklogBytes += segment.description.uploadBytes;
                }
            }
        }
    }
}

void DerivedAssetResidencyManager::markForEviction(Asset& asset, uint64_t retireAfterSerial) {
    asset.cpuState = ResidencyState::EvictionPending;
    asset.gpuState = ResidencyState::EvictionPending;
    asset.retireAfterSerial = retireAfterSerial;
}
