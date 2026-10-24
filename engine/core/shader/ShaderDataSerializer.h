// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SHADERDATASERIALIZER_H
#define SHADERDATASERIALIZER_H

#include <cstdint>
#include <string>
#include <vector>

#include "ShaderData.h"
#include "ShaderBuildTypes.h"

namespace jasperengine {

    class JASPERENGINE_API ShaderDataSerializer {
    public:
        // Simple on-disk cache format for ShaderData.
        // Stores stage source, SPIR-V bytecode, and reflection data, after the key of the
        // variant (ShaderPool::getStorageKey) a reader checks it against. A file in an
        // older format version (version 1: a packed 64-bit key with 32 property bits) is
        // rejected, so a cache of them is rebuilt rather than misread.
        //
        // Returns true on success. On failure, returns false and optionally fills err.
        static bool writeToFile(const std::string& filepath, ShaderKey shaderKey, const ShaderData& shaderData, std::string* err = nullptr);
        static bool writeToBytes(std::vector<unsigned char>& outBytes, ShaderKey shaderKey, const ShaderData& shaderData, std::string* err = nullptr);
        static bool writeJsonToFile(const std::string& filepath, ShaderKey shaderKey, const ShaderData& shaderData, std::string* err = nullptr);
        static bool readFromFile(const std::string& filepath, ShaderKey expectedShaderKey, ShaderData& outShaderData, std::string* err = nullptr);
        static bool readFromBytes(const std::vector<unsigned char>& bytes, ShaderKey expectedShaderKey, ShaderData& outShaderData, std::string* err = nullptr);

        // the format version written (and the only one read)
        static uint32_t getFormatVersion();
    };

}

#endif // SHADERDATASERIALIZER_H
