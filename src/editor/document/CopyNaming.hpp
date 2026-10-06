#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

// Names for copies of entities, Blender-style: Crate, Crate_1, Crate_2.

// A name split into its base and numeric suffix: "Crate_12" is {"Crate", 12}
// and "Crate" is {"Crate", 0}. Only a trailing "_<digits>" after a non-empty
// base counts as a suffix, so "_3" and "Crate_" are base names.
struct SplitCopyName {
    std::string_view base;
    uint64_t number{0};
};

SplitCopyName splitCopyName(std::string_view name);

// Chooses the name of each copy from the names already in use.
//
// A copy of a name nobody else uses keeps it. Otherwise the copy is
// "<base>_<n>", where n is one more than the largest number any used name
// with that base has (the bare base counts as 0), so copies of Crate_1 with
// Crate_2 in the scene become Crate_3. Every name copyOf returns is recorded as
// used, so a run of copies counts up.
class CopyNamer {
  public:
    // Records a name in use. Call it for every entity name in the scene before
    // asking for copies.
    void add(std::string_view name);
    // The name a copy of `name` should get; also recorded as in use.
    std::string copyOf(std::string_view name);

  private:
    std::unordered_set<std::string> m_used;
    // The largest suffix used with each base.
    std::unordered_map<std::string, uint64_t> m_largest;
};
