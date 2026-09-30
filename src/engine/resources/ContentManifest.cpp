#include "ContentManifest.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <vector>

namespace {
using Json = nlohmann::json;

constexpr std::array<uint32_t, 64> constants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

std::string sha256(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(input), {});
    const uint64_t bitLength = static_cast<uint64_t>(bytes.size()) * 8;
    bytes.push_back(0x80);
    while (bytes.size() % 64 != 56) bytes.push_back(0);
    for (int shift = 56; shift >= 0; shift -= 8) bytes.push_back(static_cast<uint8_t>(bitLength >> shift));
    std::array<uint32_t, 8> hash = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    for (size_t offset = 0; offset < bytes.size(); offset += 64) {
        std::array<uint32_t, 64> words{};
        for (size_t index = 0; index < 16; ++index) {
            const size_t base = offset + index * 4;
            words[index] = (static_cast<uint32_t>(bytes[base]) << 24) |
                           (static_cast<uint32_t>(bytes[base + 1]) << 16) |
                           (static_cast<uint32_t>(bytes[base + 2]) << 8) | bytes[base + 3];
        }
        for (size_t index = 16; index < 64; ++index) {
            const uint32_t s0 = std::rotr(words[index - 15], 7) ^ std::rotr(words[index - 15], 18) ^
                                (words[index - 15] >> 3);
            const uint32_t s1 = std::rotr(words[index - 2], 17) ^ std::rotr(words[index - 2], 19) ^
                                (words[index - 2] >> 10);
            words[index] = words[index - 16] + s0 + words[index - 7] + s1;
        }
        auto [a, b, c, d, e, f, g, h] = hash;
        for (size_t index = 0; index < 64; ++index) {
            const uint32_t sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const uint32_t choice = (e & f) ^ (~e & g);
            const uint32_t temporary1 = h + sum1 + choice + constants[index] + words[index];
            const uint32_t sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temporary2 = sum0 + majority;
            h = g; g = f; f = e; e = d + temporary1; d = c; c = b; b = a; a = temporary1 + temporary2;
        }
        hash[0] += a; hash[1] += b; hash[2] += c; hash[3] += d;
        hash[4] += e; hash[5] += f; hash[6] += g; hash[7] += h;
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (uint32_t value : hash) output << std::setw(8) << value;
    return output.str();
}

bool contained(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    const std::filesystem::path relative = candidate.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}
}

ContentValidationResult validateContentManifest(const std::filesystem::path& packageRoot,
                                                const std::filesystem::path& manifestPath) {
    ContentValidationResult result;
    const std::filesystem::path root = std::filesystem::absolute(packageRoot).lexically_normal();
    const std::filesystem::path manifest = manifestPath.empty() ? root / "content-manifest.json" : manifestPath;
    std::ifstream input(manifest);
    if (!input) {
        result.issues.push_back({manifest, "Content manifest is missing"});
        return result;
    }
    try {
        Json document;
        input >> document;
        if (document.value("formatVersion", 0) != 1 || !document.contains("files") || !document["files"].is_array()) {
            result.issues.push_back({manifest, "Unsupported or malformed content manifest"});
            return result;
        }
        std::set<std::string> foldedPaths;
        for (const Json& entry : document["files"]) {
            const std::filesystem::path relative = entry.at("path").get<std::string>();
            const std::string expected = entry.at("sha256").get<std::string>();
            std::string folded = relative.generic_string();
            std::ranges::transform(folded, folded.begin(),
                                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
            if (relative.empty() || relative.is_absolute() || !foldedPaths.insert(folded).second) {
                result.issues.push_back({relative, "Manifest path is empty, absolute, duplicate, or case-conflicting"});
                continue;
            }
            const std::filesystem::path file = (root / relative).lexically_normal();
            if (!contained(root, file)) {
                result.issues.push_back({relative, "Manifest path escapes the package root"});
            } else if (!std::filesystem::is_regular_file(file)) {
                result.issues.push_back({relative, "Manifest file is missing"});
            } else if (sha256(file) != expected) {
                result.issues.push_back({relative, "Content digest does not match"});
            }
        }
    } catch (const Json::exception& error) {
        result.issues.push_back({manifest, std::string("Could not parse content manifest: ") + error.what()});
        return result;
    }
    result.success = result.issues.empty();
    return result;
}
