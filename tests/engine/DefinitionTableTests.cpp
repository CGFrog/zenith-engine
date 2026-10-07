// CPU-only coverage of definition tables: entries by id and in file order,
// nested lists, the strict errors a hand-written file gets, failed loads that
// keep the table, and reloading a changed file.

#include "engine/gameplay/DefinitionTable.hpp"

#include "support/Doctest.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_BEGIN("DefinitionTable");

namespace {

bool contains(const std::string& text, std::string_view fragment) {
    return text.find(fragment) != std::string::npos;
}

enum class Rarity : uint8_t {
    Common,
    Rare,
};

struct ItemDef {
    std::string id;
    std::string name;
    float damage{0.0f};
    Rarity rarity{Rarity::Common};
};

Fields<ItemDef> itemFields() {
    return Fields<ItemDef>()
        .add("id", &ItemDef::id)
        .add("name", &ItemDef::name)
        .add("damage", &ItemDef::damage)
        .optional()
        .add("rarity", &ItemDef::rarity)
        .options({{"common", "Common", 0}, {"rare", "Rare", 1}})
        .optional();
}

struct LootDrop {
    std::string item;
    float weight{1.0f};
};

struct LootTableDef {
    std::string id;
    std::vector<LootDrop> drops;
};

Fields<LootTableDef> lootFields() {
    return Fields<LootTableDef>()
        .add("id", &LootTableDef::id)
        .list("drops", &LootTableDef::drops,
              Fields<LootDrop>().add("item", &LootDrop::item).add("weight", &LootDrop::weight).optional());
}

constexpr std::string_view items = R"([
    {"id": "sword", "name": "Sword", "damage": 12, "rarity": "rare"},
    {"id": "torch", "name": "Torch"}
])";

TEST_CASE("DefinitionTable: entries") {
    DefinitionTable<ItemDef> table(itemFields());
    const DefinitionTableResult result = table.parse(items, "items.json");
    CHECK_MESSAGE(result.success, ("a valid table loads: " + result.error));
    CHECK_MESSAGE((table.size() == 2 && table.entries()[0].id == "sword" && table.entries()[1].id == "torch"),
                  "entries keep file order");
    const ItemDef* sword = table.find("sword");
    CHECK_MESSAGE((sword != nullptr && sword->damage == 12.0f && sword->rarity == Rarity::Rare),
                  "entries are found by id");
    const ItemDef* torch = table.find("torch");
    CHECK_MESSAGE((torch != nullptr && torch->damage == 0.0f && torch->rarity == Rarity::Common),
                  "optional fields keep their defaults");
    CHECK_MESSAGE(table.find("axe") == nullptr, "an unknown id finds nothing");

    DefinitionTable<LootTableDef> loot(lootFields());
    CHECK_MESSAGE(
        loot.parse(R"([{"id": "crypt", "drops": [{"item": "sword", "weight": 2}, {"item": "torch"}]}])", "loot.json")
            .success,
        "a table with a list loads");
    const LootTableDef* crypt = loot.find("crypt");
    CHECK_MESSAGE((crypt != nullptr && crypt->drops.size() == 2 && crypt->drops[0].weight == 2.0f &&
                   crypt->drops[1].item == "torch" && crypt->drops[1].weight == 1.0f),
                  "list entries are read with their defaults");
}

std::string itemError(std::string_view text) {
    DefinitionTable<ItemDef> table(itemFields());
    return table.parse(text, "items.json").error;
}

std::string lootError(std::string_view text) {
    DefinitionTable<LootTableDef> table(lootFields());
    return table.parse(text, "loot.json").error;
}

