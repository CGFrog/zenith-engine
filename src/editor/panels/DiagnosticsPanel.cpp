#include "DiagnosticsPanel.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/project/ExternalEditor.hpp"
#include "editor/settings/EditorSettings.hpp"
#include "editor/theme/EditorWidgets.hpp"

#include <imgui.h>

#include <limits>
#include <string>
#include <vector>

namespace {
EditorIcon severityIcon(EditorDiagnosticSeverity severity) {
    switch (severity) {
    case EditorDiagnosticSeverity::Error:
        return EditorIcon::Error;
    case EditorDiagnosticSeverity::Warning:
        return EditorIcon::Warning;
    case EditorDiagnosticSeverity::Info:
        return EditorIcon::Info;
    }
    return EditorIcon::Info;
}

const char* severityName(EditorDiagnosticSeverity severity) {
    switch (severity) {
    case EditorDiagnosticSeverity::Error:
        return "Error";
    case EditorDiagnosticSeverity::Warning:
        return "Warning";
    case EditorDiagnosticSeverity::Info:
        return "Info";
    }
    return "Info";
}

const char* assetKindName(AssetKind kind) {
    switch (kind) {
    case AssetKind::Model:
        return "Model";
    case AssetKind::Mesh:
        return "Mesh";
    case AssetKind::Material:
        return "Material";
    case AssetKind::Texture:
        return "Texture";
    case AssetKind::Font:
        return "Font";
    }
    return "Font";
}
} // namespace

DiagnosticsPanel::DiagnosticsPanel(const EditorToolSettings& tools)
    : m_tools(tools) {}

const char* DiagnosticsPanel::title() const {
    return "Diagnostics";
}

void DiagnosticsPanel::draw(EditorContext& context) {
    drawDiagnostics(context.diagnostics);
    ImGui::SeparatorText("Asset lifetime");
    drawAssetLifetime(context);
    drawAudioLifetime(context);
}

