// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NOISE_NOISEPLAN_H
#define NOISE_NOISEPLAN_H

// The compiled form (T2 R17): a flat list of ops over numbered buffers. Point ops run chunk by chunk;
// a grid-only op is a stage of its own, running over the whole padded region between segments (R22).

#include "noise/NoiseGraphRuntime.h"
#include "noise/NoiseKernels.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine { namespace noise {

    inline constexpr size_t kChunkPoints = 4096;

    enum class OpKind : uint8_t {
        Fill, Variable, PositionField,
        Noise, Fbm, Ridged, DomainWarp, DomainTransform,
        ScaleOffset, MathOp, Clamp, Remap, Curve, Terrace, Select, Blend, HeightMask,
        TerrainHeight, SlopeMask, Hydraulic, Thermal,
        Output
    };

    enum class MathKind : uint8_t {
        Add, Subtract, Multiply, Divide, Min, Max, Power, Modulo, Abs, Negate, OneMinus, Sqrt, Floor, Frac, Sin, Cos
    };

    // Buffer numbers: the five domain arrays come first, then the scratch slots, then the region grids
    inline constexpr int32_t kBufX = 0, kBufY = 1, kBufZ = 2, kBufU = 3, kBufV = 4, kFirstSlot = 5;

    struct PlanOp {
        OpKind kind = OpKind::Fill;
        MathKind math = MathKind::Add;
        bool sanitize = false;      // R6: not proven finite, so the output is cleaned
        bool stage = false;         // runs over the whole region
        int node = -1;              // PlanData::nodes
        int level = 0;
        // buffers
        int32_t out[3] = {-1, -1, -1};
        int32_t in[4] = {-1, -1, -1, -1};
        // the same, as value numbers for the range analysis
        int32_t outValue[3] = {-1, -1, -1};
        int32_t inValue[4] = {-1, -1, -1, -1};
        // the meaning of these depends on kind; see NoiseCompiler.cpp's emit
        float f[10] = {};
        int i[3] = {};
        uint32_t seedOffset = 0;
        FractalParams fractal;
        int lut = -1;
    };

    struct PlanSegment {
        uint32_t begin = 0, end = 0;
        bool stage = false;
    };

    struct PlanOutput {
        OutputInfo info;
        int value = -1;
        int op = -1;
        int pad = 0;
        std::vector<uint32_t> ops;  // ascending: what this output needs
        bool usesStage = false;
    };

    struct PlanVariable {
        std::string name;
        float value = 0.0f;
        float min = 0.0f, max = 0.0f;
    };

    struct PlanData {
        std::vector<PlanOp> ops;
        std::vector<PlanSegment> segments;
        std::vector<int> opSegment;
        std::vector<PlanOutput> outputs;
        std::vector<OutputInfo> outputInfos;
        std::vector<CompiledNodeInfo> nodes;
        std::vector<std::vector<float>> luts;
        std::vector<PlanVariable> variables;
        std::vector<Interval> ranges;    // by value number
        int32_t firstGrid = kFirstSlot;  // the first buffer number that is a region grid
        int32_t bufferTotal = kFirstSlot;
        int gridCount = 0;
        std::vector<int> gridLastSegment;
        int slotCount = 0;
        int globalPad = 0;
        bool gridOnly = false;
        bool bakeOnly = false;
        bool readsHeight = false;
        bool readsPosition = false;
        bool readsUv = false;
    };

    // NoiseRanges.cpp: one op's outputs from its inputs' ranges (R19)
    void rangeOfOp(const PlanOp& op, const PlanData& plan, std::vector<Interval>& ranges);
    // NoiseEval.cpp: a one-point run of a point op, for constant folding
    float foldOp(const PlanOp& op, const PlanData& plan, const float* constants);

}}

#endif // NOISE_NOISEPLAN_H