TEST_CASE("DefinitionTable: errors") {
    const std::string unknown =
        itemError(R"([{"id": "sword", "name": "Sword"}, {"id": "axe", "name": "Axe", "dmg": 3}])");
    CHECK_MESSAGE(unknown == "items.json[1] 'axe': unknown field 'dmg'",
                  ("an unknown key names the file, the entry, and the field: " + unknown));
    const std::string nested = lootError(R"([{"id": "crypt", "drops": [{"item": "a"}, {"item": "b", "wieght": 2}]}])");
    CHECK_MESSAGE(nested == "loot.json[0] 'crypt': unknown field 'drops[1].wieght'",
                  ("an unknown key inside a list names its path: " + nested));
    const std::string wrongType = itemError(R"([{"id": "sword", "name": "Sword", "damage": "high"}])");
    CHECK_MESSAGE((contains(wrongType, "items.json[0] 'sword': ") && contains(wrongType, "'damage' must be a number")),
                  ("a wrong type is reported: " + wrongType));
    const std::string missing = itemError(R"([{"id": "sword"}])");
    CHECK_MESSAGE((contains(missing, "items.json[0] 'sword': ") && contains(missing, "Missing required field 'name'")),
                  ("a missing required field is reported: " + missing));
    const std::string noId = itemError(R"([{"name": "Nameless"}])");
    CHECK_MESSAGE((contains(noId, "items.json[0]: ") && contains(noId, "'id'")), ("a missing id is reported: " + noId));
    const std::string emptyId = itemError(R"([{"id": "", "name": "Blank"}])");
    CHECK_MESSAGE(contains(emptyId, "items.json[0]: the id 'id' is empty"), ("an empty id is reported: " + emptyId));
    const std::string duplicate =
        itemError(R"([{"id": "sword", "name": "A"}, {"id": "torch", "name": "B"}, {"id": "sword", "name": "C"}])");
    CHECK_MESSAGE(duplicate == "items.json[2] 'sword': duplicate id, also used by entry 0",
                  ("a duplicate id names both entries: " + duplicate));
    CHECK_MESSAGE(contains(itemError(R"({"id": "sword"})"), "items.json: expected an array of entries"),
                  "a file that is not an array is rejected");
    CHECK_MESSAGE(contains(itemError(R"([42])"), "items.json[0]: an entry must be an object"),
                  "an entry that is not an object is rejected");
    CHECK_MESSAGE(contains(itemError(R"([{"id": "sword",])"), "items.json: not valid JSON"), "broken JSON is rejected");

    DefinitionTable<ItemDef> noIdField(Fields<ItemDef>().add("name", &ItemDef::name));
    CHECK_MESSAGE(contains(noIdField.parse("[]", "items.json").error, "required string field 'id'"),
                  "a table needs a string id field");
}

TEST_CASE("DefinitionTable: failed load keeps entries") {
    DefinitionTable<ItemDef> table(itemFields());
    CHECK_MESSAGE(table.parse(items, "items.json").success, "the first load succeeds");
    CHECK_MESSAGE(!table.parse(R"([{"id": "axe"}])", "items.json").success, "a broken edit fails");
    CHECK_MESSAGE((table.size() == 2 && table.find("sword") != nullptr && table.find("axe") == nullptr),
                  "a failed load keeps the previous entries");
}

void write(const std::filesystem::path& path, std::string_view text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
}

TEST_CASE("DefinitionTable: files and reload") {
    const testsupport::TempDirectory directoryDirectory("zenith_definition_tables");
    const std::filesystem::path directory = directoryDirectory.path();
    const std::filesystem::path path = directory / "items.json";
    write(path, items);

    DefinitionTable<ItemDef> table(itemFields());
    CHECK_MESSAGE(!table.reloadIfChanged(), "a table that never loaded has nothing to reload");
    CHECK_MESSAGE((table.load(path).success && table.size() == 2), "a table loads from a file");
    CHECK_MESSAGE(!table.reloadIfChanged(), "an unchanged file is not reloaded");

    write(path, R"([{"id": "axe", "name": "Axe", "damage": 20}])");
    const std::optional<DefinitionTableResult> reloaded = table.reloadIfChanged();
    CHECK_MESSAGE((reloaded && reloaded->success && table.size() == 1 && table.find("axe") != nullptr),
                  "a changed file is reloaded");

    write(path, R"([{"id": "axe", "name": "Axe", "damage": "lots"}])");
    const std::optional<DefinitionTableResult> broken = table.reloadIfChanged();
    CHECK_MESSAGE((broken && !broken->success && contains(broken->error, "items.json[0] 'axe'") &&
                   table.find("axe")->damage == 20.0f),
                  "a broken edit is reported and the table keeps its entries");
    CHECK_MESSAGE(!table.reloadIfChanged(), "a broken file is not retried until it changes again");

    CHECK_MESSAGE(contains(table.load(directory / "missing.json").error, "missing.json: cannot be read"),
                  "a missing file is reported");
}
} // namespace

TEST_SUITE_END();
