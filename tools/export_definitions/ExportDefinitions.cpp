// Writes the module-definition (.def) file that exports a Windows DLL's
// symbols, from the COFF object files the DLL is linked from.
//
// Usage: zenith_export_definitions <output.def> <object-list>
// <object-list> names one object file per line.
//
// It exports the external functions and data the objects define once: those in
// ordinary sections and in no-duplicates COMDATs (a /Gy function). Inline
// functions, template instances, and other pick-any COMDATs are left out: every
// module that uses one compiles its own copy, and exporting them all (as
// CMake's WINDOWS_EXPORT_ALL_SYMBOLS does) overruns the 65535-member limit of
// an import library, mostly with standard library instances.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint16_t machineI386 = 0x14c;
constexpr std::uint32_t sectionComdat = 0x00001000;
constexpr std::uint32_t sectionExecutable = 0x20000000;
constexpr std::uint8_t storageExternal = 2;
constexpr std::uint8_t storageStatic = 3;
constexpr std::uint8_t comdatNoDuplicates = 1;
constexpr std::size_t sectionHeaderSize = 40;
constexpr std::size_t comdatSelectionOffset = 14;

// The class id of an ANON_OBJECT_HEADER_BIGOBJ (/bigobj) object.
constexpr std::array<std::uint8_t, 16> bigObjectClassId = {0xc7, 0xa1, 0xba, 0xd1, 0xee, 0xba, 0xa9, 0x4b,
                                                           0xaf, 0x20, 0xfa, 0xf6, 0x6a, 0xa4, 0xdc, 0xb8};

struct ExportedSymbols {
    std::set<std::string> functions;
    std::set<std::string> data;
};

class ObjectReader {
  public:
    explicit ObjectReader(std::vector<std::uint8_t> bytes)
        : bytes_(std::move(bytes)) {}

    template <typename T> T read(std::size_t offset) const {
        if (offset + sizeof(T) > bytes_.size()) {
            throw std::runtime_error("truncated object file");
        }
        T value{};
        std::memcpy(&value, bytes_.data() + offset, sizeof(T));
        return value;
    }

    std::string_view text(std::size_t offset, std::size_t maximumLength) const {
        if (offset >= bytes_.size()) {
            throw std::runtime_error("truncated object file");
        }
        const auto* start = reinterpret_cast<const char*>(bytes_.data() + offset);
        const std::size_t available = std::min(maximumLength, bytes_.size() - offset);
        return {start, strnlen(start, available)};
    }

    bool matches(std::size_t offset, const std::array<std::uint8_t, 16>& expected) const {
        return offset + expected.size() <= bytes_.size() &&
               std::memcmp(bytes_.data() + offset, expected.data(), expected.size()) == 0;
    }

  private:
    std::vector<std::uint8_t> bytes_;
};

// The layout of a regular (IMAGE_FILE_HEADER) or /bigobj object file.
struct ObjectLayout {
    std::uint16_t machine = 0;
    std::size_t sectionTable = 0;
    std::uint32_t sectionCount = 0;
    std::size_t symbolTable = 0;
    std::uint32_t symbolCount = 0;
    std::size_t symbolSize = 0;
};

ObjectLayout readLayout(const ObjectReader& object) {
    ObjectLayout layout;
    const auto signature1 = object.read<std::uint16_t>(0);
    const auto signature2 = object.read<std::uint16_t>(2);
    if (signature1 == 0 && signature2 == 0xffff) {
        if (object.read<std::uint16_t>(4) < 2 || !object.matches(12, bigObjectClassId)) {
            throw std::runtime_error("not a COFF object (compiled with /GL?)");
        }
        layout.machine = object.read<std::uint16_t>(6);
        layout.sectionTable = 56;
        layout.sectionCount = object.read<std::uint32_t>(44);
        layout.symbolTable = object.read<std::uint32_t>(48);
        layout.symbolCount = object.read<std::uint32_t>(52);
        layout.symbolSize = 20;
        return layout;
    }
    layout.machine = signature1;
    layout.sectionTable = 20 + object.read<std::uint16_t>(16);
    layout.sectionCount = signature2;
    layout.symbolTable = object.read<std::uint32_t>(8);
    layout.symbolCount = object.read<std::uint32_t>(12);
    layout.symbolSize = 18;
    return layout;
}

struct Symbol {
    std::string_view name;
    std::int32_t section = 0;
    std::uint8_t storageClass = 0;
    std::uint8_t auxiliaryCount = 0;
};

