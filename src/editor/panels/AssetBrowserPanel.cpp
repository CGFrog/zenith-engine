#include "AssetBrowserPanel.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/panels/EditorContext.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <utility>

namespace {
constexpr float refreshIntervalSeconds = 1.0f;

const char* kindName(EditorAssetKind kind) {
    switch (kind) {
    case EditorAssetKind::Model:
        return "Model";
    case EditorAssetKind::Texture:
        return "Texture";
    case EditorAssetKind::Audio:
        return "Audio";
    case EditorAssetKind::Scene:
        return "Scene";
    }
    return "Asset";
}

std::string lowercase(std::string text) {
    std::ranges::transform(text, text.begin(),
                           [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return text;
}
} // namespace

AssetBrowserPanel::AssetBrowserPanel(std::filesystem::path assetRoot, std::filesystem::path sceneRoot)
    : m_catalog(std::move(assetRoot), std::move(sceneRoot)) {}

const char* AssetBrowserPanel::title() const {
    return "Asset Browser";
}

void AssetBrowserPanel::update(EditorContext& context, const InputState&, float deltaSeconds) {
    m_refreshElapsed += deltaSeconds;
    if (m_refreshElapsed >= refreshIntervalSeconds) {
        m_refreshElapsed = 0.0f;
        refresh(context.diagnostics);
    }
}

void AssetBrowserPanel::refresh(EditorDiagnostics& diagnostics) {
    AssetCatalogResult result = m_catalog.refresh();
    if (!result) {
        diagnostics.reportError(result.error);
        return;
    }
    if (m_initialized && !result.changes.empty()) {
        for (const AssetCatalogChange& change : result.changes) {
            if (change.asset.kind == EditorAssetKind::Texture) {
                m_thumbnails.invalidate(change.asset.absolutePath);
            }
        }
        diagnostics.reportError(std::to_string(result.changes.size()) +
                                " project asset change(s) detected. Select a changed model and choose Reload Model "
                                "to reload it explicitly.");
    }
    m_initialized = true;
}

void AssetBrowserPanel::draw(EditorContext& context) {
    if (ImGui::Button("Refresh")) {
        refresh(context.diagnostics);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##AssetFilter", "Filter assets...", &m_filter);
    ImGui::Separator();

    const std::string filter = lowercase(m_filter);
    for (const EditorAssetRecord& asset : m_catalog.assets()) {
        const std::string label = asset.relativePath.generic_string();
        if (!filter.empty() && lowercase(label).find(filter) == std::string::npos) {
            continue;
        }

        ImGui::PushID(asset.absolutePath.string().c_str());
        ImGui::TextDisabled("[%s]", kindName(asset.kind));
        if (asset.kind == EditorAssetKind::Texture) {
            m_thumbnails.request(asset.absolutePath);
            if (const auto thumbnail = m_thumbnails.find(asset.absolutePath);
                thumbnail && thumbnail->state == ThumbnailState::Ready) {
                ImGui::SameLine();
                ImGui::TextDisabled("%ux%u", thumbnail->width, thumbnail->height);
            }
        }
        ImGui::SameLine();
        const bool selectedAsset = m_selectedAsset == asset.absolutePath;
        if (ImGui::Selectable(label.c_str(), selectedAsset, ImGuiSelectableFlags_AllowDoubleClick)) {
            m_selectedAsset = asset.absolutePath;
            if (asset.kind == EditorAssetKind::Scene && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                context.actions.requestOpenScene(context.app, asset.absolutePath);
            }
        }
        if (asset.kind == EditorAssetKind::Model && ImGui::BeginDragDropSource()) {
            const std::string path = asset.absolutePath.string();
            ImGui::SetDragDropPayload("ZENITH_MODEL_ASSET", path.c_str(), path.size() + 1);
            ImGui::Text("Place %s", label.c_str());
            ImGui::EndDragDropSource();
        }
        ImGui::PopID();
    }
    drawSelectedAsset(context);
}

void AssetBrowserPanel::drawSelectedAsset(EditorContext& context) {
    if (m_selectedAsset.empty()) {
        return;
    }
    ImGui::Separator();
    ImGui::TextWrapped("%s", m_selectedAsset.string().c_str());
    const auto found = std::ranges::find_if(
        m_catalog.assets(), [this](const EditorAssetRecord& asset) { return asset.absolutePath == m_selectedAsset; });
    if (found == m_catalog.assets().end() || found->kind != EditorAssetKind::Model || !ImGui::Button("Reload Model")) {
        return;
    }
    const AssetLoadResult<ModelHandle> result = context.app.assets.reloadModel(found->absolutePath);
    if (!result) {
        context.diagnostics.reportError("Model reload failed: " + result.error);
    } else {
        context.diagnostics.reportError("Model reloaded. Existing instances retain their previous immutable "
                                        "resources; new placements use the refreshed model.");
    }
}
