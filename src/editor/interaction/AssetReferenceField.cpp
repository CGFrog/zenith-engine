#include "AssetReferenceField.hpp"

#include "editor/document/EditorDocument.hpp"
#include "editor/interaction/ReflectedFieldEditor.hpp"
#include "editor/panels/AssetDragPayloads.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/project/ProjectAssets.hpp"
#include "editor/project/ProjectFileOperations.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"
#include "engine/graphics/Rendering.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <span>

namespace {
constexpr std::array<EditorAssetKind, 7> everyKind{
    EditorAssetKind::Model,  EditorAssetKind::Texture, EditorAssetKind::Audio,   EditorAssetKind::Scene,
    EditorAssetKind::Prefab, EditorAssetKind::Script,  EditorAssetKind::Material};
constexpr const char* pickerPopup = "##AssetPicker";
constexpr int pickerVisibleRows = 10;

std::string lowercase(std::string text) {
    std::ranges::transform(text, text.begin(),
                           [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return text;
}

// True when path is base or lies inside it.
bool isWithin(const std::filesystem::path& path, const std::filesystem::path& base) {
    const std::filesystem::path relative = path.lexically_relative(base);
    return !relative.empty() && *relative.begin() != "..";
}

std::filesystem::path absoluteAsset(const AssetReferenceFieldContext& context, const std::string& value) {
    return normalizedCatalogPath(context.assetRoot / pathFromUtf8Text(value));
}

ThemeColor stateInk(AssetReferenceState state) {
    switch (state) {
    case AssetReferenceState::None:
        return palette::inkMuted;
    case AssetReferenceState::Present:
        return palette::ink;
    case AssetReferenceState::Missing:
        return palette::error;
    }
    return palette::ink;
}

// The absolute path of an asset of an accepted kind dragged onto the last
// item, once dropped.
std::optional<std::filesystem::path> acceptAssetDrop(std::span<const EditorAssetKind> kinds) {
    std::optional<std::filesystem::path> dropped;
    if (!ImGui::BeginDragDropTarget()) {
        return dropped;
    }
    for (const EditorAssetKind kind : kinds) {
        const char* type = assetDragPayloadFor(kind);
        if (type == nullptr) {
            continue;
        }
        const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(type);
        if (payload == nullptr || payload->DataSize <= 0) {
            continue;
        }
        const auto* text = static_cast<const char*>(payload->Data);
        const std::string_view path(text, strnlen(text, static_cast<size_t>(payload->DataSize)));
        // Models and prefabs travel in the narrow encoding the viewports read.
        dropped = kind == EditorAssetKind::Model || kind == EditorAssetKind::Prefab ? std::filesystem::path(path)
                                                                                    : pathFromUtf8Text(path);
    }
    ImGui::EndDragDropTarget();
    return dropped;
}

// One picker row: a thumbnail (images only) and the asset's value.
bool drawCandidate(const AssetReferenceFieldContext& context, const EditorAssetRecord& asset, const std::string& value,
                   bool selected) {
    const float rowHeight = ImGui::GetFrameHeight();
    const std::string label = "##" + value;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const bool chosen = ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_None, {0.0f, rowHeight});
    ImGui::SetItemTooltip("%s", utf8PathText(asset.absolutePath).c_str());
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    float textLeft = start.x;
    if (context.thumbnail && asset.kind == EditorAssetKind::Texture) {
        if (const uint64_t texture = context.thumbnail(asset.absolutePath); texture != 0) {
            drawList->AddImage(static_cast<ImTextureID>(texture), start, {start.x + rowHeight, start.y + rowHeight});
        }
        textLeft += rowHeight + ImGui::GetStyle().ItemInnerSpacing.x;
    }
    drawList->AddText({textLeft, start.y + ImGui::GetStyle().FramePadding.y}, widgets::color(palette::ink),
                      value.c_str());
    return chosen;
}

// The picker's search box and candidates; returns the chosen value (empty
// for None).
std::optional<std::string> drawPicker(const AssetReferenceFieldContext& context, std::string_view assetKind,
                                      const std::string& current) {
    std::optional<std::string> chosen;
    // UI-thread state of the one picker that can be open at a time, cleared
    // whenever a picker opens.
    static std::string search;
    if (ImGui::IsWindowAppearing()) {
        search.clear();
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
    ImGui::InputTextWithHint("##AssetSearch", "Search...", &search);
    if (ImGui::Selectable("None", current.empty())) {
        chosen = std::string{};
    }
    const std::vector<const EditorAssetRecord*> candidates = assetFieldCandidates(context, assetKind, search);
    if (candidates.empty()) {
        ImGui::TextDisabled("No matching assets in the project.");
    }
    const float listHeight = ImGui::GetFrameHeightWithSpacing() *
                             static_cast<float>(std::min<int>(static_cast<int>(candidates.size()), pickerVisibleRows));
    if (!candidates.empty() && ImGui::BeginChild("##Candidates", {ImGui::GetFontSize() * 18.0f, listHeight})) {
        for (const EditorAssetRecord* asset : candidates) {
            const std::optional<std::string> value = projectAssetValue(context.assetRoot, asset->absolutePath);
            if (value && drawCandidate(context, *asset, *value, *value == current)) {
                chosen = *value;
            }
        }
    }
    if (!candidates.empty()) {
        ImGui::EndChild();
    }
    if (chosen) {
        ImGui::CloseCurrentPopup();
    }
    return chosen;
}

// A recessed box naming the asset; clicking it opens the picker.
void drawValueBox(float width, const std::string& value, AssetReferenceState state, bool mixed) {
    const bool clicked = ImGui::InvisibleButton("##value", {width, ImGui::GetFrameHeight()});
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(min, max, widgets::color(hovered ? palette::fieldHovered : palette::field));
    std::string text = mixed           ? std::string(mixedValueText)
                       : value.empty() ? std::string("None")
                                       : utf8PathText(pathFromUtf8Text(value).filename());
    if (!mixed && state == AssetReferenceState::Missing) {
        text += " (missing)";
    }
    const ImVec2 padding = ImGui::GetStyle().FramePadding;
    drawList->PushClipRect({min.x + padding.x, min.y}, {max.x - padding.x, max.y}, true);
    drawList->AddText({min.x + padding.x, min.y + padding.y},
                      widgets::color(mixed ? palette::inkMuted : stateInk(state)), text.c_str());
    drawList->PopClipRect();
    if (hovered) {
        if (mixed) {
            ImGui::SetTooltip("The selected objects use different assets.");
        } else if (value.empty()) {
            ImGui::SetTooltip("No asset. Click to choose one, or drop one from the Asset Browser here.");
        } else {
            ImGui::SetTooltip("%s%s", value.c_str(),
                              state == AssetReferenceState::Missing ? "\nNo loaded project asset has this path." : "");
        }
    }
    if (clicked) {
        ImGui::OpenPopup(pickerPopup);
    }
}
} // namespace

std::optional<AssetReferenceFieldContext> assetReferenceFieldContext(EditorContext& context) {
    if (context.projectAssets == nullptr) {
        return std::nullopt;
    }
    ProjectAssets* project = context.projectAssets;
    RenderRuntime* rendering = &context.app.rendering;
    return AssetReferenceFieldContext{.catalog = project->catalog(),
                                      .assetRoot =
                                          normalizedCatalogPath(context.editing.document().options().assetRoot),
                                      .thumbnail = [project, rendering](const std::filesystem::path& image) {
                                          return project->thumbnailTexture(*rendering, image).value;
                                      }};
}

std::vector<EditorAssetKind> editorAssetKindsFor(std::string_view assetKind) {
    if (assetKind == "texture" || assetKind == "environment") {
        return {EditorAssetKind::Texture};
    }
    if (assetKind == "audio") {
        return {EditorAssetKind::Audio};
    }
    if (assetKind == "material") {
        return {EditorAssetKind::Material};
    }
    if (assetKind == "model") {
        return {EditorAssetKind::Model};
    }
    return {everyKind.begin(), everyKind.end()};
}

const char* assetDragPayloadFor(EditorAssetKind kind) {
    switch (kind) {
    case EditorAssetKind::Model:
        return modelAssetPayload;
    case EditorAssetKind::Prefab:
        return prefabAssetPayload;
    case EditorAssetKind::Texture:
        return textureAssetPayload;
    case EditorAssetKind::Audio:
        return audioAssetPayload;
    case EditorAssetKind::Material:
        return materialAssetPayload;
    case EditorAssetKind::Scene:
    case EditorAssetKind::Script:
        break;
    }
    return nullptr;
}

std::optional<std::string> projectAssetValue(const std::filesystem::path& assetRoot,
                                             const std::filesystem::path& absolutePath) {
    const std::filesystem::path root = normalizedCatalogPath(assetRoot);
    const std::filesystem::path path = normalizedCatalogPath(absolutePath);
    if (path == root || !isWithin(path, root)) {
        return std::nullopt;
    }
    const std::u8string text = path.lexically_relative(root).generic_u8string();
    return std::string(text.begin(), text.end());
}

std::vector<const EditorAssetRecord*> assetFieldCandidates(const AssetReferenceFieldContext& context,
                                                           std::string_view assetKind, std::string_view search) {
    const std::vector<EditorAssetKind> kinds = editorAssetKindsFor(assetKind);
    const std::string needle = lowercase(std::string(search));
    std::vector<const EditorAssetRecord*> candidates;
    for (const EditorAssetRecord& asset : context.catalog.assets()) {
        if (std::ranges::find(kinds, asset.kind) == kinds.end() ||
            !projectAssetValue(context.assetRoot, asset.absolutePath)) {
            continue;
        }
        if (!needle.empty() &&
            lowercase(utf8PathText(asset.absolutePath.filename())).find(needle) == std::string::npos) {
            continue;
        }
        candidates.push_back(&asset);
    }
    // The catalog lists by absolute path, which orders values alike.
    return candidates;
}

AssetReferenceState assetReferenceState(const AssetReferenceFieldContext& context, const std::string& value,
                                        std::optional<bool> loaded) {
    if (value.empty()) {
        return AssetReferenceState::None;
    }
    if (loaded == false || context.catalog.findAsset(absoluteAsset(context, value)) == nullptr) {
        return AssetReferenceState::Missing;
    }
    return AssetReferenceState::Present;
}

bool drawAssetReferenceField(const char* label, std::string& value, std::string_view assetKind,
                             const AssetReferenceFieldContext& context, const AssetReferenceFieldOptions& options) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float spacing = style.ItemInnerSpacing.x;
    const float buttonsWidth =
        options.clearable ? ImGui::CalcTextSize("Clear").x + style.FramePadding.x * 2.0f + spacing : 0.0f;
    const float valueWidth = std::max(ImGui::CalcItemWidth() - buttonsWidth, ImGui::GetFrameHeight());
    const AssetReferenceState state = assetReferenceState(context, value, options.loaded);

    bool changed = false;
    const auto assign = [&](std::string chosen) {
        if (options.mixed || chosen != value) {
            value = std::move(chosen);
            changed = true;
        }
    };
    drawValueBox(valueWidth, value, state, options.mixed);
    const std::vector<EditorAssetKind> kinds = editorAssetKindsFor(assetKind);
    if (const std::optional<std::filesystem::path> dropped = acceptAssetDrop(kinds)) {
        if (std::optional<std::string> droppedValue = projectAssetValue(context.assetRoot, *dropped)) {
            assign(std::move(*droppedValue));
        }
    }
    if (ImGui::BeginPopup(pickerPopup)) {
        if (std::optional<std::string> chosen = drawPicker(context, assetKind, options.mixed ? std::string{} : value)) {
            assign(std::move(*chosen));
        }
        ImGui::EndPopup();
    }
    if (options.clearable) {
        ImGui::SameLine(0.0f, spacing);
        ImGui::BeginDisabled(value.empty() && !options.mixed);
        if (widgets::Button("Clear")) {
            assign({});
        }
        ImGui::EndDisabled();
    }
    ImGui::SameLine(0.0f, spacing);
    ImGui::TextUnformatted(label);
    return changed;
}
