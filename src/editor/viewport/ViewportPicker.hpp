#pragma once

// Resolves one viewport pick at a time: GPU pick when available, CPU fallback otherwise.

#include "editor/viewport/RegionSelect.hpp"
#include "editor/viewport/SceneGizmoDraw.hpp"
#include "editor/viewport/ViewportPickProvider.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/scene/Scene.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <vector>

enum class ViewportPickKind : uint8_t {
    // The entity under the cursor.
    Click,
    // Every entity under the cursor, front to back, for Alt+click cycling.
    Cycle,
    // Every entity inside a box or lasso.
    Region,
};

// One pick a viewport asks for, with what the CPU fallback needs to answer it
// the same way: the lens and view size at the time of the request and the
// local view's isolation.
struct ViewportPickQuery {
    ViewportPickKind kind{ViewportPickKind::Click};
    // Click and Cycle: the cursor in view pixels (continuous, top-left
    // origin; pixel (x, y) spans [x, x + 1)).
    glm::vec2 point{0.0f};
    // Region: the finished box or lasso in view pixels.
    RegionSelection region;
    RenderView lens;
    // The view's output size in pixels.
    glm::vec2 viewSize{1.0f};
    // Sorted render objects the view is isolated to (local view); nullopt
    // picks everything.
    std::optional<std::vector<RenderObjectId>> isolatedObjects;
    // Objects without a mesh the view marks (IconPicking: pickableIcons),
    // tested before the meshes; empty when the view draws no overlays.
    std::vector<SceneGizmoAnchor> icons;
};

// A resolved pick, in the same order the query defines: Click holds at most
// one entity, Cycle the candidates front to back (visible ones first, then
// ones a ray hits behind them), Region every entity inside. Icons the query
// lists come first (mergeIconPicks): a click on one picks it over the mesh
// behind it. Only entities the
// editor may select (SceneVisibility::selectable in Editor mode) inside the
// isolation appear; a collection instance appears as its instancing entity.
struct ViewportPickOutcome {
    ViewportPickKind kind{ViewportPickKind::Click};
    std::vector<SceneEntityId> entities;
    // Whether the GPU id buffer answered; false for the CPU fallback.
    bool fromGpu{false};
};

// Resolves one viewport pick at a time, preferring the GPU.
//
// Timing: start() requests an id-buffer pick when the provider can serve the
// view; the renderer draws it with the view's next frame and the result is
// ready one to three frames later. Selection should still feel immediate, so
// the viewport starts the pick on mouse press and calls poll() every frame:
// a Ready result is applied as soon as it arrives, and the CPU answers at
// once instead when there is no GPU or the view does not render (the first
// poll), when the request was refused, failed, or was cancelled (a view
// resize), or when it is still pending after frameBudget polls, after which
// the late GPU request is cancelled. Starting a new pick cancels a pending
// one.
class ViewportPicker {
  public:
    // Click tolerance and the Alt+click candidate radius, in view pixels.
    static constexpr uint32_t clickRadius = 4;
    // Polls a GPU pick may stay pending before the CPU answers instead.
    static constexpr uint32_t frameBudget = 4;

    void start(ViewportPickProvider& provider, RenderViewId view, ViewportPickQuery query);
    bool pending() const;
    // The resolved pick, or nullopt while it waits or when none is pending.
    std::optional<ViewportPickOutcome> poll(ViewportPickProvider& provider, const Scene& scene,
                                            const AssetManager& assets);
    void cancel(ViewportPickProvider& provider);

  private:
    struct Pending {
        ViewportPickQuery query;
        PickTicket ticket;
        uint32_t polls{0};
    };

    std::optional<Pending> m_pending;
};

// The id-buffer rectangle (view pixels) a query asks for.
PickRect pickRectFor(const ViewportPickQuery& query);
// Answers a query from a finished GPU pick of pickRectFor(query).
ViewportPickOutcome resolveGpuPick(const ViewportPickQuery& query, const PickResult& result, const Scene& scene,
                                   const AssetManager& assets);
// Answers a query by ray casts (click, cycle) or by projecting each object's
// bounds centre (box, lasso).
ViewportPickOutcome resolveCpuPick(const ViewportPickQuery& query, const Scene& scene, const AssetManager& assets);