void DiagnosticsPanel::drawDiagnostics(EditorDiagnostics& diagnostics) {
    if (diagnostics.entries().empty()) {
        ImGui::TextDisabled("No diagnostics.");
        return;
    }
    // Opening may add an IDE diagnostic, so it runs after the loop.
    std::optional<EditorSourceLocation> open;
    for (const EditorDiagnostic& diagnostic : diagnostics.entries()) {
        ImGui::PushID(static_cast<int>(diagnostic.id));
        ImGui::Text("%s[%s] %s", widgets::IconLabelPadding(), severityName(diagnostic.severity),
                    diagnostic.source.c_str());
        widgets::ItemIcon(severityIcon(diagnostic.severity), 0.0f);
        ImGui::TextWrapped("%s", diagnostic.message.c_str());
        if (diagnostic.location) {
            const EditorSourceLocation& location = *diagnostic.location;
            const std::string label = location.file.filename().string() + "(" + std::to_string(location.line) +
                                      (location.column != 0 ? "," + std::to_string(location.column) : "") + ")";
            if (ImGui::TextLink(label.c_str())) {
                open = location;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Open %s in the IDE", location.file.string().c_str());
            }
        }
        if (!diagnostic.action.empty()) {
            ImGui::TextDisabled("Action: %s", diagnostic.action.c_str());
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Dismiss")) {
            // Dismissing invalidates the entry list, so stop iterating.
            const uint64_t id = diagnostic.id;
            diagnostics.dismiss(id);
            ImGui::PopID();
            break;
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    if (open) {
        openSourceLocation(m_tools.ideCommand, *open, diagnostics);
    }
}

void DiagnosticsPanel::drawAssetLifetime(EditorContext& context) {
    AssetManager& assets = context.app.assets;
    const AssetMemoryBudget budget = assets.memoryBudget();
    ImGui::Text("CPU retained: %llu bytes", static_cast<unsigned long long>(assets.cpuBytes()));
    if (budget.cpuBytes != std::numeric_limits<uint64_t>::max()) {
        ImGui::SameLine();
        ImGui::TextDisabled("Budget: %llu bytes", static_cast<unsigned long long>(budget.cpuBytes));
    }
    if (widgets::Button("Collect unused assets")) {
        // The scene may have changed during this UI callback. This fresh
        // lease set protects it while collection runs; renderer frame leases
        // protect submitted GPU work independently.
        const AssetLeaseSet liveAssets = assets.leaseSceneAssets(context.app.scene);
        static_cast<void>(liveAssets);
        m_lastAssetCollection = assets.collectUnused(true);
        m_lastAudioCollection = context.app.audio.collectUnusedAssets(true);
    }
    if (m_lastAssetCollection) {
        const AssetCollectionResult& collection = *m_lastAssetCollection;
        ImGui::SameLine();
        ImGui::Text("%u evicted, %llu bytes freed", collection.assetsEvicted,
                    static_cast<unsigned long long>(collection.bytesFreed));
        if (!collection.budgetSatisfied) {
            ImGui::TextWrapped("Collection could not satisfy the asset memory budget. Inspect strong-reference counts "
                               "below and release previews, scenes, voices, or in-flight work before retrying.");
        }
    }

    const std::vector<AssetDiagnostic> entries = assets.diagnostics();
    if (!ImGui::BeginTable("Asset ownership", 6,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        return;
    }
    ImGui::TableSetupColumn("Kind");
    ImGui::TableSetupColumn("Slot");
    ImGui::TableSetupColumn("Generation");
    ImGui::TableSetupColumn("Strong refs");
    ImGui::TableSetupColumn("CPU bytes");
    ImGui::TableSetupColumn("GPU estimate");
    ImGui::TableHeadersRow();
    for (const AssetDiagnostic& asset : entries) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(assetKindName(asset.kind));
        ImGui::TableNextColumn();
        ImGui::Text("%u", asset.index);
        ImGui::TableNextColumn();
        ImGui::Text("%u", asset.generation);
        ImGui::TableNextColumn();
        ImGui::Text("%u", asset.strongReferences);
        ImGui::TableNextColumn();
        ImGui::Text("%llu", static_cast<unsigned long long>(asset.cpuBytes));
        ImGui::TableNextColumn();
        ImGui::Text("%llu", static_cast<unsigned long long>(asset.gpuBytes));
        if (!asset.source.empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", asset.source.c_str());
        }
    }
    ImGui::EndTable();
}

void DiagnosticsPanel::drawAudioLifetime(EditorContext& context) {
    AudioSystem& audio = context.app.audio;
    const std::vector<AudioAssetDiagnostic> entries = audio.assetDiagnostics();
    ImGui::Text("Decoded audio retained: %llu bytes", static_cast<unsigned long long>(audio.decodedAudioBytes()));
    if (m_lastAudioCollection) {
        ImGui::SameLine();
        ImGui::Text("%u clips and %u streams evicted", m_lastAudioCollection->clipsEvicted,
                    m_lastAudioCollection->streamsEvicted);
    }
    if (!ImGui::BeginTable("Audio asset ownership", 5,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        return;
    }
    ImGui::TableSetupColumn("Kind");
    ImGui::TableSetupColumn("Slot");
    ImGui::TableSetupColumn("Generation");
    ImGui::TableSetupColumn("Active voices");
    ImGui::TableSetupColumn("Decoded bytes");
    ImGui::TableHeadersRow();
    for (const AudioAssetDiagnostic& asset : entries) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(asset.streaming ? "Stream" : "Clip");
        ImGui::TableNextColumn();
        ImGui::Text("%u", asset.index);
        ImGui::TableNextColumn();
        ImGui::Text("%u", asset.generation);
        ImGui::TableNextColumn();
        ImGui::Text("%u", asset.strongReferences);
        ImGui::TableNextColumn();
        ImGui::Text("%llu", static_cast<unsigned long long>(asset.decodedBytes));
        if (!asset.source.empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", asset.source.c_str());
        }
    }
    ImGui::EndTable();
}
