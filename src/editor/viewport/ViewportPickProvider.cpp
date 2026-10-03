#include "ViewportPickProvider.hpp"

RenderPickProvider::RenderPickProvider(RenderRuntime& rendering)
    : m_rendering(rendering) {}

bool RenderPickProvider::available(RenderViewId view) const {
    return m_rendering.viewAlive(view) && static_cast<bool>(m_rendering.viewTexture(view));
}

PickTicket RenderPickProvider::request(RenderViewId view, const PickRect& rect) {
    return m_rendering.requestPick(view, rect);
}

PickStatus RenderPickProvider::status(PickTicket ticket) const {
    return m_rendering.pickStatus(ticket);
}

PickResult RenderPickProvider::take(PickTicket ticket) {
    return m_rendering.pickResult(ticket);
}

void RenderPickProvider::cancel(PickTicket ticket) {
    m_rendering.cancelPick(ticket);
}
