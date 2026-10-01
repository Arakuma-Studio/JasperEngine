// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class ShaderHeaderBuilder {
    public:
        struct HeaderShader {
            std::string name;
            std::string encodedData;
        };

        static bool writeShaderHeader(const std::filesystem::path& outputPath, const std::string& suffix, const std::vector<HeaderShader>& shaders, std::string& err);

        // shaderlib.h, the engine shader library the runtime shader compiler builds from,
        // in the format engine/libs/shadercompiler/embed_shaderlib.cmake writes for
        // CMake builds (the Xcode workspace has no generation step)
        static std::string buildShaderLibHeader(const std::map<std::string, std::string>& sources);

    private:
        static std::string buildShaderHeader(const std::string& suffix, std::vector<HeaderShader> shaders);
    };

} // namespace jasperengine::editor
