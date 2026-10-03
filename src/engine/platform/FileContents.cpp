#include "engine/platform/FileContents.hpp"

#include <fstream>

namespace {
template <typename Container> FileReadStatus readWholeFile(const std::filesystem::path& path, Container& output) {
    output.clear();
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return FileReadStatus::OpenFailed;
    }
    const std::streamoff size = input.tellg();
    if (size < 0) {
        return FileReadStatus::ReadFailed;
    }
    output.resize(static_cast<size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(size));
    if (input.gcount() != static_cast<std::streamsize>(size)) {
        output.clear();
        return FileReadStatus::ReadFailed;
    }
    return FileReadStatus::Success;
}
} // namespace

FileReadStatus readFileBytes(const std::filesystem::path& path, std::vector<uint8_t>& output) {
    return readWholeFile(path, output);
}

FileReadStatus readFileText(const std::filesystem::path& path, std::string& output) {
    return readWholeFile(path, output);
}
