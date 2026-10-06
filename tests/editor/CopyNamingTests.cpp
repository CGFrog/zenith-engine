#include "editor/document/CopyNaming.hpp"

#include <iostream>
#include <string>

namespace {
int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void expectName(const std::string& actual, const std::string& expected, const std::string& message) {
    expect(actual == expected, message + ": expected '" + expected + "', got '" + actual + "'");
}

void testSplit() {
    expect(splitCopyName("Crate").base == "Crate" && splitCopyName("Crate").number == 0, "no suffix");
    expect(splitCopyName("Crate_12").base == "Crate" && splitCopyName("Crate_12").number == 12, "numeric suffix");
    expect(splitCopyName("Crate_").base == "Crate_", "a trailing separator is part of the base");
    expect(splitCopyName("_3").base == "_3", "a number without a base is a base");
    expect(splitCopyName("Crate_left").base == "Crate_left", "a word suffix is part of the base");
    expect(splitCopyName("a_b_7").base == "a_b" && splitCopyName("a_b_7").number == 7, "only the last suffix counts");
    expect(splitCopyName("Crate_12345678901").base == "Crate_12345678901", "an overlong number is not a counter");
}

void testUnusedNameIsKept() {
    CopyNamer namer;
    namer.add("Other");
    expectName(namer.copyOf("Crate"), "Crate", "a name nobody uses stays");
    // It is now in use, so the next copy is numbered.
    expectName(namer.copyOf("Crate"), "Crate_1", "the second copy is numbered");
}

void testDuplicateCountsUp() {
    CopyNamer namer;
    namer.add("Crate");
    expectName(namer.copyOf("Crate"), "Crate_1", "first duplicate");
    expectName(namer.copyOf("Crate"), "Crate_2", "second duplicate");
    expectName(namer.copyOf("Crate_1"), "Crate_3", "a copy of a numbered name continues the base's count");
}

void testUsesMaximumNotGaps() {
    CopyNamer namer;
    for (const char* name : {"Crate", "Crate_1", "Crate_5", "Crate_2"}) {
        namer.add(name);
    }
    expectName(namer.copyOf("Crate"), "Crate_6", "the number follows the largest used, not the first gap");
    expectName(namer.copyOf("Crate_2"), "Crate_7", "any member of the base uses the same count");
}

void testSuffixOfOtherBasesIsIgnored() {
    CopyNamer namer;
    for (const char* name : {"Wall", "Wall_9", "Crate_left", "Crate"}) {
        namer.add(name);
    }
    expectName(namer.copyOf("Crate"), "Crate_1", "other bases do not count");
    expectName(namer.copyOf("Wall"), "Wall_10", "the base's own largest does");
}

void testNumberedNameAlone() {
    CopyNamer namer;
    namer.add("Door_4");
    // The original base name is not in use, but Door_4 is: copying it counts from 4.
    expectName(namer.copyOf("Door_4"), "Door_5", "a lone numbered name continues its count");
}
} // namespace

int main() {
    testSplit();
    testUnusedNameIsKept();
    testDuplicateCountsUp();
    testUsesMaximumNotGaps();
    testSuffixOfOtherBasesIsIgnored();
    testNumberedNameAlone();
    return failures == 0 ? 0 : 1;
}
