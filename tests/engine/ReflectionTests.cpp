#include "engine/reflection/FieldJson.hpp"
#include "engine/reflection/Reflection.hpp"
#include "engine/scene/SceneEntityId.hpp"

#include "support/Doctest.hpp"

#include <nlohmann/json.hpp>

#include <limits>
#include <string>
#include <string_view>
#include <vector>

// Direct contract tests for reflected field descriptors and their JSON codec,
// which component types, operators, and settings documents all build on.

TEST_SUITE_BEGIN("Reflection");

namespace {

bool contains(const std::string& text, std::string_view fragment) {
    return text.find(fragment) != std::string::npos;
}

// Fields::build() consumes the builder; chained modifiers return an lvalue.
template <typename Object> std::vector<FieldDescriptor> built(Fields<Object>& fields) {
    return std::move(fields).build();
}

enum class Blend : uint8_t {
    Opaque = 0,
    Additive = 4,
};

struct Falloff {
    float radius{2.0f};
    bool smooth{false};
};

struct Sample {
    bool enabled{true};
    int32_t offset{-3};
    uint32_t count{7};
    float weight{0.25f};
    glm::vec3 direction{0.0f, 1.0f, 0.0f};
    glm::vec4 tint{1.0f, 0.5f, 0.25f, 1.0f};
    glm::uvec3 cells{2u, 3u, 4u};
    std::string name{"probe"};
    Blend blend{Blend::Additive};
    Blend packedBlend{Blend::Opaque};
    float minimum{0.5f};
    float maximum{1.5f};
    Falloff falloff;
    float note{9.0f};
};

std::vector<EnumOption> blendOptions() {
    return {{"opaque", "Opaque", static_cast<int64_t>(Blend::Opaque)},
            {"additive", "Additive", static_cast<int64_t>(Blend::Additive)}};
}

std::vector<FieldDescriptor> sampleFields() {
    return built(
        Fields<Sample>()
            .add("enabled", &Sample::enabled)
            .add("offset", &Sample::offset)
            .add("count", &Sample::count)
            .add("weight", &Sample::weight)
            .add("direction", &Sample::direction)
            .add("tint", &Sample::tint)
            .color()
            .add("cells", &Sample::cells)
            .add("name", &Sample::name)
            .add("blend", &Sample::blend)
            .options(blendOptions())
            .add("packed_blend", &Sample::packedBlend)
            .options(blendOptions(), EnumEncoding::Integer)
            .range("delay", &Sample::minimum, &Sample::maximum)
            .structure("falloff", &Sample::falloff,
                       Fields<Falloff>().add("radius", &Falloff::radius).add("smooth", &Falloff::smooth).optional())
            .add("note", &Sample::note)
            .optional());
}

nlohmann::json written(const Sample& sample, const std::vector<FieldDescriptor>& fields) {
    nlohmann::json output;
    writeReflectedFields(&sample, fields, output);
    return output;
}

// Reads `document` into a default Sample and returns the error.
std::string readInto(const nlohmann::json& document, Sample& sample) {
    return readReflectedFields(document, &sample, sampleFields());
}

TEST_CASE("Reflection: builder records presentation") {
    const std::vector<FieldDescriptor> fields = built(Fields<Sample>()
                                                          .add("weight", &Sample::weight)
                                                          .slider(0.0, 1.0)
                                                          .format("%.2f")
                                                          .tooltip("How much")
                                                          .readOnly()
                                                          .add("direction", &Sample::direction)
                                                          .label("Aim")
                                                          .drag(0.5f, -1.0)
                                                          .angle()
                                                          .hidden());
    CHECK_MESSAGE(fields.size() == 2, "builder: one descriptor per add()");
    const FieldPresentation& weight = fields[0].presentation;
    CHECK_MESSAGE((fields[0].kind == FieldKind::Float && weight.label == "Weight"),
                  "builder: kind and label come from the member and key");
    CHECK_MESSAGE((weight.widget == NumberWidget::Slider && weight.minimum == 0.0 && weight.maximum == 1.0 &&
                   weight.format == "%.2f" && weight.tooltip == "How much" && weight.readOnly),
                  "builder: modifiers apply to the most recent field");
    const FieldPresentation& direction = fields[1].presentation;
    CHECK_MESSAGE((direction.label == "Aim" && direction.widget == NumberWidget::Drag && direction.speed == 0.5f &&
                   direction.minimum == -1.0 && !direction.maximum && direction.angle && direction.hidden &&
                   !direction.readOnly),
                  "builder: a later field does not inherit earlier modifiers");
    CHECK_MESSAGE(
        (labelFromKey("").empty() && labelFromKey("x") == "X" && labelFromKey("cast_shadows") == "Cast shadows"),
        "labelFromKey: capitalizes and replaces underscores");
}

TEST_CASE("Reflection: round trip every kind") {
    const std::vector<FieldDescriptor> fields = sampleFields();
    CHECK_MESSAGE(validateFieldDescriptors(fields).empty(), "round trip: the sample descriptors are valid");

    Sample source;
    source.enabled = false;
    source.offset = -2'000'000'000;
    source.count = 4'000'000'000u;
    source.weight = 0.1f;
    source.direction = {0.25f, -0.5f, 0.75f};
    source.tint = {0.1f, 0.2f, 0.3f, 0.4f};
    source.cells = {9u, 8u, 7u};
    source.name = "renamed";
    source.blend = Blend::Opaque;
    source.packedBlend = Blend::Additive;
    source.minimum = -1.0f;
    source.maximum = 3.0f;
    source.falloff = {.radius = 6.5f, .smooth = true};
    source.note = 42.0f;

    const nlohmann::json document = written(source, fields);
    CHECK_MESSAGE(document["blend"] == "opaque", "round trip: name-encoded enums write their key");
    CHECK_MESSAGE(document["packed_blend"] == 4, "round trip: integer-encoded enums write their value");
    CHECK_MESSAGE(document["delay"] == nlohmann::json::array({-1.0f, 3.0f}),
                  "round trip: ranges write [minimum, maximum]");
    CHECK_MESSAGE((document["falloff"].is_object() && document["falloff"]["radius"] == 6.5f),
                  "round trip: structs write nested objects");
    CHECK_MESSAGE(document["weight"].get<float>() == 0.1f, "round trip: floats keep float precision");

    Sample restored{};
    CHECK_MESSAGE(readInto(document, restored).empty(), "round trip: a written document reads back");
    CHECK_MESSAGE((restored.enabled == source.enabled && restored.offset == source.offset &&
                   restored.count == source.count && restored.weight == source.weight &&
                   restored.direction == source.direction && restored.tint == source.tint &&
                   restored.cells == source.cells && restored.name == source.name && restored.blend == source.blend &&
                   restored.packedBlend == source.packedBlend && restored.minimum == source.minimum &&
                   restored.maximum == source.maximum && restored.falloff.radius == source.falloff.radius &&
                   restored.falloff.smooth == source.falloff.smooth && restored.note == source.note),
                  "round trip: every field kind restores its value");
    CHECK_MESSAGE(written(restored, fields) == document, "round trip: writing the restored object is stable");
}

TEST_CASE("Reflection: unknown enum value writes integer") {
    Sample sample;
    sample.blend = static_cast<Blend>(9);
    const nlohmann::json document = written(sample, sampleFields());
    CHECK_MESSAGE(document["blend"] == 9, "enum: a value without an option is written as its integer");
    Sample restored;
    const std::string error = readInto(document, restored);
    CHECK_MESSAGE((contains(error, "'blend'") && contains(error, "opaque") && contains(error, "additive")),
                  ("enum: reading an unlisted value names the accepted keys: " + error));
}

TEST_CASE("Reflection: optional and unknown keys") {
    const nlohmann::json full = written(Sample{}, sampleFields());

    nlohmann::json withoutOptional = full;
    withoutOptional.erase("note");
    withoutOptional["falloff"].erase("smooth");
    withoutOptional["future_field"] = "from a newer version";
    Sample sample;
    sample.note = -1.0f;
    sample.falloff.smooth = true;
    CHECK_MESSAGE(readInto(withoutOptional, sample).empty(),
                  "optional: missing optional fields and unknown keys are accepted");
    CHECK_MESSAGE((sample.note == -1.0f && sample.falloff.smooth),
                  "optional: missing optional fields keep the current value");

    nlohmann::json withoutRequired = full;
    withoutRequired.erase("weight");
    CHECK_MESSAGE(readInto(withoutRequired, sample) == "Missing required field 'weight'",
                  "optional: a missing required field is reported by key");

    nlohmann::json withoutNestedRequired = full;
    withoutNestedRequired["falloff"].erase("radius");
    CHECK_MESSAGE(readInto(withoutNestedRequired, sample) == "falloff: Missing required field 'radius'",
                  "optional: nested errors are prefixed with the struct key");

    CHECK_MESSAGE(!readInto(nlohmann::json::array(), sample).empty(), "optional: a non-object document is rejected");
}

// Each case replaces one field of a valid document with a wrong value.
TEST_CASE("Reflection: rejects mismatched values") {
    struct Case {
        const char* key;
        nlohmann::json value;
    };

    const Case cases[] = {
        {"enabled", 1},
        {"offset", 2.5},
        {"offset", int64_t{3'000'000'000}},
        {"offset", "3"},
        {"count", -1},
        {"count", uint64_t{5'000'000'000}},
        {"weight", "heavy"},
        {"direction", nlohmann::json::array({1.0, 2.0})},
        {"direction", nlohmann::json::array({1.0, "2", 3.0})},
        {"tint", nlohmann::json::array({1.0, 1.0, 1.0})},
        {"cells", nlohmann::json::array({1, -2, 3})},
        {"cells", nlohmann::json::array({1, 2, 3, 4})},
        {"cells", nlohmann::json::array({1, 2.5, 3})},
        {"name", 5},
        {"blend", 4},
        {"blend", "Additive"},
        {"packed_blend", "additive"},
        {"packed_blend", 3},
        {"delay", nlohmann::json::array({1.0})},
        {"delay", 1.0},
        {"falloff", nlohmann::json::array()},
    };
    const nlohmann::json valid = written(Sample{}, sampleFields());
    for (const Case& rejected : cases) {
        nlohmann::json document = valid;
        document[rejected.key] = rejected.value;
        Sample sample;
        const std::string error = readInto(document, sample);
        CHECK_MESSAGE(contains(error, rejected.key), (std::string("mismatch: ") + rejected.key + " = " +
                                                      rejected.value.dump() + " is rejected by key: " + error));
    }

    // Boundaries that must still be accepted.
    nlohmann::json limits = valid;
    limits["offset"] = std::numeric_limits<int32_t>::min();
    limits["count"] = std::numeric_limits<uint32_t>::max();
    limits["weight"] = 7;
    Sample sample;
    CHECK_MESSAGE((readInto(limits, sample).empty() && sample.offset == std::numeric_limits<int32_t>::min() &&
                   sample.count == std::numeric_limits<uint32_t>::max() && sample.weight == 7.0f),
                  "mismatch: 32-bit limits and integer floats are accepted");
}

struct Follower {
    SceneEntityId target;
    SceneEntityId fallback;
};

std::vector<FieldDescriptor> followerFields() {
    return built(Fields<Follower>().add("target", &Follower::target).add("fallback", &Follower::fallback));
}

// Entity references are written as scene documents write entity ids, and an
// id is kept whether or not any entity has it.
TEST_CASE("Reflection: entity references") {
    const std::vector<FieldDescriptor> fields = followerFields();
    CHECK_MESSAGE(fields[0].kind == FieldKind::EntityReference,
                  "entity reference: SceneEntityId members deduce the kind");
    CHECK_MESSAGE(validateFieldDescriptors(fields).empty(), "entity reference: the descriptors are valid");

    // An id no scene has (for example of a deleted entity) is a plain value here.
    const Follower source{.target = SceneEntityId{0xFFFF'FFFF'FFFF'FFF0ull}, .fallback = {}};
    nlohmann::json document;
    writeReflectedFields(&source, fields, document);
    CHECK_MESSAGE(document["target"] == uint64_t{0xFFFF'FFFF'FFFF'FFF0ull},
                  "entity reference: an id is written as a number");
    CHECK_MESSAGE(document["fallback"].is_null(), "entity reference: no reference is written as null");

    Follower restored{.target = SceneEntityId{1}, .fallback = SceneEntityId{2}};
    CHECK_MESSAGE(readReflectedFields(document, &restored, fields).empty(),
                  "entity reference: a written document reads back");
    CHECK_MESSAGE((restored.target == source.target && !restored.fallback),
                  "entity reference: missing-entity ids and null round-trip unchanged");

    for (const nlohmann::json& invalid :
         {nlohmann::json(-1), nlohmann::json(1.5), nlohmann::json("7"), nlohmann::json::array({1})}) {
        nlohmann::json rejected = document;
        rejected["target"] = invalid;
        Follower follower;
        const std::string error = readReflectedFields(rejected, &follower, fields);
        CHECK_MESSAGE((contains(error, "'target'") && contains(error, "entity id")),
                      ("entity reference: " + invalid.dump() + " is rejected: " + error));
    }

    std::vector<FieldDescriptor> colored = built(Fields<Follower>().add("target", &Follower::target).color());
    CHECK_MESSAGE(contains(validateFieldDescriptors(colored), "Entity reference field 'target'"),
                  "entity reference: color presentation is rejected");
}

struct Drop {
    std::string item;
    float weight{1.0f};
};

struct Loot {
    std::vector<Drop> drops;
};

std::vector<FieldDescriptor> lootFields() {
    return built(Fields<Loot>().list("drops", &Loot::drops,
                                     Fields<Drop>().add("item", &Drop::item).add("weight", &Drop::weight).optional()));
}

// A list of nested structs is an array of objects; reading it replaces the
// list, and every element starts from its defaults.
TEST_CASE("Reflection: lists") {
    const std::vector<FieldDescriptor> fields = lootFields();
    CHECK_MESSAGE((fields[0].kind == FieldKind::List && validateFieldDescriptors(fields).empty()),
                  "list: the field is valid");

    const Loot source{.drops = {{.item = "sword", .weight = 3.0f}, {.item = "coin", .weight = 0.5f}}};
    nlohmann::json document;
    writeReflectedFields(&source, fields, document);
    CHECK_MESSAGE(
        (document["drops"].is_array() && document["drops"].size() == 2 && document["drops"][1]["item"] == "coin"),
        "list: elements are written as an array of objects");

    Loot restored{.drops = {{.item = "old", .weight = 9.0f}, {}, {}}};
    document["drops"][1].erase("weight");
    CHECK_MESSAGE(readReflectedFields(document, &restored, fields).empty(), "list: a written list reads back");
    CHECK_MESSAGE((restored.drops.size() == 2 && restored.drops[0].item == "sword" &&
                   restored.drops[0].weight == 3.0f && restored.drops[1].weight == 1.0f),
                  "list: reading replaces the list, and a missing optional field keeps the element's default");

    nlohmann::json bad = document;
    bad["drops"][1]["item"] = 7;
    Loot rejected;
    CHECK_MESSAGE(contains(readReflectedFields(bad, &rejected, fields), "drops[1]: Field 'item' must be a string"),
                  "list: an element's error names its index");
    bad["drops"] = nlohmann::json::object();
    CHECK_MESSAGE(contains(readReflectedFields(bad, &rejected, fields), "'drops' must be an array of objects"),
                  "list: a non-array is rejected");

    std::vector<FieldDescriptor> duplicate = built(
        Fields<Loot>().list("drops", &Loot::drops, Fields<Drop>().add("item", &Drop::item).add("item", &Drop::item)));
    CHECK_MESSAGE(validateFieldDescriptors(duplicate) == "drops.Duplicate reflected field 'item'",
                  "list: element problems are prefixed with the list key");
}

TEST_CASE("Reflection: validate field descriptors") {
    std::vector<FieldDescriptor> emptyKey = built(Fields<Sample>().add("", &Sample::weight));
    CHECK_MESSAGE(contains(validateFieldDescriptors(emptyKey), "empty key"), "validate: an empty key is rejected");

    std::vector<FieldDescriptor> duplicate =
        built(Fields<Sample>().add("weight", &Sample::weight).add("weight", &Sample::note));
    CHECK_MESSAGE(contains(validateFieldDescriptors(duplicate), "Duplicate reflected field 'weight'"),
                  "validate: duplicate keys are rejected");

    std::vector<FieldDescriptor> noOptions = built(Fields<Sample>().add("blend", &Sample::blend));
    CHECK_MESSAGE(contains(validateFieldDescriptors(noOptions), "'blend' lists no options"),
                  "validate: an enum without options is rejected");

    std::vector<FieldDescriptor> nested = built(
        Fields<Sample>().structure("falloff", &Sample::falloff,
                                   Fields<Falloff>().add("radius", &Falloff::radius).add("radius", &Falloff::radius)));
    CHECK_MESSAGE(validateFieldDescriptors(nested) == "falloff.Duplicate reflected field 'radius'",
                  "validate: nested problems are prefixed with the struct key");

    std::vector<FieldDescriptor> sameKeyInDifferentStructs =
        built(Fields<Sample>()
                  .add("radius", &Sample::weight)
                  .structure("falloff", &Sample::falloff, Fields<Falloff>().add("radius", &Falloff::radius)));
    CHECK_MESSAGE(validateFieldDescriptors(sameKeyInDifferentStructs).empty(),
                  "validate: keys only need to be unique within one struct");
}

TEST_CASE("Reflection: asset kind hint") {
    std::vector<FieldDescriptor> fields =
        built(Fields<Sample>().add("name", &Sample::name).asset("texture").add("weight", &Sample::weight));
    CHECK_MESSAGE(fields[0].presentation.assetKind == "texture",
                  "asset() should record the asset kind on the string field");
    CHECK_MESSAGE(fields[1].presentation.assetKind.empty(), "other fields should name no asset kind");
    CHECK_MESSAGE(validateFieldDescriptors(fields).empty(), "a string naming an asset kind should validate");
    CHECK_MESSAGE(!fields[0].presentation.readOnly, "naming an asset kind should leave the field editable");

    std::vector<FieldDescriptor> notString = built(Fields<Sample>().add("weight", &Sample::weight).asset("audio"));
    CHECK_MESSAGE(contains(validateFieldDescriptors(notString), "names an asset kind but is not a string"),
                  "validate: only string fields may name an asset kind");
}
} // namespace

TEST_SUITE_END();
