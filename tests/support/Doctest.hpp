#pragma once

// What a unit test includes: doctest, the support helpers, and printing of glm values in failed checks.

#include "support/Approx.hpp"
#include "support/Files.hpp"
#include "support/TempDirectory.hpp"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <sstream>

namespace doctest {

// A failed CHECK(a == b) on glm values prints them, not "{?}".
template <glm::length_t Length, typename Value, glm::qualifier Q> struct StringMaker<glm::vec<Length, Value, Q>> {
    static String convert(const glm::vec<Length, Value, Q>& value) {
        std::ostringstream text;
        text << '(';
        for (glm::length_t i = 0; i < Length; ++i) {
            text << (i == 0 ? "" : ", ") << value[i];
        }
        text << ')';
        return text.str().c_str();
    }
};

template <glm::length_t Columns, glm::length_t Rows, typename Value, glm::qualifier Q>
struct StringMaker<glm::mat<Columns, Rows, Value, Q>> {
    static String convert(const glm::mat<Columns, Rows, Value, Q>& value) {
        std::ostringstream text;
        text << '[';
        for (glm::length_t column = 0; column < Columns; ++column) {
            text << (column == 0 ? "" : ", ") << StringMaker<glm::vec<Rows, Value, Q>>::convert(value[column]).c_str();
        }
        text << ']';
        return text.str().c_str();
    }
};

template <typename Value, glm::qualifier Q> struct StringMaker<glm::qua<Value, Q>> {
    static String convert(const glm::qua<Value, Q>& value) {
        std::ostringstream text;
        text << "quat(w " << value.w << ", " << value.x << ", " << value.y << ", " << value.z << ')';
        return text.str().c_str();
    }
};

} // namespace doctest
