#include "editor/project/DerivedAssetInspector.hpp"
#include "engine/animation/AnimationSystem.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/assets/ImageDecoder.hpp"
#include "engine/assets/ProductionAssetPipeline.hpp"
#include "engine/input/InputProfiles.hpp"
#include "engine/platform/PlatformServices.hpp"
#include "engine/ui/Accessibility.hpp"
#include "engine/ui/Localization.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
int failures = 0;

class FakeMeshoptDecoder final : public CompressedMeshDecoder {
  public:
    CompressedMeshCodec codec() const override {
        return CompressedMeshCodec::Meshopt;
    }

    AssetPipelineResult decode(std::span<const uint8_t> input, std::vector<uint8_t>& output,
                               uint64_t maximumOutputBytes) const override {
        if (input.size() > maximumOutputBytes) {
            return {.error = "decoded output limit exceeded"};
        }
        output.assign(input.rbegin(), input.rend());
        return {.success = true};
    }
};

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void writeFile(const std::filesystem::path& path, std::string_view value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << value;
}
} // namespace

int main() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "zenith_milestone_19_21";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);

    expect(!validateAssetLimits({.vertices = 4'000'001}), "untrusted vertex counts are bounded before allocation");
    expect(!validateAssetLimits({.decodedBytes = 1024ull * 1024ull * 1024ull + 1}),
           "decoded sizes are bounded before allocation");
    CompressedMeshDecoderRegistry decoders;
    std::vector<uint8_t> decoded;
    const AssetPipelineResult missingDecoder = decoders.decode(CompressedMeshCodec::Draco, {}, decoded, 1024);
    expect(!missingDecoder && missingDecoder.error.find("optional decoder") != std::string::npos,
           "compressed meshes report a clear missing-decoder diagnostic");
    FakeMeshoptDecoder meshopt;
    decoders.registerDecoder(meshopt);
    const std::array<uint8_t, 3> encodedMesh{1, 2, 3};
    expect(decoders.decode(CompressedMeshCodec::Meshopt, encodedMesh, decoded, 3) && decoded[0] == 3,
           "optional compressed-mesh decoders are isolated behind a bounded interface");
    expect(selectKtx2TargetFormat({.bc7 = true}) == DerivedTextureFormat::Bc7 &&
               selectKtx2TargetFormat({}) == DerivedTextureFormat::Rgba8,
           "KTX2 target selection retains an uncompressed fallback");
    std::array<uint8_t, 80> invalidKtx{};
    expect(!validateKtx2(invalidKtx), "malformed KTX2 input is rejected");
    std::array<uint8_t, 108> rgbaKtx{};
    const std::array<uint8_t, 12> ktxIdentifier{0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
    std::copy(ktxIdentifier.begin(), ktxIdentifier.end(), rgbaKtx.begin());
    auto write32 = [&rgbaKtx](size_t offset, uint32_t value) {
        for (uint32_t byte = 0; byte < 4; ++byte) {
            rgbaKtx[offset + byte] = uint8_t(value >> (byte * 8));
        }
    };
    write32(12, 37);
    write32(20, 1);
    write32(24, 1);
    write32(40, 1);
    write32(80, 104);
    write32(88, 4);
    rgbaKtx[104] = 12;
    rgbaKtx[105] = 34;
    rgbaKtx[106] = 56;
    rgbaKtx[107] = 255;
    const ImageDecodeResult decodedKtx = decodeImageMemory(rgbaKtx.data(), rgbaKtx.size(), "RGBA fixture");
    expect(decodedKtx && decodedKtx.texture.rgba[2] == 56,
           "uncompressed KTX2 uses the deterministic RGBA8 fallback path");

#ifdef ZENITH_ASSET_DIR
    AssetManager importedAssets;
    const auto importedMorph =
        importedAssets.loadModel(std::filesystem::path(ZENITH_ASSET_DIR) / "morph_triangle.gltf");
    const ModelAsset* importedModel = importedAssets.model(importedMorph.handle);
    const MeshAsset* importedMesh =
        importedModel && !importedModel->nodes.empty() ? importedAssets.mesh(importedModel->nodes[0].mesh) : nullptr;
    expect(importedMorph && importedModel && importedMesh && importedMesh->morphTargets.size() == 1 &&
               importedModel->animations.size() == 1 &&
               importedModel->animations[0].channels[0].path == AnimationPath::Weights,
           "the glTF conformance fixture imports morph deltas and animation weights");
#endif

    const std::filesystem::path source = root / "source.bin";
    writeFile(source, "identical source content");
    const DerivedAssetRequest request{source, "quality=production", "zenith-ddc-1", "windows-x64"};
    DerivedAssetCache cache(root / "derived");
    std::string firstKey;
    std::string secondKey;
    expect(cache.key(request, firstKey) && cache.key(request, secondKey) && firstKey == secondKey,
           "identical source and settings produce the same derived-data key");
    const std::array<uint8_t, 4> payload{1, 2, 3, 4};
    DerivedAssetRecord firstRecord;
    DerivedAssetRecord secondRecord;
    expect(cache.store(request, payload, firstRecord) && cache.store(request, payload, secondRecord) &&
               firstRecord.key == secondRecord.key,
           "derived asset builds are byte-addressed and reproducible");
    DerivedAssetInspector inspector(root / "project");
    DerivedAssetRecord editorRecord;
    expect(inspector.rebuild(request, payload, editorRecord) && inspector.inspect(editorRecord.key, editorRecord) &&
               inspector.invalidate(editorRecord.key),
           "editor tooling can rebuild, inspect, and invalidate derived data outside source folders");

    ModelAsset morphModel;
    morphModel.nodes.push_back({.morphWeights = {0.0f, 0.0f, 0.0f}});
    AnimationClip morphClip{.name = "Expressions", .duration = 1.0f};
    AnimationChannel weights;
    weights.path = AnimationPath::Weights;
    weights.times = {0.0f, 1.0f};
    weights.weightValues = {{0.0f, 0.5f, 0.0f}, {1.0f, 0.0f, -0.25f}};
    morphClip.channels.push_back(weights);
    morphModel.animations.push_back(morphClip);
    const AnimationPose pose = evaluateAnimationClip(morphModel, 0, 0.5f, false);
    expect(pose[0].morphWeights.size() == 3 && pose[0].morphWeights[0] == 0.5f,
           "glTF animation weight channels interpolate morph weights");
    const auto active = selectActiveMorphTargets({0.1f, -0.9f, 0.2f, 0.8f}, 2);
    expect(active.size() == 2 && active[0].index == 1 && active[1].index == 3,
           "active morph targets are bounded by absolute influence with deterministic ordering");

    LocalizationCatalog catalog;
    expect(catalog.addLocale("en", {{"items", {.one = "{count} item", .other = "{count} items"}}}) &&
               catalog.addLocale("fr", {{"items", {.one = "{count} objet", .other = "{count} objets"}}}) &&
               catalog.selectLocale("fr-CA") && catalog.text("items", 2) == "2 objets",
           "stable message IDs support locale fallback and plural selection");
    expect(
        catalog.addLocale("ru", {{"items", {.one = "один", .few = "несколько", .many = "много", .other = "прочее"}}}) &&
            catalog.selectLocale("ru") && catalog.text("items", 3) == "несколько" &&
            catalog.text("items", 12) == "много",
        "locale-specific plural rules distinguish few and many forms");
    const std::array<FontHandle, 2> fonts{{{0, 1}, {1, 1}}};
    std::vector<ShapedGlyph> glyphs;
    expect(shapeUtf8("Aé", fonts, glyphs) && glyphs.size() == 2 && glyphs[1].font == fonts[1],
           "UTF-8 shaping chooses a fallback font for non-ASCII text");

    InputProfileSet profiles;
    expect(profiles.addProfile({.name = "Keyboard"}) && profiles.addProfile({.name = "Gamepad"}) &&
               profiles.select("Keyboard"),
           "multiple input profiles can be saved and selected");
    expect(static_cast<bool>(
               profiles.bind("Jump", {.source = Key::Space}, ButtonBehavior::Hold, BindingConflictPolicy::Reject)),
           "a profile accepts remapped keyboard controls");
    const auto conflict =
        profiles.bind("Interact", {.source = Key::Space}, ButtonBehavior::Toggle, BindingConflictPolicy::Reject);
    expect(!conflict && conflict.conflictingBinding == 0, "binding conflicts are reported without mutation");
    expect(static_cast<bool>(profiles.bind("Interact", {.source = Key::Space}, ButtonBehavior::Toggle,
                                           BindingConflictPolicy::ReplaceExisting)),
           "binding conflicts can be resolved explicitly and toggle behavior is preserved");
    InputProfileSet loadedProfiles;
    expect(saveInputProfiles(root / "profile" / "controls.json", profiles) &&
               loadInputProfiles(root / "profile" / "controls.json", loadedProfiles) &&
               loadedProfiles.selectedName() == "Keyboard" &&
               loadedProfiles.selected()->bindings[0].behavior == ButtonBehavior::Toggle,
           "remapped controls and hold/toggle alternatives persist across profiles");

    CaptionTrack captions;
    std::string captionError;
    expect(captions.setCues({{0.0, 1.0, "caption.door", "Guide"}, {0.5, 2.0, "caption.alarm", ""}}, captionError) &&
               captions.active(0.75).size() == 2,
           "timed captions support overlapping speech and sound cues");
    expect(makeAccessibleIndicator(1, "Danger").shape != makeAccessibleIndicator(2, "Safe").shape,
           "gameplay indicators convey categories independently of color");

    const std::filesystem::path settings = root / "profile" / "settings.json";
    const std::string settingsBytes = "version two";
    expect(static_cast<bool>(atomicWriteUserData(
               settings, {reinterpret_cast<const uint8_t*>(settingsBytes.data()), settingsBytes.size()})),
           "user data is written atomically in a writable profile directory");
    const std::string nextBytes = "next version";
    expect(static_cast<bool>(
               atomicWriteUserData(settings, {reinterpret_cast<const uint8_t*>(nextBytes.data()), nextBytes.size()})),
           "upgrades preserve a recovery backup");
    std::filesystem::remove(settings, error);
    expect(static_cast<bool>(recoverAtomicUserData(settings)),
           "missing user data recovers from the previous atomic version");

    DiagnosticBundleWriter diagnostics(root / "diagnostics");
    std::filesystem::path diagnosticPath;
    expect(!DiagnosticBundleWriter::automaticallyUploads() &&
               diagnostics.write({"2026-09-30T12-00-00Z", "0.1.0", "test failure", {{"scene", "demo"}}}, {},
                                 diagnosticPath) &&
               std::filesystem::exists(diagnosticPath),
           "structured diagnostics remain local unless application policy uploads them");
    const ReleaseProvenance provenance{1, "source-revision", "dependency-lock", "msvc", "manifest", "windows"};
    ReleaseProvenance loaded;
    expect(writeReleaseProvenance(root / "release" / "provenance.json", provenance) &&
               readReleaseProvenance(root / "release" / "provenance.json", loaded) &&
               loaded.sourceRevision == provenance.sourceRevision,
           "release provenance round-trips exact source, dependency, toolchain, and content revisions");

    std::filesystem::remove_all(root, error);
    return failures == 0 ? 0 : 1;
}
