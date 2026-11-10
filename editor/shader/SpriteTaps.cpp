// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "shader/SpriteTaps.h"

#include "pool/ShaderPool.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderVariantCompiler.h"
#include "subsystem/RenderSystem.h"

#include <algorithm>
#include <map>
#include <utility>

using namespace jasperengine;

int editor::countSpirvTextureReads(const ShaderBinSource& bytecode){
    if (!bytecode.data || bytecode.size < 20)
        return -1;
    const uint32_t* words = reinterpret_cast<const uint32_t*>(bytecode.data);
    const size_t count = bytecode.size / sizeof(uint32_t);
    int taps = 0;
    for (size_t i = 5; i < count;){
        const uint32_t opcode = words[i] & 0xFFFFu;
        const uint32_t length = words[i] >> 16;
        if (opcode >= 87 && opcode <= 96)
            taps++;
        i += length ? length : 1;
    }
    return taps;
}

namespace {

    // the fragment stage's reads of one variant (the renderer's inputs for a textured sprite)
    int spriteFragmentTaps(const std::vector<std::string>& features, MaterialBlendMode blend){
        std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get("library:sprite");
        if (!manifest)
            return -1;
        SubmeshShaderInputs inputs;
        inputs.texCoord1 = true;
        inputs.normal = true;
        inputs.tangent = true;
        inputs.vertexColor4 = true;
        inputs.textureRect = true;
        inputs.baseColorTexture = true;
        inputs.normalTexture = true;
        inputs.manifest = manifest;
        inputs.features = manifest->resolveFeatures(features);
        const std::string reference = ShaderReference::getColorVariantReference("library:sprite", features, blend);
        const uint16_t customId = ShaderPool::registerCustomShader(reference);
        const SubmeshShaderVariant variant = RenderSystem::getSubmeshShaderVariant(MeshShaderEnvironment(), inputs);
        const ShaderKey key = ShaderPool::getShaderKey(ShaderType::MESH, variant.properties, customId);
        const CustomShaderSources sources = ShaderVariantCompiler::resolveCustomShaderSources(reference, nullptr, ShaderType::MESH);
        ShaderData data;
        std::string error, log;
        if (!ShaderVariantCompiler::compile(key, ShaderBackend::Vulkan, &sources, data, &error, &log))
            return -1;
        for (const ShaderStage& stage : data.stages){
            if (stage.type == ShaderStageType::FRAGMENT)
                return editor::countSpirvTextureReads(stage.bytecode);
        }
        return -1;
    }

}

int editor::librarySpriteExtraTaps(const std::vector<std::string>& features, MaterialBlendMode blend){
    if (!ShaderVariantCompiler::isAvailable())
        return -1;
    static std::map<std::pair<std::vector<std::string>, int>, int> cache;
    const auto key = std::make_pair(features, (int)blend);
    auto found = cache.find(key);
    if (found != cache.end())
        return found->second;
    const int base = spriteFragmentTaps({}, blend);
    const int with = spriteFragmentTaps(features, blend);
    const int extra = (base < 0 || with < 0) ? -1 : std::max(with - base, 0);
    cache[key] = extra;
    return extra;
}
