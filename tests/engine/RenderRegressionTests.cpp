#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>

namespace {
constexpr int width = 48;
constexpr int height = 32;
using Image = std::vector<uint8_t>;
int failures = 0;

uint8_t toByte(float value) {
    return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

uint64_t hash(const Image& image) {
    uint64_t value = 1469598103934665603ull;
    for (uint8_t byte : image) {
        value = (value ^ byte) * 1099511628211ull;
    }
    return value;
}

void writePpm(const std::filesystem::path& path, const Image& image) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "P6\n" << width << ' ' << height << "\n255\n";
    output.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
}

void compare(std::string_view name, const Image& image, uint64_t expected) {
    const uint64_t actual = hash(image);
    if (actual != expected) {
        const auto output =
            std::filesystem::temp_directory_path() / (std::string("zenith-") + std::string(name) + ".ppm");
        writePpm(output, image);
        std::cerr << "FAILED: " << name << " screenshot hash " << actual << " expected " << expected
                  << "; capture: " << output << '\n';
        ++failures;
    }
}

Image colorSpaceCapture() {
    Image image(width * height * 3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float linear = static_cast<float>(x) / (width - 1);
            const float srgb = linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
            const size_t pixel = static_cast<size_t>(y * width + x) * 3;
            image[pixel] = toByte(srgb);
            image[pixel + 1] = toByte(std::sqrt(linear));
            image[pixel + 2] = toByte(linear);
        }
    }
    return image;
}

Image shadowCapture() {
    Image image(width * height * 3);
    constexpr std::array<std::array<float, 3>, 3> colors{
        {{1.0f, 0.35f, 0.35f}, {0.35f, 1.0f, 0.35f}, {0.35f, 0.55f, 1.0f}}};
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int cascade = std::min(x / 16, 2);
            const float receiver = 0.35f + 0.65f * static_cast<float>(y) / (height - 1);
            const size_t pixel = static_cast<size_t>(y * width + x) * 3;
            for (int channel = 0; channel < 3; ++channel) {
                image[pixel + channel] = toByte(receiver * colors[cascade][channel]);
            }
        }
    }
    return image;
}

Image antialiasingCapture() {
    Image image(width * height * 3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float signedDistance = static_cast<float>(x) - (0.7f * y + 10.0f);
            const float coverage = std::clamp(0.5f - signedDistance, 0.0f, 1.0f);
            const uint8_t value = toByte(coverage);
            const size_t pixel = static_cast<size_t>(y * width + x) * 3;
            image[pixel] = value;
            image[pixel + 1] = value;
            image[pixel + 2] = value;
        }
    }
    return image;
}
} // namespace

int main() {
    compare("color-space", colorSpaceCapture(), 11920415357468837699ull);
    compare("shadow", shadowCapture(), 9332500078490701299ull);
    compare("antialiasing", antialiasingCapture(), 4834795595963545455ull);
    return failures == 0 ? 0 : 1;
}
