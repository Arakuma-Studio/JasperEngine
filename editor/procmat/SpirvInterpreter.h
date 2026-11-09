// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SPIRVINTERPRETER_H
#define SPIRVINTERPRETER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace jasperengine::editor::procmat {

    // The nearest half-precision value, as an RGBA16F target stores a float (S90 R12): 11 significant
    // bits, subnormals down to 2^-24, and what rounds past 65504 is infinity. NaN stays NaN.
    float roundedToHalf(float value);

    // An RGBA32F image with its box-filtered mip chain, what a shader samples on the CPU tier.
    class SpirvImage {
    public:
        SpirvImage(int width, int height, std::vector<float> rgba);
        // As a GPU tier's Buffer holds it: a mip chain only when the Buffer has one (one level samples
        // level 0 at any level of detail), and with halfPrecision every level stored as RGBA16F stores it
        SpirvImage(int width, int height, std::vector<float> rgba, bool mipChain, bool halfPrecision);

        int mipCount() const { return (int)mips.size(); }

        void sampleBilinear(float u, float v, int mip, float out[4]) const;

    private:
        struct Mip {
            int width = 0;
            int height = 0;
            std::vector<float> rgba;
        };
        std::vector<Mip> mips;
    };

    struct SpirvInstruction {
        uint32_t op = 0;
        uint32_t a = 0;
        uint32_t b = 0;
        uint32_t c = 0;
        uint32_t d = 0;
    };

    union SpirvRegister {
        float f;
        int32_t i;
        uint32_t u;
    };

    // A fragment shader's SPIR-V translated once to flat register code (R11). Everything that can
    // fail fails here: an instruction outside the covered set is an error at load, never per pixel.
    // A shader has no recursion, so every function and variable lives at a fixed register.
    class SpirvProgram {
    public:
        static std::unique_ptr<SpirvProgram> load(const uint32_t* words, size_t wordCount, std::string& error);

        size_t uniformWords() const { return uniformSize; }
        // The texture named in GLSL, -1 when the shader has none of that name
        int textureSlot(const std::string& name) const;
        int textureCount() const { return (int)textureNames.size(); }
        size_t instructionCount() const { return code.size(); }
        size_t registerCount() const { return initial.size(); }

    private:
        friend class SpirvMachine;
        friend class SpirvCompiler;

        std::vector<SpirvInstruction> code;
        std::vector<SpirvRegister> initial;
        std::vector<uint32_t> tables;
        std::unordered_map<std::string, int> textureNames;
        uint32_t entry = 0;
        uint32_t inputLoc = 0;
        uint32_t inputSize = 0;
        uint32_t outputLoc = 0;
        uint32_t outputSize = 0;
        uint32_t uniformLoc = 0;
        uint32_t uniformSize = 0;
        uint32_t maxCallDepth = 0;
    };

    // One thread's registers. Not shared: a pass runs one machine per worker over the same program.
    class SpirvMachine {
    public:
        explicit SpirvMachine(const SpirvProgram& program);

        // At most uniformWords() words, in the block's layout
        void setUniforms(const float* values, size_t count);
        void setTexture(int slot, const SpirvImage* image);

        enum class Result { Ok, TooLong };

        // Runs the shader for one fragment: uv is its first input, rgba what it wrote
        Result shade(float u, float v, float rgba[4]);

    private:
        Result execute();
        const SpirvProgram& program;
        std::vector<SpirvRegister> registers;
        std::vector<const SpirvImage*> textures;
        std::vector<uint32_t> callStack;
    };

}

#endif
