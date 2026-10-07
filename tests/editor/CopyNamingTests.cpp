#include "editor/document/CopyNaming.hpp"

#include "support/Doctest.hpp"

#include <iostream>
#include <string>

TEST_SUITE_BEGIN("CopyNaming");

namespace {

void checkName(const std::string& actual, const std::string& expected, const std::string& message) {
    CHECK_MESSAGE(actual == expected, (message + ": expected '" + expected + "', got '" + actual + "'"));
}

TEST_CASE("CopyNaming: split") {
    CHECK_MESSAGE((splitCopyName("Crate").base == "Crate" && splitCopyName("Crate").number == 0), "no suffix");
    CHECK_MESSAGE((splitCopyName("Crate_12").base == "Crate" && splitCopyName("Crate_12").number == 12),
                  "numeric suffix");
    CHECK_MESSAGE(splitCopyName("Crate_").base == "Crate_", "a trailing separator is part of the base");
    CHECK_MESSAGE(splitCopyName("_3").base == "_3", "a number without a base is a base");
    CHECK_MESSAGE(splitCopyName("Crate_left").base == "Crate_left", "a word suffix is part of the base");
    CHECK_MESSAGE((splitCopyName("a_b_7").base == "a_b" && splitCopyName("a_b_7").number == 7),
                  "only the last suffix counts");
    CHECK_MESSAGE(splitCopyName("Crate_12345678901").base == "Crate_12345678901",
                  "an overlong number is not a counter");
}

TEST_CASE("CopyNaming: unused name is kept") {
    CopyNamer namer;
    namer.add("Other");
    checkName(namer.copyOf("Crate"), "Crate", "a name nobody uses stays");
    // It is now in use, so the next copy is numbered.
    checkName(namer.copyOf("Crate"), "Crate_1", "the second copy is numbered");
}

TEST_CASE("CopyNaming: duplicate counts up") {
    CopyNamer namer;
    namer.add("Crate");
    checkName(namer.copyOf("Crate"), "Crate_1", "first duplicate");
    checkName(namer.copyOf("Crate"), "Crate_2", "second duplicate");
    checkName(namer.copyOf("Crate_1"), "Crate_3", "a copy of a numbered name continues the base's count");
}

TEST_CASE("CopyNaming: uses maximum not gaps") {
    CopyNamer namer;
    for (const char* name : {"Crate", "Crate_1", "Crate_5", "Crate_2"}) {
        namer.add(name);
    }
    checkName(namer.copyOf("Crate"), "Crate_6", "the number follows the largest used, not the first gap");
    checkName(namer.copyOf("Crate_2"), "Crate_7", "any member of the base uses the same count");
}

TEST_CASE("CopyNaming: suffix of other bases is ignored") {
    CopyNamer namer;
    for (const char* name : {"Wall", "Wall_9", "Crate_left", "Crate"}) {
        namer.add(name);
    }
    checkName(namer.copyOf("Crate"), "Crate_1", "other bases do not count");
    checkName(namer.copyOf("Wall"), "Wall_10", "the base's own largest does");
}

TEST_CASE("CopyNaming: numbered name alone") {
    CopyNamer namer;
    namer.add("Door_4");
    // The original base name is not in use, but Door_4 is: copying it counts from 4.
    checkName(namer.copyOf("Door_4"), "Door_5", "a lone numbered name continues its count");
}
} // namespace

TEST_SUITE_END();
