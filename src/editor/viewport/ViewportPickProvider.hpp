#pragma once

#include "engine/graphics/Rendering.hpp"

// Where a viewport's GPU picks come from (RenderRuntime::requestPick and its
// companions). The editor uses RenderPickProvider; tests substitute a fake
// that answers with a synthetic id grid, so click, box, lasso, and cycling
// flows run without a GPU.
class ViewportPickProvider {
  public:
    virtual ~ViewportPickProvider() = default;

    // Whether a GPU pick of `view` can resolve: the view exists and renders.
    // When it cannot, the viewport picks on the CPU at once.
    virtual bool available(RenderViewId view) const = 0;
    // Empty when the request was refused.
    virtual PickTicket request(RenderViewId view, const PickRect& rect) = 0;
    virtual PickStatus status(PickTicket ticket) const = 0;
    // Hands a finished result over once (RenderRuntime::pickResult).
    virtual PickResult take(PickTicket ticket) = 0;
    virtual void cancel(PickTicket ticket) = 0;
};

// The renderer's picks. A view counts as rendering once it has published a
// texture, so headless runs (no renderer) always pick on the CPU.
class RenderPickProvider final : public ViewportPickProvider {
  public:
    explicit RenderPickProvider(RenderRuntime& rendering);

    bool available(RenderViewId view) const override;
    PickTicket request(RenderViewId view, const PickRect& rect) override;
    PickStatus status(PickTicket ticket) const override;
    PickResult take(PickTicket ticket) override;
    void cancel(PickTicket ticket) override;

  private:
    RenderRuntime& m_rendering;
};
