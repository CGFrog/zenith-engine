#include "ModuleShadowCopy.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <system_error>
#include <utility>
#include <vector>

namespace {
// The CodeView record of a Portable Executable image: where the null-terminated
// program database path is stored in the file, and how many bytes it may use.
struct CodeViewPath {
    size_t offset{0};
    size_t capacity{0};
    std::string path;
};

std::optional<uint32_t> readUInt32(const std::vector<char>& bytes, size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4) {
        return std::nullopt;
    }
    uint32_t value = 0;
    for (size_t index = 0; index < 4; ++index) {
        value |= static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset + index])) << (8 * index);
    }
    return value;
}

std::optional<uint16_t> readUInt16(const std::vector<char>& bytes, size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 2) {
        return std::nullopt;
    }
    return static_cast<uint16_t>(static_cast<unsigned char>(bytes[offset]) |
                                 (static_cast<unsigned char>(bytes[offset + 1]) << 8));
}

// Maps a relative virtual address to a file offset through the section table.
std::optional<size_t> fileOffsetOf(const std::vector<char>& bytes, size_t sectionTable, uint16_t sectionCount,
                                   uint32_t relativeAddress) {
    constexpr size_t sectionHeaderSize = 40;
    for (uint16_t section = 0; section < sectionCount; ++section) {
        const size_t header = sectionTable + section * sectionHeaderSize;
        const auto virtualSize = readUInt32(bytes, header + 8);
        const auto virtualAddress = readUInt32(bytes, header + 12);
        const auto rawSize = readUInt32(bytes, header + 16);
        const auto rawOffset = readUInt32(bytes, header + 20);
        if (!virtualSize || !virtualAddress || !rawSize || !rawOffset) {
            return std::nullopt;
        }
        const uint32_t extent = std::max(*virtualSize, *rawSize);
        if (relativeAddress >= *virtualAddress && relativeAddress - *virtualAddress < extent) {
            return static_cast<size_t>(*rawOffset) + (relativeAddress - *virtualAddress);
        }
    }
    return std::nullopt;
}

// Finds the program database path a PE image records, or nullopt when the
// image is not a PE file or records none (ELF libraries, release builds
// without debug information).
std::optional<CodeViewPath> findCodeViewPath(const std::vector<char>& bytes) {
    if (bytes.size() < 64 || bytes[0] != 'M' || bytes[1] != 'Z') {
        return std::nullopt;
    }
    const auto peHeader = readUInt32(bytes, 0x3c);
    if (!peHeader || bytes.size() < static_cast<size_t>(*peHeader) + 24 ||
        std::memcmp(bytes.data() + *peHeader, "PE\0\0", 4) != 0) {
        return std::nullopt;
    }
    const size_t fileHeader = static_cast<size_t>(*peHeader) + 4;
    const auto sectionCount = readUInt16(bytes, fileHeader + 2);
    const auto optionalHeaderSize = readUInt16(bytes, fileHeader + 16);
    const size_t optionalHeader = fileHeader + 20;
    const auto magic = readUInt16(bytes, optionalHeader);
    if (!sectionCount || !optionalHeaderSize || !magic) {
        return std::nullopt;
    }

    constexpr uint16_t pe32Magic = 0x10b;
    constexpr uint16_t pe32PlusMagic = 0x20b;
    size_t directoryCountOffset = 0;
    if (*magic == pe32Magic) {
        directoryCountOffset = 92;
    } else if (*magic == pe32PlusMagic) {
        directoryCountOffset = 108;
    } else {
        return std::nullopt;
    }
    constexpr uint32_t debugDirectoryIndex = 6;
    const auto directoryCount = readUInt32(bytes, optionalHeader + directoryCountOffset);
    if (!directoryCount || *directoryCount <= debugDirectoryIndex) {
        return std::nullopt;
    }
    const size_t debugEntry = optionalHeader + directoryCountOffset + 4 + debugDirectoryIndex * 8;
    const auto debugAddress = readUInt32(bytes, debugEntry);
    const auto debugSize = readUInt32(bytes, debugEntry + 4);
    if (!debugAddress || !debugSize || *debugAddress == 0) {
        return std::nullopt;
    }
    const size_t sectionTable = optionalHeader + *optionalHeaderSize;
    const auto debugDirectory = fileOffsetOf(bytes, sectionTable, *sectionCount, *debugAddress);
    if (!debugDirectory) {
        return std::nullopt;
    }

    constexpr size_t debugDirectorySize = 28;
    constexpr uint32_t codeViewType = 2;
    constexpr size_t rsdsHeaderSize = 24; // signature, GUID, age
    for (size_t entry = 0; entry + debugDirectorySize <= *debugSize; entry += debugDirectorySize) {
        const size_t directory = *debugDirectory + entry;
        const auto type = readUInt32(bytes, directory + 12);
        const auto dataSize = readUInt32(bytes, directory + 16);
        const auto dataOffset = readUInt32(bytes, directory + 24);
        if (!type || !dataSize || !dataOffset) {
            return std::nullopt;
        }
        if (*type != codeViewType || *dataSize <= rsdsHeaderSize ||
            static_cast<size_t>(*dataOffset) + *dataSize > bytes.size() ||
            std::memcmp(bytes.data() + *dataOffset, "RSDS", 4) != 0) {
            continue;
        }
        CodeViewPath record;
        record.offset = static_cast<size_t>(*dataOffset) + rsdsHeaderSize;
        record.capacity = *dataSize - rsdsHeaderSize;
        const char* begin = bytes.data() + record.offset;
        record.path.assign(begin, std::find(begin, begin + record.capacity, '\0'));
        return record;
    }
    return std::nullopt;
}

