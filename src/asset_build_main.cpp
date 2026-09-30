#include "engine/assets/ProductionAssetPipeline.hpp"

#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 6) {
        std::cerr << "usage: zenith_asset_build <source> <derived-root> <settings> <tool-version> <platform>\n";
        return 2;
    }
    const DerivedAssetRequest request{argv[1], argv[3], argv[4], argv[5]};
    std::ifstream input(request.source, std::ios::binary);
    if (!input) {
        std::cerr << "could not open source asset: " << request.source << '\n';
        return 1;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    DerivedAssetCache cache(argv[2]);
    DerivedAssetRecord record;
    const AssetPipelineResult result = cache.store(request, bytes, record);
    if (!result) {
        std::cerr << result.error << '\n';
        return 1;
    }
    std::cout << record.key << ' ' << record.path.generic_string() << ' ' << record.bytes << '\n';
    return 0;
}
