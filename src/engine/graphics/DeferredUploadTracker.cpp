#include "engine/graphics/DeferredUploadTracker.hpp"

std::string describeDeferredUpload(const DeferredUploadWarning& warning) {
    const bool mesh = warning.kind == DeferredUploadKind::Mesh;
    const uint32_t index = static_cast<uint32_t>(warning.assetKey & 0xffffffffull);
    const uint32_t generation = static_cast<uint32_t>(warning.assetKey >> 32);
    return std::string(mesh ? "Mesh" : "Texture") + " upload (asset index " + std::to_string(index) + ", generation " +
           std::to_string(generation) + ") has been deferred for " + std::to_string(warning.deferredFrames) +
           " consecutive frames; " + (mesh ? "its draws are skipped" : "materials sample a fallback texture") +
           " until the upload succeeds. Check staging capacity, geometry arena and bindless table limits.";
}

DeferredUploadTracker::DeferredUploadTracker(Configuration configuration)
    : m_configuration(configuration) {}

void DeferredUploadTracker::markDeferred(DeferredUploadKind kind, uint64_t assetKey) {
    entries(kind)[assetKey].deferredThisFrame = true;
}

void DeferredUploadTracker::markUploaded(DeferredUploadKind kind, uint64_t assetKey) {
    entries(kind).erase(assetKey);
}

void DeferredUploadTracker::release(DeferredUploadKind kind, uint64_t assetKey) {
    entries(kind).erase(assetKey);
}

DeferredUploadFrameSummary DeferredUploadTracker::endFrame() {
    m_overdue.clear();
    DeferredUploadFrameSummary summary;
    for (const DeferredUploadKind kind : {DeferredUploadKind::Mesh, DeferredUploadKind::Texture}) {
        uint32_t deferred = 0;
        for (auto entry = entries(kind).begin(); entry != entries(kind).end();) {
            Entry& state = entry->second;
            if (!state.deferredThisFrame) {
                entry = entries(kind).erase(entry);
                continue;
            }
            state.deferredThisFrame = false;
            ++state.deferredFrames;
            ++deferred;
            if (!state.warned && m_configuration.warningFrames > 0 &&
                state.deferredFrames > m_configuration.warningFrames) {
                state.warned = true;
                m_overdue.push_back({.kind = kind, .assetKey = entry->first, .deferredFrames = state.deferredFrames});
            }
            ++entry;
        }
        if (kind == DeferredUploadKind::Mesh) {
            summary.deferredMeshes = deferred;
        } else {
            summary.deferredTextures = deferred;
        }
    }
    summary.overdue = m_overdue;
    return summary;
}

bool DeferredUploadTracker::deferredThisFrame(DeferredUploadKind kind, uint64_t assetKey) const {
    const EntryMap& map = entries(kind);
    const auto found = map.find(assetKey);
    return found != map.end() && found->second.deferredThisFrame;
}

size_t DeferredUploadTracker::trackedAssetCount() const {
    return m_entries[0].size() + m_entries[1].size();
}

uint32_t DeferredUploadTracker::deferredFrames(DeferredUploadKind kind, uint64_t assetKey) const {
    const EntryMap& map = entries(kind);
    const auto found = map.find(assetKey);
    return found == map.end() ? 0u : found->second.deferredFrames;
}

DeferredUploadTracker::EntryMap& DeferredUploadTracker::entries(DeferredUploadKind kind) {
    return m_entries[static_cast<size_t>(kind)];
}

const DeferredUploadTracker::EntryMap& DeferredUploadTracker::entries(DeferredUploadKind kind) const {
    return m_entries[static_cast<size_t>(kind)];
}