std::string uniqueSuffix(std::random_device& random) {
    const uint64_t value = (static_cast<uint64_t>(random()) << 32) ^ random();
    constexpr std::array<char, 16> digits{'0', '1', '2', '3', '4', '5', '6', '7',
                                          '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string suffix(16, '0');
    for (size_t index = 0; index < suffix.size(); ++index) {
        suffix[suffix.size() - 1 - index] = digits[(value >> (4 * index)) & 0xf];
    }
    return suffix;
}

#if defined(_WIN32)
// Whether name is "<stem>-<16 hex digits>", the form create() gives copies.
bool isShadowStemOf(const std::string& name, const std::string& stem) {
    constexpr size_t suffixLength = 16;
    if (name.size() != stem.size() + 1 + suffixLength || name.compare(0, stem.size(), stem) != 0 ||
        name[stem.size()] != '-') {
        return false;
    }
    return std::all_of(name.begin() + static_cast<std::ptrdiff_t>(stem.size() + 1), name.end(),
                       [](char digit) { return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f'); });
}
#endif

// Best effort: deletes copies of source left by processes that exited without
// unloading (or modules intentionally left loaded). Windows only: there a
// library still loaded by any process is locked against deletion, which tells
// stale copies from live ones. A program database is deleted only with its
// library.
void removeStaleCopies(const std::filesystem::path& source, const std::filesystem::path& directory) {
#if defined(_WIN32)
    const std::string stem = source.stem().string();
    std::error_code error;
    std::vector<std::filesystem::path> staleLibraries;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        const std::filesystem::path& path = entry.path();
        if (path.extension() == source.extension() && isShadowStemOf(path.stem().string(), stem)) {
            staleLibraries.push_back(path);
        }
    }
    for (const std::filesystem::path& library : staleLibraries) {
        if (std::filesystem::remove(library, error) && !error) {
            std::filesystem::path symbols = library;
            symbols.replace_extension(".pdb");
            std::filesystem::remove(symbols, error);
        }
    }
#else
    static_cast<void>(source);
    static_cast<void>(directory);
#endif
}

bool writeBytes(const std::filesystem::path& path, const std::vector<char>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}
} // namespace

ModuleShadowCopy::~ModuleShadowCopy() {
    remove();
}

ModuleShadowCopy::ModuleShadowCopy(ModuleShadowCopy&& other) noexcept
    : m_library(std::exchange(other.m_library, {}))
    , m_debugSymbols(std::exchange(other.m_debugSymbols, {})) {}

ModuleShadowCopy& ModuleShadowCopy::operator=(ModuleShadowCopy&& other) noexcept {
    if (this != &other) {
        remove();
        m_library = std::exchange(other.m_library, {});
        m_debugSymbols = std::exchange(other.m_debugSymbols, {});
    }
    return *this;
}

ModuleShadowCopyResult ModuleShadowCopy::create(const std::filesystem::path& source,
                                                const std::filesystem::path& directory, ModuleShadowCopy& copy) {
    std::ifstream input(source, std::ios::binary);
    if (!input) {
        return {.error = "Could not read game module '" + source.string() + "'"};
    }
    std::vector<char> bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad()) {
        return {.error = "Could not read game module '" + source.string() + "'"};
    }

    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        return {.error = "Could not create module shadow directory '" + directory.string() + "': " + error.message()};
    }

    removeStaleCopies(source, directory);

    std::random_device random;
    std::filesystem::path library;
    std::filesystem::path debugSymbolsName;
    constexpr int maximumAttempts = 16;
    for (int attempt = 0; attempt < maximumAttempts && library.empty(); ++attempt) {
        const std::string stem = source.stem().string() + "-" + uniqueSuffix(random);
        const std::filesystem::path candidate = directory / (stem + source.extension().string());
        if (!std::filesystem::exists(candidate, error) &&
            !std::filesystem::exists(directory / (stem + ".pdb"), error)) {
            library = candidate;
            debugSymbolsName = stem + ".pdb";
        }
    }
    if (library.empty()) {
        return {.error = "Could not choose a unique shadow name for game module '" + source.string() + "'"};
    }

    ModuleShadowCopy created;
    if (const std::optional<CodeViewPath> record = findCodeViewPath(bytes); record) {
        const std::filesystem::path recordedSymbols = record->path;
        // A bare file name, which a debugger looks up beside the loaded library.
        const std::string replacement = debugSymbolsName.string();
        if (std::filesystem::is_regular_file(recordedSymbols, error) && replacement.size() < record->capacity) {
            const std::filesystem::path copiedSymbols = directory / debugSymbolsName;
            if (std::filesystem::copy_file(recordedSymbols, copiedSymbols,
                                           std::filesystem::copy_options::overwrite_existing, error)) {
                created.m_debugSymbols = copiedSymbols;
                std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(record->offset), record->capacity, '\0');
                std::copy(replacement.begin(), replacement.end(),
                          bytes.begin() + static_cast<std::ptrdiff_t>(record->offset));
            }
        }
    }

    created.m_library = library;
    if (!writeBytes(library, bytes)) {
        return {.error = "Could not write module shadow copy '" + library.string() + "'"};
    }
    copy = std::move(created);
    return {.success = true};
}

const std::filesystem::path& ModuleShadowCopy::library() const {
    return m_library;
}

const std::filesystem::path& ModuleShadowCopy::debugSymbols() const {
    return m_debugSymbols;
}

void ModuleShadowCopy::keep() {
    m_library.clear();
    m_debugSymbols.clear();
}

void ModuleShadowCopy::remove() {
    std::error_code error;
    if (!m_library.empty()) {
        std::filesystem::remove(m_library, error);
    }
    if (!m_debugSymbols.empty()) {
        std::filesystem::remove(m_debugSymbols, error);
    }
    m_library.clear();
    m_debugSymbols.clear();
}
