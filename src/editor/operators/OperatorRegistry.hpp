#pragma once

#include "editor/operators/Operator.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct OperatorRegistrationResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// The catalog of operators. The editor registers its own; game or tool
// modules may add theirs later through the same call. Operators are kept in
// registration order and never removed, so pointers from find() stay valid
// for the registry's lifetime.
class OperatorRegistry {
  public:
    // Rejects an empty or duplicate id, a missing label or execute function,
    // and property fields that do not describe their struct.
    OperatorRegistrationResult add(Operator op);
    const Operator* find(std::string_view id) const;
    // Registration order, which is also the F3 search order for equal scores.
    std::vector<const Operator*> operators() const;
    size_t size() const;

  private:
    std::vector<std::unique_ptr<const Operator>> m_operators;
};
