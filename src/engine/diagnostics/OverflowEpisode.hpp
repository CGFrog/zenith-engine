#pragma once

// Tracks one runtime limit across frames or steps so a sustained overflow is
// reported once instead of every time it recurs. An episode starts on the
// first update that exceeds the limit and ends on the next update that does
// not; the caller logs when update() reports a new episode.
class OverflowEpisode {
  public:
    // Records whether the limit is exceeded now. Returns true when this update
    // starts a new episode.
    bool update(bool exceeded) {
        const bool started = exceeded && !m_active;
        m_active = exceeded;
        return started;
    }

    bool active() const {
        return m_active;
    }

  private:
    bool m_active{false};
};
