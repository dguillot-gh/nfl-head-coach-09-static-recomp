// Translates dumped Xenos shader microcode (HC09_DUMP_SHADERS) to HLSL files, without compiling them,
// for the offline SPIR-V pipeline (tools/shaders_to_spirv.py runs DXC on the output).
// Usage: XenosToHlsl <out folder> <file.vs.bin|file.ps.bin>...
// Writes <out>/<name>.hlsl per shader and prints one line per shader: name, ok/error, texture mask.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "gpu/hlsl_translator.h"
#include "gpu/ucode.h"

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: XenosToHlsl <out folder> <file.vs.bin|file.ps.bin>...\n");
        return 2;
    }
    const std::filesystem::path out = argv[1];
    std::filesystem::create_directories(out);
    int failed = 0;
    for (int arg = 2; arg < argc; arg++)
    {
        const std::filesystem::path path = argv[arg];
        // Dumps keep the console's byte order (big-endian).
        std::ifstream file(path, std::ios::binary);
        std::vector<uint32_t> dwords;
        uint32_t big;
        while (file.read(reinterpret_cast<char*>(&big), 4))
            dwords.push_back(__builtin_bswap32(big));
        const bool pixel = path.filename().string().find(".ps.") != std::string::npos;
        hc09::gpu::ShaderMicrocode shader{ pixel ? hc09::gpu::ShaderType::Pixel : hc09::gpu::ShaderType::Vertex, 0, dwords };
        const auto translated = hc09::gpu::TranslateShader(shader);
        const std::string name = path.stem().string();  // <hash>.vs / <hash>.ps
        if (!translated.valid)
        {
            std::printf("%s error %s\n", name.c_str(), translated.error.c_str());
            failed++;
            continue;
        }
        std::ofstream(out / (name + ".hlsl"), std::ios::binary) << translated.hlsl;
        std::printf("%s ok textures=0x%08X cube=0x%08X volume=0x%08X streams=%zu\n", name.c_str(), translated.textureFetchMask,
            translated.textureCubeMask, translated.textureVolumeMask, translated.vertexFetchConstants.size());
    }
    return failed ? 1 : 0;
}
