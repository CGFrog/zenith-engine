#pragma once

#include "editor/operators/CodeOperators.hpp"

#include <optional>
#include <string>
#include <vector>

struct OperatorResult;

// The New Behaviour, Component, and System dialog the module.new_* operators
// open: a name, a category (or a system's phase), and fields, checked as
// they are typed. Create hands back module.generate_code's properties; the
// session runs it and reports the result with finish(), which closes the
// dialog or shows why generation failed.
class NewCodeDialog {
  public:
    void open(GeneratedCodeKind kind);
    bool isOpen() const;

    // Draws the dialog while it is open. Returns the properties to generate
    // with on the frame Create is pressed.
    std::optional<GenerateCodeProperties> draw();
    void finish(const OperatorResult& result);

    // What Create would generate now, or why it cannot (empty when it can).
    GenerateCodeProperties properties() const;
    std::string problem() const;

  private:
    struct FieldRow {
        std::string name;
        GeneratedFieldType type{GeneratedFieldType::Float};
    };

    void drawFields();

    GeneratedCodeKind m_kind{GeneratedCodeKind::Behaviour};
    std::string m_name;
    std::string m_category;
    SystemPhase m_phase{SystemPhase::Update};
    std::vector<FieldRow> m_fields;
    // Why the last Create failed.
    std::string m_failure;
    bool m_open{false};
    bool m_openPopup{false};
    bool m_focusName{false};
};
