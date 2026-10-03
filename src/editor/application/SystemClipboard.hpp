#pragma once

#include "editor/operators/ComponentOperators.hpp"

#include <string>

// The operating system clipboard, through ImGui's platform backend. Reading
// falls back to the text last written here while the system clipboard is
// empty or there is no ImGui context.
class SystemClipboard final : public TextClipboard {
  public:
    std::string text() const override;
    void setText(std::string text) override;

  private:
    std::string m_lastWritten;
};
