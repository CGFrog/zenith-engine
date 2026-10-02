#include "OperatorRegistry.hpp"

#include <utility>

OperatorRegistrationResult OperatorRegistry::add(Operator op) {
    if (op.id.empty()) {
        return {.error = "Operators need an id"};
    }
    if (find(op.id) != nullptr) {
        return {.error = "Operator '" + op.id + "' is already registered"};
    }
    if (op.label.empty()) {
        return {.error = "Operator '" + op.id + "' needs a label"};
    }
    if (!op.execute) {
        return {.error = "Operator '" + op.id + "' needs an execute function"};
    }
    if (!op.properties.empty()) {
        const std::vector<FieldDescriptor> fields(op.properties.fields().begin(), op.properties.fields().end());
        if (const std::string problem = validateFieldDescriptors(fields); !problem.empty()) {
            return {.error = "Operator '" + op.id + "' properties: " + problem};
        }
    }
    m_operators.push_back(std::make_unique<const Operator>(std::move(op)));
    return {.success = true};
}

bool OperatorRegistry::remove(std::string_view id) {
    return std::erase_if(m_operators, [id](const std::unique_ptr<const Operator>& op) { return op->id == id; }) > 0;
}

const Operator* OperatorRegistry::find(std::string_view id) const {
    for (const std::unique_ptr<const Operator>& op : m_operators) {
        if (op->id == id) {
            return op.get();
        }
    }
    return nullptr;
}

std::vector<const Operator*> OperatorRegistry::operators() const {
    std::vector<const Operator*> result;
    result.reserve(m_operators.size());
    for (const std::unique_ptr<const Operator>& op : m_operators) {
        result.push_back(op.get());
    }
    return result;
}

size_t OperatorRegistry::size() const {
    return m_operators.size();
}