Symbol readSymbol(const ObjectReader& object, const ObjectLayout& layout, std::size_t offset) {
    const bool bigObject = layout.symbolSize == 20;
    Symbol symbol;
    if (object.read<std::uint32_t>(offset) == 0) {
        const std::size_t stringTable = layout.symbolTable + layout.symbolCount * layout.symbolSize;
        symbol.name = object.text(stringTable + object.read<std::uint32_t>(offset + 4), SIZE_MAX);
    } else {
        symbol.name = object.text(offset, 8);
    }
    symbol.section = bigObject ? object.read<std::int32_t>(12 + offset) : object.read<std::int16_t>(12 + offset);
    const std::size_t tail = offset + (bigObject ? 16 : 14);
    symbol.storageClass = object.read<std::uint8_t>(tail + 2);
    symbol.auxiliaryCount = object.read<std::uint8_t>(tail + 3);
    return symbol;
}

// Whether a symbol would be a scalar or vector deleting destructor, which the
// linker generates in every module (CMake leaves these out too).
bool isDeletingDestructor(std::string_view name) {
    return name.starts_with("??_G") || name.starts_with("??_E");
}

void collectExports(const ObjectReader& object, ExportedSymbols& exports) {
    const ObjectLayout layout = readLayout(object);
    std::vector<std::uint32_t> characteristics(layout.sectionCount);
    std::vector<std::optional<std::uint8_t>> comdatSelection(layout.sectionCount);
    for (std::uint32_t section = 0; section < layout.sectionCount; ++section) {
        characteristics[section] = object.read<std::uint32_t>(layout.sectionTable + section * sectionHeaderSize + 36);
    }

    // A COMDAT section's first symbol is its static section symbol, whose
    // auxiliary record holds the selection; externals in it follow.
    for (std::uint32_t index = 0; index < layout.symbolCount;) {
        const std::size_t offset = layout.symbolTable + index * layout.symbolSize;
        const Symbol symbol = readSymbol(object, layout, offset);
        const bool definedHere =
            symbol.section > 0 && static_cast<std::uint32_t>(symbol.section) <= layout.sectionCount;
        if (definedHere) {
            const std::size_t section = static_cast<std::size_t>(symbol.section) - 1;
            const bool comdat = (characteristics[section] & sectionComdat) != 0;
            if (symbol.storageClass == storageStatic && symbol.auxiliaryCount > 0 && comdat &&
                !comdatSelection[section]) {
                comdatSelection[section] =
                    object.read<std::uint8_t>(offset + layout.symbolSize + comdatSelectionOffset);
            } else if (symbol.storageClass == storageExternal && !isDeletingDestructor(symbol.name) &&
                       (!comdat || comdatSelection[section] == comdatNoDuplicates)) {
                std::string_view name = symbol.name;
                if (layout.machine == machineI386 && name.starts_with('_')) {
                    name.remove_prefix(1);
                }
                auto& destination =
                    (characteristics[section] & sectionExecutable) != 0 ? exports.functions : exports.data;
                destination.emplace(name);
            }
        }
        index += 1 + symbol.auxiliaryCount;
    }
}

std::vector<std::uint8_t> readFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("cannot open " + path);
    }
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

} // namespace

int main(int argumentCount, char** arguments) {
    if (argumentCount != 3) {
        std::cerr << "usage: zenith_export_definitions <output.def> <object-list>\n";
        return 2;
    }
    ExportedSymbols exports;
    std::ifstream objectList(arguments[2]);
    if (!objectList) {
        std::cerr << "zenith_export_definitions: cannot open " << arguments[2] << '\n';
        return 1;
    }
    std::string objectPath;
    while (std::getline(objectList, objectPath)) {
        if (objectPath.empty()) {
            continue;
        }
        try {
            collectExports(ObjectReader(readFile(objectPath)), exports);
        } catch (const std::exception& error) {
            std::cerr << "zenith_export_definitions: " << objectPath << ": " << error.what() << '\n';
            return 1;
        }
    }

    std::ofstream definitions(arguments[1], std::ios::trunc);
    definitions << "EXPORTS\n";
    for (const std::string& name : exports.functions) {
        definitions << '\t' << name << '\n';
    }
    for (const std::string& name : exports.data) {
        definitions << '\t' << name << " DATA\n";
    }
    if (!definitions) {
        std::cerr << "zenith_export_definitions: cannot write " << arguments[1] << '\n';
        return 1;
    }
    return 0;
}
