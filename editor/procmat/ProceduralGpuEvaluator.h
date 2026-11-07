// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PROCEDURALGPUEVALUATOR_H
#define PROCEDURALGPUEVALUATOR_H

#include "PassPlanner.h"
#include "ProceduralEvaluator.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {
    class ObjectRender;
    class ShaderRender;
    class TextureRender;
}

namespace jasperengine::editor::procmat {

    // Tiers 1 and 2 of R8: the passes drawn on the editor's own context through sokol-gfx, into RGBA16F
    // buffers, then an RGBA8 quantize pass and a read back (R12). Which tier the context is, is the
    // caller's to say: a renderer's name decides that, not how the passes run.
    class ProceduralGpuEvaluator : public ProceduralEvaluator {
    public:
        explicit ProceduralGpuEvaluator(EvaluatorTier tier);
        ~ProceduralGpuEvaluator() override;

        EvaluatorTier tier() const override { return tierName; }
        std::string deviceName() const override;
        bool prepare(const PassPlan& plan, std::string& error) override;
        void setUniforms(uint32_t pass, const UniformBlock& block) override;
        bool advance(double budgetMs, EvaluationResult& result) override;
        void cancel() override;

        // The latest RGBA8 image of an output of the plan, for a preview that reads no pixels back: it
        // stays valid, and shows the previous image, until the next evaluation replaces it. Null before.
        TextureRender* outputTexture(const std::string& name) const;
        // That image, the caller's from now on (a thumbnail cache keeps it after the next plan): the
        // next evaluation draws into a new one
        std::unique_ptr<TextureRender> takeOutputTexture(const std::string& name);

        // How many passes were compiled since this evaluator was made: a uniform edit adds none (R7)
        uint64_t compileCount() const { return compiles; }

    private:
        struct Program;
        struct Target;
        struct Buffer;
        enum class Phase { Idle, Compile, Draw, Quantize, WaitCommit, ReadBack };

        bool compilePass(size_t pass, EvaluationResult& result);
        bool createProgram(Program& program, const std::string& fragment, const std::string& label, EvaluationResult& result);
        // false: not ready yet, come back on a later frame. A failure sets `failed`.
        bool drawStep(size_t step, EvaluationResult& result, bool& failed);
        bool drawQuantize(size_t output, EvaluationResult& result, bool& failed);
        bool generateMips(Target& target, EvaluationResult& result, bool& failed);
        bool drawInto(Program& program, TextureRender& target, int mip, const void* uniforms, size_t uniformBytes,
                      const std::vector<std::pair<std::pair<int, int>, TextureRender*>>& textures, bool floatTarget);
        Target* acquireTarget(Buffer& buffer, int size, bool mips, bool sampledByItself);
        void dropUnusedTargets();
        bool frameHasRoom(size_t uniformBytes);
        void fail(EvaluationResult& result, const std::string& error, int pass, const std::string& log);

        EvaluatorTier tierName;
        PassPlan plan;
        Phase phase = Phase::Idle;
        size_t nextPass = 0;
        size_t nextStep = 0;
        size_t nextOutput = 0;
        uint64_t drawnFrame = 0;
        uint64_t compiles = 0;
        // the frame's share of the budgets a skipped draw would otherwise silently lose
        uint64_t budgetFrame = ~uint64_t(0);
        uint64_t frameUniformBytes = 0;
        int frameDraws = 0;

        std::map<std::string, std::unique_ptr<Program>> programs;   // by fragment text: a rebuild keeps what did not change
        std::vector<Program*> passPrograms;
        // per pass and planned texture: {-1, -1} for one the compiler optimized out
        std::vector<std::vector<std::pair<int, int>>> passTextureSlots;
        std::unique_ptr<Program> quantize;
        std::unique_ptr<Program> mipBox;
        std::vector<std::unique_ptr<Buffer>> buffers;
        std::map<std::string, std::unique_ptr<TextureRender>> outputs;
    };

}

#endif
