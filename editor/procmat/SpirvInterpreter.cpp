// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpirvInterpreter.h"

#include "SPIRV/GLSL.std.450.h"
#include "SPIRV/spirv.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <set>

using namespace jasperengine::editor::procmat;

namespace {

// A runaway loop stops a pixel instead of the whole bake: a real node runs far fewer backward jumps
const int64_t backwardJumpLimit = 1 << 22;
// Generous for any real shader (the library's largest has 5,259 registers), and low enough that a
// corrupt type size cannot ask for gigabytes
const uint64_t maxTypeSize = 1u << 20;
const uint64_t maxRegisters = 1u << 24;

#define OP_FAMILY(X, NAME) X(NAME##1) X(NAME##2) X(NAME##3) X(NAME##4)

// Every operation of the register code. In a family the number is the component count.
#define OP_LIST(X) \
    OP_FAMILY(X, Copy) X(CopyN) X(ClearN) \
    /* float, per component */ \
    OP_FAMILY(X, FAdd) OP_FAMILY(X, FSub) OP_FAMILY(X, FMul) OP_FAMILY(X, FDiv) OP_FAMILY(X, FMod) \
    OP_FAMILY(X, FMin) OP_FAMILY(X, FMax) OP_FAMILY(X, Step) \
    OP_FAMILY(X, FNeg) OP_FAMILY(X, FAbs) OP_FAMILY(X, Floor) OP_FAMILY(X, Ceil) OP_FAMILY(X, Fract) OP_FAMILY(X, Sqrt) \
    OP_FAMILY(X, FSign) OP_FAMILY(X, Trunc) OP_FAMILY(X, Round) \
    OP_FAMILY(X, Mix) OP_FAMILY(X, Clamp) OP_FAMILY(X, SmoothStep) \
    /* integer, per component */ \
    OP_FAMILY(X, IAdd) OP_FAMILY(X, ISub) OP_FAMILY(X, IMul) OP_FAMILY(X, UDiv) OP_FAMILY(X, SDiv) OP_FAMILY(X, UMod) \
    OP_FAMILY(X, SMod) OP_FAMILY(X, And) OP_FAMILY(X, Or) OP_FAMILY(X, Xor) OP_FAMILY(X, Shl) \
    OP_FAMILY(X, ShrL) OP_FAMILY(X, ShrA) OP_FAMILY(X, UMin) OP_FAMILY(X, UMax) OP_FAMILY(X, SMin) OP_FAMILY(X, SMax) \
    OP_FAMILY(X, SClamp) OP_FAMILY(X, INeg) OP_FAMILY(X, Not) OP_FAMILY(X, BoolNot) \
    OP_FAMILY(X, SAbs) OP_FAMILY(X, SSign) \
    /* comparisons */ \
    OP_FAMILY(X, FEq) OP_FAMILY(X, FNe) OP_FAMILY(X, FLt) OP_FAMILY(X, FLe) OP_FAMILY(X, FGt) OP_FAMILY(X, FGe) \
    OP_FAMILY(X, FUNe) \
    OP_FAMILY(X, IEq) OP_FAMILY(X, INe) OP_FAMILY(X, SLt) OP_FAMILY(X, SLe) OP_FAMILY(X, SGt) OP_FAMILY(X, SGe) \
    OP_FAMILY(X, ULt) OP_FAMILY(X, ULe) OP_FAMILY(X, UGt) OP_FAMILY(X, UGe) OP_FAMILY(X, IsNan) \
    /* conversions and selection */ \
    OP_FAMILY(X, F2S) OP_FAMILY(X, F2U) OP_FAMILY(X, S2F) OP_FAMILY(X, U2F) \
    OP_FAMILY(X, SelectS) OP_FAMILY(X, SelectV) X(SelectSN) OP_FAMILY(X, Any) OP_FAMILY(X, All) \
    /* vectors and matrices: these read their sources before they write */ \
    OP_FAMILY(X, Dot) OP_FAMILY(X, Length) OP_FAMILY(X, Normalize) X(Cross) X(Distance) \
    OP_FAMILY(X, Vts) X(VtsN) X(MatVec) X(VecMat) X(MatMat) X(Shuffle) X(ExtractDyn) X(InsertDyn) \
    /* pointers: a register holds a register number */ \
    X(PtrIndex) X(PtrIndexAdd) X(LoadInd) X(StoreInd) \
    /* the rest of GLSL.std.450, by function */ \
    X(Math1) X(Math2) \
    /* control */ \
    X(Jump) X(JumpIf) X(Call) X(Ret) X(Sample)

enum Op : uint32_t {
#define OP_ENUM(NAME) NAME,
    OP_LIST(OP_ENUM)
#undef OP_ENUM
};

typedef float (*Math1Function)(float);
typedef float (*Math2Function)(float, float);

float mathRadians(float x){ return x * 0.01745329251994f; }
float mathDegrees(float x){ return x * 57.29577951308f; }
float mathExp2(float x){ return std::exp2(x); }
float mathLog2(float x){ return std::log2(x); }
float mathSin(float x){ return std::sin(x); }
float mathCos(float x){ return std::cos(x); }
float mathTan(float x){ return std::tan(x); }
float mathAsin(float x){ return std::asin(x); }
float mathAcos(float x){ return std::acos(x); }
float mathAtan(float x){ return std::atan(x); }
float mathSinh(float x){ return std::sinh(x); }
float mathCosh(float x){ return std::cosh(x); }
float mathTanh(float x){ return std::tanh(x); }
float mathExp(float x){ return std::exp(x); }
float mathLog(float x){ return std::log(x); }
float mathAtan2(float y, float x){ return std::atan2(y, x); }
float mathPow(float x, float y){ return std::pow(x, y); }

const Math1Function math1Functions[] = {
    mathRadians, mathDegrees, mathSin, mathCos, mathTan, mathAsin, mathAcos, mathAtan, mathSinh, mathCosh, mathTanh,
    mathExp, mathLog, mathExp2, mathLog2,
};
const Math2Function math2Functions[] = {mathAtan2, mathPow};

// Past 2^23 a float is an integer already, and the comparison sends NaN there too. std::floor is
// a library call on baseline x86-64, and a node's code is largely floor, fract and mod.
// The result has x's sign, which keeps IEEE's -0 for ceil(-0.5): it decides atan(0, y) and 1/y.
inline float floorFast(float x){
    if (!(std::fabs(x) < 8388608.0f))
        return x;
    const float t = (float)(int32_t)x;
    return std::copysign(t > x ? t - 1.0f : t, x);
}
inline float ceilFast(float x){
    if (!(std::fabs(x) < 8388608.0f))
        return x;
    const float t = (float)(int32_t)x;
    return std::copysign(t < x ? t + 1.0f : t, x);
}
inline float truncFast(float x){
    return std::fabs(x) < 8388608.0f ? std::copysign((float)(int32_t)x, x) : x;
}
inline float glslMod(float x, float y){ return x - y * floorFast(x / y); }
// IEEE minNum and maxNum, as GPUs and llvmpipe implement them: a NaN operand loses. clamp() of a
// 0/0 is undefined in GLSL but not rare in the library (a smooth minimum with k = 0).
inline float glslMin(float x, float y){ return (y < x || x != x) ? y : x; }
inline float glslMax(float x, float y){ return (x < y || x != x) ? y : x; }
inline float glslClamp(float x, float lo, float hi){ return glslMin(glslMax(x, lo), hi); }
inline float glslSign(float x){ return x > 0.0f ? 1.0f : (x < 0.0f ? -1.0f : 0.0f); }
inline float glslSmoothStep(float e0, float e1, float x){
    const float t = glslClamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
// Out of range is undefined in GLSL and in C++; this is what x86 and the GPU tiers on it give
inline int32_t floatToInt(float x){
    if (x >= -2147483648.0f && x < 2147483648.0f)
        return (int32_t)x;
    return std::numeric_limits<int32_t>::min();
}
inline uint32_t floatToUint(float x){
    if (x >= 0.0f && x < 4294967296.0f)
        return (uint32_t)x;
    return 0u;
}
// Integer division by zero and INT_MIN / -1 trap on x86: undefined in GLSL, so they must not crash
inline int32_t safeSDiv(int32_t x, int32_t y){
    if (y == 0 || (x == std::numeric_limits<int32_t>::min() && y == -1))
        return 0;
    return x / y;
}
inline int32_t safeSMod(int32_t x, int32_t y){
    if (y == 0 || y == -1)
        return 0;
    const int32_t r = x % y;
    return (r != 0 && ((r < 0) != (y < 0))) ? r + y : r;
}
inline uint32_t safeUDiv(uint32_t x, uint32_t y){ return y == 0 ? 0u : x / y; }
inline uint32_t safeUMod(uint32_t x, uint32_t y){ return y == 0 ? 0u : x % y; }

struct IdType {
    spv::Op kind = spv::OpNop;
    uint32_t size = 0;        // in registers
    uint32_t element = 0;     // the component, column, element or pointee type
    uint32_t count = 0;       // components, columns or elements
    uint32_t elementSize = 0;
    std::vector<uint32_t> members;
    spv::StorageClass storage = spv::StorageClassMax;
};

struct RawInstruction {
    const uint32_t* words = nullptr;
    uint32_t count = 0;
    spv::Op op = spv::OpNop;
    uint32_t block = 0;
    uint32_t position = 0;
};

struct FunctionInfo {
    uint32_t id = 0;
    uint32_t returnType = 0;
    std::vector<uint32_t> params;
    std::vector<uint32_t> variables;
    std::vector<RawInstruction> body;
    uint32_t returnLoc = 0;
    uint32_t start = 0;
    std::vector<bool> paramWritten;
    std::set<uint32_t> callees;
};

// How a result id exists at run time
enum class IdKind : uint8_t { None, Type, Register, StaticPointer, DynamicPointer, Label, Function, ExtSet };

struct IdInfo {
    IdKind kind = IdKind::None;
    uint32_t type = 0;
    uint32_t loc = 0;       // the value's register; for a static pointer, the pointee's
    uint32_t pointee = 0;   // the type a pointer points at
    uint32_t root = 0;      // the variable a pointer is rooted at
};

}

namespace jasperengine::editor::procmat {

class SpirvCompiler {
public:
    SpirvCompiler(const uint32_t* words, size_t count, SpirvProgram& program) : words(words), wordCount(count), program(program) {}

    bool compile(std::string& error){
        if (!parseModule() || !allocateFunctions() || !validateFunctions() || !emitProgram() || !problem.empty() || !verifyCode()){
            error = problem;
            return false;
        }
        return true;
    }

private:
    const uint32_t* words;
    size_t wordCount;
    SpirvProgram& program;
    std::string problem;

    std::vector<IdType> types;
    std::vector<IdInfo> ids;
    std::map<uint32_t, FunctionInfo> functions;
    std::vector<uint32_t> functionOrder;
    uint32_t entryFunction = 0;
    uint32_t bound = 0;
    std::vector<uint32_t> privateInits;
    std::vector<uint32_t> resetEachPixel;       // what a shader can read before writing, between pixels
    std::map<uint32_t, std::string> names;
    std::set<uint32_t> constantIds;

    uint32_t allocate(uint32_t size){
        const uint32_t loc = (uint32_t)program.initial.size();
        if (loc + (uint64_t)size > maxRegisters){
            fail("a shader with too many registers");
            return 0;
        }
        SpirvRegister zero;
        zero.u = 0;
        program.initial.resize(program.initial.size() + size, zero);
        return loc;
    }

    bool fail(const std::string& message){
        if (problem.empty())
            problem = message;
        return false;
    }

    static std::string text(const uint32_t* start, uint32_t maxWords){
        std::string result;
        for (uint32_t i = 0; i < maxWords * 4; i++){
            const char c = (char)((start[i / 4] >> (8 * (i % 4))) & 0xff);
            if (c == 0)
                break;
            result += c;
        }
        return result;
    }

    // The words an instruction this interpreter supports has at least; 0 when it does not support it.
    // Checking this first means no later code reads an operand that is not there.
    static uint32_t minimumWords(spv::Op op){
        switch (op){
            case spv::OpNop: case spv::OpNoLine: case spv::OpFunctionEnd: case spv::OpReturn: case spv::OpUnreachable:
                return 1;
            case spv::OpCapability: case spv::OpExtension: case spv::OpSourceExtension: case spv::OpSourceContinued:
            case spv::OpModuleProcessed: case spv::OpTypeVoid: case spv::OpTypeBool: case spv::OpTypeSampler: case spv::OpTypeStruct:
            case spv::OpLabel: case spv::OpBranch: case spv::OpReturnValue:
                return 2;
            case spv::OpExtInstImport: case spv::OpMemoryModel: case spv::OpExecutionMode: case spv::OpSource: case spv::OpString:
            case spv::OpName: case spv::OpDecorate: case spv::OpTypeFloat: case spv::OpTypeFunction: case spv::OpTypeSampledImage:
            case spv::OpConstantTrue: case spv::OpConstantFalse: case spv::OpConstantNull: case spv::OpConstantComposite:
            case spv::OpUndef: case spv::OpFunctionParameter: case spv::OpStore: case spv::OpSelectionMerge:
            case spv::OpCompositeConstruct:
                return 3;
            case spv::OpEntryPoint: case spv::OpMemberName: case spv::OpMemberDecorate: case spv::OpLine: case spv::OpTypeInt:
            case spv::OpTypeVector: case spv::OpTypeMatrix: case spv::OpTypeArray: case spv::OpTypePointer: case spv::OpConstant:
            case spv::OpVariable: case spv::OpLoad: case spv::OpAccessChain: case spv::OpInBoundsAccessChain: case spv::OpFunctionCall:
            case spv::OpBranchConditional: case spv::OpLoopMerge: case spv::OpCopyObject: case spv::OpCopyLogical: case spv::OpBitcast:
            case spv::OpFNegate: case spv::OpSNegate: case spv::OpNot: case spv::OpLogicalNot: case spv::OpIsNan:
            case spv::OpConvertFToS: case spv::OpConvertFToU: case spv::OpConvertSToF: case spv::OpConvertUToF: case spv::OpAny: case spv::OpAll:
                return 4;
            case spv::OpFunction: case spv::OpExtInst: case spv::OpCompositeExtract: case spv::OpVectorShuffle: case spv::OpPhi:
            case spv::OpSampledImage: case spv::OpVectorExtractDynamic: case spv::OpFAdd: case spv::OpFSub: case spv::OpFMul:
            case spv::OpFDiv: case spv::OpFMod: case spv::OpIAdd: case spv::OpISub: case spv::OpIMul: case spv::OpUDiv: case spv::OpSDiv:
            case spv::OpUMod: case spv::OpSMod: case spv::OpBitwiseAnd: case spv::OpBitwiseOr: case spv::OpBitwiseXor:
            case spv::OpLogicalAnd: case spv::OpLogicalOr: case spv::OpLogicalEqual: case spv::OpLogicalNotEqual:
            case spv::OpShiftLeftLogical: case spv::OpShiftRightLogical: case spv::OpShiftRightArithmetic: case spv::OpFOrdEqual:
            case spv::OpFOrdNotEqual: case spv::OpFOrdLessThan: case spv::OpFOrdLessThanEqual: case spv::OpFOrdGreaterThan:
            case spv::OpFOrdGreaterThanEqual: case spv::OpFUnordNotEqual: case spv::OpIEqual: case spv::OpINotEqual: case spv::OpSLessThan:
            case spv::OpSLessThanEqual: case spv::OpSGreaterThan: case spv::OpSGreaterThanEqual: case spv::OpULessThan:
            case spv::OpULessThanEqual: case spv::OpUGreaterThan: case spv::OpUGreaterThanEqual: case spv::OpDot:
            case spv::OpVectorTimesScalar: case spv::OpMatrixTimesScalar: case spv::OpMatrixTimesVector: case spv::OpVectorTimesMatrix:
            case spv::OpMatrixTimesMatrix:
                return 5;
            case spv::OpCompositeInsert: case spv::OpVectorInsertDynamic: case spv::OpSelect:
                return 6;
            case spv::OpImageSampleExplicitLod:
                return 7;
            case spv::OpTypeImage:
                return 9;
            default:
                return 0;
        }
    }

    static bool isDeclarationWithoutResult(spv::Op op){
        switch (op){
            case spv::OpCapability: case spv::OpExtension: case spv::OpSourceExtension: case spv::OpSourceContinued:
            case spv::OpModuleProcessed: case spv::OpMemoryModel: case spv::OpExecutionMode: case spv::OpSource: case spv::OpString:
            case spv::OpName: case spv::OpDecorate: case spv::OpEntryPoint: case spv::OpMemberName: case spv::OpMemberDecorate:
            case spv::OpLine: case spv::OpNoLine: case spv::OpFunctionEnd: case spv::OpSelectionMerge: case spv::OpLoopMerge:
                return true;
            default:
                return !hasResult(op) && op != spv::OpLabel;
        }
    }

    static bool hasResultInWordOne(spv::Op op){
        switch (op){
            case spv::OpExtInstImport: case spv::OpLabel: case spv::OpTypeVoid: case spv::OpTypeBool: case spv::OpTypeFloat:
            case spv::OpTypeInt: case spv::OpTypeVector: case spv::OpTypeMatrix: case spv::OpTypeArray: case spv::OpTypeStruct:
            case spv::OpTypePointer: case spv::OpTypeFunction: case spv::OpTypeImage: case spv::OpTypeSampler: case spv::OpTypeSampledImage:
                return true;
            default:
                return false;
        }
    }

    bool isType(uint32_t id) const { return id < bound && types[id].kind != spv::OpNop; }

    bool parseModule(){
        if (wordCount < 5 || words[0] != spv::MagicNumber)
            return fail("not a SPIR-V module");
        bound = words[3];
        if (bound == 0 || bound > (1u << 24))
            return fail("id bound out of range");
        types.resize(bound);
        ids.resize(bound);
        FunctionInfo* function = nullptr;
        uint32_t block = 0;
        uint32_t position = 0;
        for (size_t i = 5; i < wordCount;){
            const uint32_t count = words[i] >> 16;
            const spv::Op op = (spv::Op)(words[i] & 0xffff);
            if (count == 0 || i + count > wordCount)
                return fail("truncated instruction");
            const uint32_t required = minimumWords(op);
            if (required == 0)
                return fail(std::string("unsupported SPIR-V instruction, opcode ") + std::to_string((int)op));
            if (count < required)
                return fail(std::string("malformed SPIR-V instruction, opcode ") + std::to_string((int)op));
            const uint32_t* w = words + i;
            if (!isDeclarationWithoutResult(op)){
                const uint32_t result = hasResultInWordOne(op) ? w[1] : w[2];
                if (result == 0 || result >= bound)
                    return fail("an id out of the module's bound");
            }
            if (!parseInstruction(op, w, count, function, block, position))
                return false;
            i += count;
        }
        if (function != nullptr)
            return fail("truncated module");
        if (entryFunction == 0)
            return fail("no fragment entry point");
        if (functions.find(entryFunction) == functions.end())
            return fail("the entry point names no function");
        if (!functions[entryFunction].params.empty())
            return fail("an entry point with parameters");
        return true;
    }

    bool parseInstruction(spv::Op op, const uint32_t* w, uint32_t count, FunctionInfo*& function, uint32_t& block, uint32_t& position){
        switch (op){
            case spv::OpCapability: {
                const uint32_t capability = w[1];
                if (capability != spv::CapabilityShader && capability != spv::CapabilityMatrix)
                    return fail(std::string("unsupported capability ") + std::to_string(capability));
                return true;
            }
            case spv::OpExtInstImport:
                if (text(w + 2, count - 2) != "GLSL.std.450")
                    return fail("unsupported extended instruction set " + text(w + 2, count - 2));
                ids[w[1]].kind = IdKind::ExtSet;
                return true;
            case spv::OpEntryPoint:
                if (w[1] != spv::ExecutionModelFragment)
                    return fail("not a fragment entry point");
                entryFunction = w[2];
                return true;
            case spv::OpMemoryModel: case spv::OpExecutionMode: case spv::OpSource: case spv::OpSourceExtension:
            case spv::OpString: case spv::OpMemberName: case spv::OpLine: case spv::OpNoLine: case spv::OpModuleProcessed:
            case spv::OpSourceContinued: case spv::OpExtension: case spv::OpSelectionMerge: case spv::OpLoopMerge: case spv::OpNop:
                return true;
            case spv::OpName:
                names[w[1]] = text(w + 2, count - 2);
                return true;
            case spv::OpDecorate:
                if (w[2] == spv::DecorationArrayStride && count > 3)
                    arrayStrides[w[1]] = w[3];
                return true;
            case spv::OpMemberDecorate:
                if (w[3] == spv::DecorationOffset && count > 4)
                    memberOffsets[{w[1], w[2]}] = w[4];
                return true;
            case spv::OpTypeVoid: case spv::OpTypeBool: case spv::OpTypeFloat: case spv::OpTypeInt: case spv::OpTypeVector:
            case spv::OpTypeMatrix: case spv::OpTypeArray: case spv::OpTypeStruct: case spv::OpTypePointer:
            case spv::OpTypeFunction: case spv::OpTypeImage: case spv::OpTypeSampler: case spv::OpTypeSampledImage:
                return parseType(op, w, count);
            case spv::OpConstant: case spv::OpConstantTrue: case spv::OpConstantFalse: case spv::OpConstantNull: case spv::OpConstantComposite: case spv::OpUndef:
                if (function == nullptr)
                    return parseConstant(op, w, count);
                break;
            case spv::OpVariable:
                if (!isType(w[1]) || types[w[1]].kind != spv::OpTypePointer)
                    return fail("a variable whose type is not a pointer");
                if (function == nullptr)
                    return parseGlobal(w, count);
                function->variables.push_back(w[2]);
                ids[w[2]].type = w[1];
                ids[w[2]].kind = IdKind::StaticPointer;
                ids[w[2]].pointee = types[w[1]].element;
                ids[w[2]].root = w[2];
                ids[w[2]].loc = allocate(types[types[w[1]].element].size);
                if (count > 4){
                    if (w[4] >= bound || constantIds.count(w[4]) == 0)
                        return fail("a variable initializer that is not a constant");
                    variableInit[w[2]] = w[4];
                }
                return true;
            case spv::OpFunction:
                if (!isType(w[1]))
                    return fail("a function with no return type");
                function = &functions[w[2]];
                function->id = w[2];
                function->returnType = w[1];
                functionOrder.push_back(w[2]);
                ids[w[2]].kind = IdKind::Function;
                position = 0;
                block = 0;
                return true;
            case spv::OpFunctionParameter:
                if (function == nullptr || !isType(w[1]))
                    return fail("a parameter outside a function, or with no type");
                function->params.push_back(w[2]);
                ids[w[2]].type = w[1];
                return true;
            case spv::OpFunctionEnd:
                if (function == nullptr)
                    return fail("OpFunctionEnd outside a function");
                function = nullptr;
                return true;
            case spv::OpLabel:
                block = w[1];
                ids[w[1]].kind = IdKind::Label;
                break;
            default:
                break;
        }
        if (function == nullptr)
            return fail(std::string("an instruction outside a function, opcode ") + std::to_string((int)op));
        RawInstruction raw;
        raw.words = w;
        raw.count = count;
        raw.op = op;
        raw.block = block;
        raw.position = position++;
        function->body.push_back(raw);
        return true;
    }

    bool parseType(spv::Op op, const uint32_t* w, uint32_t count){
        IdType& type = types[w[1]];
        if (type.kind != spv::OpNop)
            return fail("a type declared twice");
        ids[w[1]].kind = IdKind::Type;
        switch (op){
            case spv::OpTypeVoid:
                type.size = 0;
                break;
            case spv::OpTypeBool:
                type.size = 1;
                break;
            case spv::OpTypeFloat: case spv::OpTypeInt:
                if (w[2] != 32)
                    return fail("a type that is not 32 bits wide");
                type.size = 1;
                break;
            case spv::OpTypeVector:
                if (!isType(w[2]) || types[w[2]].size != 1 || types[w[2]].count != 0 || w[3] < 2 || w[3] > 4)
                    return fail("a vector that is not 2 to 4 scalars");
                type.element = w[2];
                type.count = w[3];
                type.elementSize = 1;
                type.size = type.count;
                break;
            case spv::OpTypeMatrix:
                if (!isType(w[2]) || types[w[2]].kind != spv::OpTypeVector || w[3] < 2 || w[3] > 4)
                    return fail("a matrix that is not 2 to 4 vectors");
                type.element = w[2];
                type.count = w[3];
                type.elementSize = types[w[2]].size;
                type.size = type.count * type.elementSize;
                break;
            case spv::OpTypeArray: {
                auto length = constantValues.find(w[3]);
                if (!isType(w[2]) || types[w[2]].size == 0 || length == constantValues.end() || length->second == 0
                        || (uint64_t)length->second * types[w[2]].size > maxTypeSize)
                    return fail("an array with no element type, no length or too many registers");
                type.element = w[2];
                type.count = length->second;
                type.elementSize = types[w[2]].size;
                type.size = type.count * type.elementSize;
                break;
            }
            case spv::OpTypeStruct: {
                uint64_t size = 0;
                for (uint32_t i = 2; i < count; i++){
                    if (!isType(w[i]))
                        return fail("a struct member with no type");
                    type.members.push_back(w[i]);
                    size += types[w[i]].size;
                }
                if (size > maxTypeSize)
                    return fail("a struct with too many registers");
                type.size = (uint32_t)size;
                break;
            }
            case spv::OpTypePointer:
                if (!isType(w[3]))
                    return fail("a pointer to no type");
                type.storage = (spv::StorageClass)w[2];
                type.element = w[3];
                break;
            case spv::OpTypeFunction:
                break;
            case spv::OpTypeImage:
                // R11's samplers are 2D: another kind of image would read the wrong coordinates
                if (w[3] != spv::Dim2D || w[5] != 0 || w[6] != 0)
                    return fail("an image that is not a plain 2D texture");
                type.size = 1; // the register holds the texture's slot
                break;
            case spv::OpTypeSampler:
                type.size = 1;
                break;
            case spv::OpTypeSampledImage:
                if (!isType(w[2]) || types[w[2]].kind != spv::OpTypeImage)
                    return fail("a sampled image of no image");
                type.size = 1;
                break;
            default:
                return fail("unsupported type");
        }
        type.kind = op;
        return true;
    }

    std::map<uint32_t, uint32_t> constantValues;
    std::map<uint32_t, uint32_t> variableInit;
    std::map<uint32_t, uint32_t> arrayStrides;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> memberOffsets;

    bool parseConstant(spv::Op op, const uint32_t* w, uint32_t count){
        const uint32_t type = w[1];
        const uint32_t id = w[2];
        if (!isType(type))
            return fail("a constant with no type");
        const uint32_t loc = allocate(types[type].size);
        if (!problem.empty())
            return false;
        ids[id].kind = IdKind::Register;
        ids[id].type = type;
        ids[id].loc = loc;
        constantIds.insert(id);
        switch (op){
            case spv::OpConstant:
                if (types[type].size != 1)
                    return fail("a scalar constant of another type");
                program.initial[loc].u = w[3];
                constantValues[id] = w[3];
                break;
            case spv::OpConstantTrue:
                if (types[type].size != 1)
                    return fail("a scalar constant of another type");
                program.initial[loc].u = 1;
                break;
            case spv::OpConstantFalse: case spv::OpConstantNull: case spv::OpUndef:
                break;
            case spv::OpConstantComposite: {
                uint32_t offset = loc;
                const uint32_t end = loc + types[type].size;
                for (uint32_t i = 3; i < count; i++){
                    if (w[i] >= bound || constantIds.count(w[i]) == 0)
                        return fail("a constant composite of something else");
                    const IdInfo& part = ids[w[i]];
                    const uint32_t size = types[part.type].size;
                    if (offset + (uint64_t)size > end)
                        return fail("a constant composite larger than its type");
                    for (uint32_t k = 0; k < size; k++)
                        program.initial[offset + k] = program.initial[part.loc + k];
                    offset += size;
                }
                if (offset != end)
                    return fail("a constant composite smaller than its type");
                break;
            }
            default:
                return fail("unsupported constant");
        }
        return true;
    }

    // The register layout is the type flattened, so a block must be declared with no padding:
    // anything else would put setUniforms' values in the wrong registers without a word.
    bool layoutIsFlat(uint32_t type){
        const IdType& t = types[type];
        if (t.kind == spv::OpTypeStruct){
            uint32_t offset = 0;
            for (uint32_t i = 0; i < t.members.size(); i++){
                auto declared = memberOffsets.find({type, i});
                if (declared != memberOffsets.end() && declared->second != offset * 4)
                    return false;
                if (!layoutIsFlat(t.members[i]))
                    return false;
                offset += types[t.members[i]].size;
            }
        } else if (t.kind == spv::OpTypeArray){
            auto stride = arrayStrides.find(type);
            if (stride != arrayStrides.end() && stride->second != t.elementSize * 4)
                return false;
            return layoutIsFlat(t.element);
        } else if (t.kind == spv::OpTypeMatrix){
            return false;
        }
        return true;
    }

    bool parseGlobal(const uint32_t* w, uint32_t count){
        const uint32_t pointerType = w[1];
        const uint32_t id = w[2];
        const spv::StorageClass storage = (spv::StorageClass)w[3];
        const uint32_t pointee = types[pointerType].element;
        IdInfo& info = ids[id];
        info.kind = IdKind::StaticPointer;
        info.type = pointerType;
        info.pointee = pointee;
        info.root = id;
        info.loc = allocate(types[pointee].size);
        if (!problem.empty())
            return false;
        switch (storage){
            case spv::StorageClassInput:
                if (program.inputSize != 0)
                    return fail("more than one fragment input");
                program.inputLoc = info.loc;
                program.inputSize = types[pointee].size;
                readOnlyRoots.insert(id);
                break;
            case spv::StorageClassOutput:
                if (program.outputSize != 0)
                    return fail("more than one fragment output");
                program.outputLoc = info.loc;
                program.outputSize = types[pointee].size;
                resetEachPixel.push_back(id);
                break;
            case spv::StorageClassUniform:
                if (program.uniformSize != 0)
                    return fail("more than one uniform block");
                if (!layoutIsFlat(pointee))
                    return fail("a uniform block with padding: only arrays of vec4 are laid out as the registers are");
                program.uniformLoc = info.loc;
                program.uniformSize = types[pointee].size;
                readOnlyRoots.insert(id);
                break;
            case spv::StorageClassUniformConstant:
                readOnlyRoots.insert(id);
                if (types[pointee].kind == spv::OpTypeImage){
                    const int slot = (int)program.textureNames.size();
                    program.textureNames[names[id]] = slot;
                    program.initial[info.loc].i = slot;
                } else if (types[pointee].kind != spv::OpTypeSampler){
                    return fail("unsupported resource type (a combined image sampler?)");
                }
                break;
            case spv::StorageClassPrivate:
                if (count > 4){
                    if (w[4] >= bound || constantIds.count(w[4]) == 0)
                        return fail("a variable initializer that is not a constant");
                    privateInits.push_back(id);
                    variableInit[id] = w[4];
                } else {
                    resetEachPixel.push_back(id);
                }
                break;
            default:
                return fail(std::string("unsupported storage class ") + std::to_string((int)storage));
        }
        return true;
    }

    std::set<uint32_t> readOnlyRoots;

    // -- function analysis ---------------------------------------------------------------------

    template <typename Visit>
    static void forEachOperand(const RawInstruction& raw, Visit visit){
        const uint32_t* w = raw.words;
        const uint32_t n = raw.count;
        switch (raw.op){
            case spv::OpLabel: case spv::OpSelectionMerge: case spv::OpLoopMerge: case spv::OpBranch:
            case spv::OpReturn: case spv::OpUnreachable: case spv::OpNop:
                break;
            case spv::OpBranchConditional: case spv::OpReturnValue:
                visit(w[1]);
                break;
            case spv::OpStore:
                visit(w[1]);
                visit(w[2]);
                break;
            case spv::OpLoad: case spv::OpCompositeExtract: case spv::OpCopyObject: case spv::OpCopyLogical:
                visit(w[3]);
                break;
            case spv::OpCompositeInsert:
                visit(w[3]);
                visit(w[4]);
                break;
            case spv::OpVectorShuffle:
                visit(w[3]);
                visit(w[4]);
                break;
            case spv::OpPhi:
                for (uint32_t i = 3; i + 1 < n; i += 2)
                    visit(w[i]);
                break;
            case spv::OpExtInst:
                for (uint32_t i = 5; i < n; i++)
                    visit(w[i]);
                break;
            case spv::OpFunctionCall:
                for (uint32_t i = 4; i < n; i++)
                    visit(w[i]);
                break;
            case spv::OpImageSampleExplicitLod:
                visit(w[3]);
                visit(w[4]);
                for (uint32_t i = 6; i < n; i++)
                    visit(w[i]);
                break;
            default:
                for (uint32_t i = 3; i < n; i++)
                    visit(w[i]);
                break;
        }
    }

    // The instructions in a function body that define no value
    static bool hasResult(spv::Op op){
        switch (op){
            case spv::OpStore: case spv::OpBranch: case spv::OpBranchConditional: case spv::OpReturn:
            case spv::OpReturnValue: case spv::OpUnreachable: case spv::OpNop: case spv::OpLabel:
                return false;
            default:
                return true;
        }
    }

    bool allocateFunctions(){
        // pointers resolve in instruction order; a parameter is copied in, so it behaves as a variable
        for (uint32_t functionId : functionOrder){
            FunctionInfo& function = functions[functionId];
            function.returnLoc = allocate(types[function.returnType].size);
            function.paramWritten.assign(function.params.size(), false);
            for (const RawInstruction& raw : function.body){
                if (raw.op == spv::OpFunctionCall)
                    function.callees.insert(raw.words[3]);
            }
        }
        std::set<uint32_t> finished;
        for (uint32_t functionId : functionOrder){
            std::set<uint32_t> visiting;
            if (!checkAcyclic(functionId, visiting, finished))
                return false;
        }
        for (uint32_t functionId : functionOrder){
            FunctionInfo& function = functions[functionId];
            for (size_t i = 0; i < function.params.size(); i++){
                const uint32_t param = function.params[i];
                IdInfo& info = ids[param];
                const uint32_t type = info.type;
                if (types[type].kind == spv::OpTypePointer){
                    info.kind = IdKind::StaticPointer;
                    info.pointee = types[type].element;
                    info.root = param;
                    info.loc = allocate(types[info.pointee].size);
                } else {
                    info.kind = IdKind::Register;
                    info.loc = allocate(types[type].size);
                }
            }
        }
        return true;
    }

    bool checkAcyclic(uint32_t functionId, std::set<uint32_t>& visiting, std::set<uint32_t>& finished){
        if (finished.count(functionId))
            return true;
        if (!visiting.insert(functionId).second)
            return fail("a recursive call");
        for (uint32_t callee : functions[functionId].callees){
            if (functions.find(callee) == functions.end())
                return fail("a call to an unknown function");
            if (!checkAcyclic(callee, visiting, finished))
                return false;
        }
        visiting.erase(functionId);
        finished.insert(functionId);
        return true;
    }

    // -- validation ----------------------------------------------------------------------------

    bool isPointerValue(uint32_t id) const { return types[ids[id].type].kind == spv::OpTypePointer; }

    // What the emitter assumes of a function body: terminated blocks, values defined before their use
    // (which also rules out a cycle of renames), operands of the shape the instruction needs.
    bool validateFunctions(){
        std::vector<bool> defined(bound, false);
        for (uint32_t id = 0; id < bound; id++)
            defined[id] = ids[id].kind == IdKind::Register || ids[id].kind == IdKind::StaticPointer;
        for (uint32_t functionId : functionOrder){
            const FunctionInfo& function = functions[functionId];
            if (function.body.empty())
                return fail("a function with no body");
            bool open = false;
            for (const RawInstruction& raw : function.body){
                if (raw.op == spv::OpLabel){
                    if (open)
                        return fail("a block with no terminator");
                    open = true;
                    continue;
                }
                if (!open)
                    return fail("an instruction outside a block");
                bool known = true;
                forEachOperand(raw, [&](uint32_t id){
                    if (id >= bound || (raw.op != spv::OpPhi && !defined[id]))
                        known = false;
                });
                if (!known)
                    return fail("an operand used before it is defined, or out of the module's bound");
                if (hasResult(raw.op)){
                    if (!isType(raw.words[1]))
                        return fail("a result with no type");
                    if (defined[raw.words[2]])
                        return fail("an id defined twice");
                    defined[raw.words[2]] = true;
                    ids[raw.words[2]].type = raw.words[1];
                }
                if (!validateInstruction(function, raw))
                    return false;
                open = !(raw.op == spv::OpBranch || raw.op == spv::OpBranchConditional || raw.op == spv::OpReturn
                         || raw.op == spv::OpReturnValue || raw.op == spv::OpUnreachable);
            }
            if (open)
                return fail("a block with no terminator");
        }
        return true;
    }

    bool validateInstruction(const FunctionInfo& function, const RawInstruction& raw){
        const uint32_t* w = raw.words;
        switch (raw.op){
            case spv::OpLoad:
                if (!isPointerValue(w[3]) || types[ids[w[3]].type].element != w[1])
                    return fail("a load through something that is not a pointer to its type");
                break;
            case spv::OpStore:
                if (!isPointerValue(w[1]) || types[ids[w[1]].type].element != ids[w[2]].type)
                    return fail("a store through something that is not a pointer to its type");
                break;
            case spv::OpAccessChain: case spv::OpInBoundsAccessChain:
                if (!isPointerValue(w[3]) || types[w[1]].kind != spv::OpTypePointer)
                    return fail("an access chain that is not on a pointer");
                break;
            case spv::OpFunctionCall: {
                auto callee = functions.find(w[3]);
                if (callee == functions.end() || callee->second.params.size() != raw.count - 4)
                    return fail("a call with the wrong number of arguments");
                for (uint32_t i = 4; i < raw.count; i++){
                    if (ids[w[i]].type != ids[callee->second.params[i - 4]].type)
                        return fail("a call with an argument of the wrong type");
                }
                break;
            }
            case spv::OpReturnValue:
                if (ids[w[1]].type != function.returnType)
                    return fail("a return value of the wrong type");
                break;
            case spv::OpVectorShuffle: {
                const uint32_t n = raw.count - 5;
                const IdType& left = types[ids[w[3]].type];
                const IdType& right = types[ids[w[4]].type];
                if (left.kind != spv::OpTypeVector || right.kind != spv::OpTypeVector || n < 1 || n > 4 || componentCount(w[1]) != n)
                    return fail("a shuffle that is not of vectors, or not of 1 to 4 components");
                for (uint32_t i = 5; i < raw.count; i++){
                    if (w[i] != 0xffffffffu && w[i] >= left.count + right.count)
                        return fail("a shuffle of a component that does not exist");
                }
                break;
            }
            case spv::OpCompositeConstruct: {
                uint64_t size = 0;
                for (uint32_t i = 3; i < raw.count; i++)
                    size += sizeOf(ids[w[i]].type);
                if (size != sizeOf(w[1]))
                    return fail("a composite built from parts of another size");
                break;
            }
            case spv::OpCopyObject: case spv::OpCopyLogical: case spv::OpBitcast:
                if (sizeOf(ids[w[3]].type) != sizeOf(w[1]))
                    return fail("a copy or bitcast that changes size");
                break;
            case spv::OpSampledImage:
                if (types[ids[w[3]].type].kind != spv::OpTypeImage)
                    return fail("a sampled image of no image");
                break;
            case spv::OpImageSampleExplicitLod:
                if (types[ids[w[3]].type].kind != spv::OpTypeSampledImage || componentCount(ids[w[4]].type) < 2 || sizeOf(w[1]) != 4)
                    return fail("a sample of no sampled image, or with the wrong coordinates");
                break;
            case spv::OpSelect: {
                const uint32_t conditionSize = componentCount(ids[w[3]].type);
                if (conditionSize != 1 && conditionSize != sizeOf(w[1]))
                    return fail("a select whose condition does not fit its result");
                break;
            }
            case spv::OpMatrixTimesVector:
                if (types[ids[w[3]].type].kind != spv::OpTypeMatrix)
                    return fail("a matrix product of no matrix");
                break;
            case spv::OpVectorTimesMatrix:
                if (types[ids[w[4]].type].kind != spv::OpTypeMatrix)
                    return fail("a matrix product of no matrix");
                break;
            case spv::OpMatrixTimesMatrix:
                if (types[ids[w[3]].type].kind != spv::OpTypeMatrix || types[ids[w[4]].type].kind != spv::OpTypeMatrix)
                    return fail("a matrix product of no matrix");
                break;
            case spv::OpExtInst:
                if (ids[w[3]].kind != IdKind::ExtSet)
                    return fail("an extended instruction of no set");
                break;
            default:
                break;
        }
        return true;
    }

    // The last defence, independent of everything above: no instruction of the finished code can
    // touch a register, table entry or instruction that does not exist.
    bool verifyCode(){
        const uint64_t registerCount = program.initial.size();
        const uint64_t codeSize = program.code.size();
        const uint64_t tableSize = program.tables.size();
        if (codeSize == 0)
            return fail("no code");
        for (const SpirvInstruction& I : program.code){
            const uint32_t op = I.op;
            const auto reg = [&](uint32_t loc, uint64_t width){ return loc + width <= registerCount; };
            const auto in = [&](uint32_t first, uint32_t last){ return op >= first && op <= last; };
            const auto n = [&](uint32_t first){ return (op - first) % 4 + 1; };
            bool ok = true;
            const auto table = [&](uint32_t at, uint64_t count){ return at + count <= tableSize; };
            if (in(Copy1, Copy4))                    ok = reg(I.a, op - Copy1 + 1) && reg(I.b, op - Copy1 + 1);
            else if (op == CopyN)                    ok = reg(I.a, I.c) && reg(I.b, I.c);
            else if (op == ClearN)                   ok = reg(I.a, I.b);
            else if (in(FAdd1, Step4))               ok = reg(I.a, n(FAdd1)) && reg(I.b, n(FAdd1)) && reg(I.c, n(FAdd1));
            else if (in(FNeg1, Round4))              ok = reg(I.a, n(FNeg1)) && reg(I.b, n(FNeg1));
            else if (in(Mix1, SmoothStep4))          ok = reg(I.a, n(Mix1)) && reg(I.b, n(Mix1)) && reg(I.c, n(Mix1)) && reg(I.d, n(Mix1));
            else if (in(IAdd1, SMax4))               ok = reg(I.a, n(IAdd1)) && reg(I.b, n(IAdd1)) && reg(I.c, n(IAdd1));
            else if (in(SClamp1, SClamp4))           ok = reg(I.a, n(SClamp1)) && reg(I.b, n(SClamp1)) && reg(I.c, n(SClamp1)) && reg(I.d, n(SClamp1));
            else if (in(INeg1, SSign4))              ok = reg(I.a, n(INeg1)) && reg(I.b, n(INeg1));
            else if (in(FEq1, UGe4))                 ok = reg(I.a, n(FEq1)) && reg(I.b, n(FEq1)) && reg(I.c, n(FEq1));
            else if (in(IsNan1, U2F4))               ok = reg(I.a, n(IsNan1)) && reg(I.b, n(IsNan1));
            else if (in(SelectS1, SelectS4))         ok = reg(I.a, n(SelectS1)) && reg(I.b, 1) && reg(I.c, n(SelectS1)) && reg(I.d, n(SelectS1));
            else if (in(SelectV1, SelectV4))         ok = reg(I.a, n(SelectV1)) && reg(I.b, n(SelectV1)) && reg(I.c, n(SelectV1)) && reg(I.d, n(SelectV1));
            else if (op == SelectSN)                 ok = table(I.d, 2) && reg(I.a, program.tables[I.d + 1]) && reg(I.b, 1)
                                                          && reg(I.c, program.tables[I.d + 1]) && reg(program.tables[I.d], program.tables[I.d + 1]);
            else if (in(Any1, All4))                 ok = reg(I.a, 1) && reg(I.b, n(Any1));
            else if (in(Dot1, Dot4))                 ok = reg(I.a, 1) && reg(I.b, n(Dot1)) && reg(I.c, n(Dot1));
            else if (in(Length1, Length4))           ok = reg(I.a, 1) && reg(I.b, n(Length1));
            else if (in(Normalize1, Normalize4))     ok = reg(I.a, n(Normalize1)) && reg(I.b, n(Normalize1));
            else if (op == Cross)                    ok = reg(I.a, 3) && reg(I.b, 3) && reg(I.c, 3);
            else if (op == Distance)                 ok = I.d <= 4 && reg(I.a, 1) && reg(I.b, I.d) && reg(I.c, I.d);
            else if (in(Vts1, Vts4))                 ok = reg(I.a, op - Vts1 + 1) && reg(I.b, op - Vts1 + 1) && reg(I.c, 1);
            else if (op == VtsN)                     ok = reg(I.a, I.d) && reg(I.b, I.d) && reg(I.c, 1);
            else if (op == MatVec || op == VecMat){
                const uint32_t cols = I.d >> 8, rows = I.d & 0xff;
                ok = cols <= 4 && rows <= 4 && reg(I.a, op == MatVec ? rows : cols) && reg(op == MatVec ? I.b : I.c, (uint64_t)cols * rows)
                     && reg(op == MatVec ? I.c : I.b, op == MatVec ? cols : rows);
            } else if (op == MatMat){
                const uint32_t rightCols = I.d >> 16, leftCols = (I.d >> 8) & 0xff, rows = I.d & 0xff;
                ok = rightCols <= 4 && leftCols <= 4 && rows <= 4 && reg(I.a, (uint64_t)rightCols * rows)
                     && reg(I.b, (uint64_t)leftCols * rows) && reg(I.c, (uint64_t)rightCols * leftCols);
            } else if (op == Shuffle){
                ok = I.c <= 4 && reg(I.a, I.c) && table(I.b, I.c);
                for (uint32_t k = 0; ok && k < I.c; k++)
                    ok = reg(program.tables[I.b + k], 1);
            } else if (op == ExtractDyn)             ok = reg(I.a, 1) && reg(I.b, I.d) && reg(I.c, 1) && I.d >= 1;
            else if (op == InsertDyn)                ok = reg(I.a, I.d) && reg(I.b, 1) && reg(I.c, 1) && I.d >= 1;
            else if (op == PtrIndex)                 ok = reg(I.a, 1) && reg(I.c, 1) && table(I.d, 2) && program.tables[I.d + 1] >= 1;
            else if (op == PtrIndexAdd)              ok = reg(I.a, 1) && reg(I.b, 1) && reg(I.c, 1) && table(I.d, 2) && program.tables[I.d + 1] >= 1;
            else if (op == LoadInd)                  ok = reg(I.a, I.c) && reg(I.b, 1);
            else if (op == StoreInd)                 ok = reg(I.a, 1) && reg(I.b, I.c);
            else if (op == Math1)                    ok = I.c < std::size(math1Functions) && I.d <= 4 && reg(I.a, I.d) && reg(I.b, I.d);
            else if (op == Math2)                    ok = (I.d >> 8) < std::size(math2Functions) && (I.d & 0xff) <= 4 && reg(I.a, I.d & 0xff)
                                                          && reg(I.b, I.d & 0xff) && reg(I.c, I.d & 0xff);
            else if (op == Jump || op == Call)       ok = I.a < codeSize;
            else if (op == JumpIf)                   ok = I.a < codeSize && I.c < codeSize && reg(I.b, 1);
            else if (op == Ret)                      ok = true;
            else if (op == Sample)                   ok = reg(I.a, 4) && reg(I.b, 1) && reg(I.c, 2) && reg(I.d, 1);
            else                                     ok = false;
            if (!ok)
                return fail("an operand of the wrong shape for its instruction");
        }
        return true;
    }

    // -- emission ------------------------------------------------------------------------------

    void emit(uint32_t op, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0){
        SpirvInstruction instruction;
        instruction.op = op;
        instruction.a = a;
        instruction.b = b;
        instruction.c = c;
        instruction.d = d;
        program.code.push_back(instruction);
    }

    void emitCopy(uint32_t dst, uint32_t src, uint32_t size){
        if (dst == src || size == 0)
            return;
        if (size <= 4)
            emit(Copy1 + size - 1, dst, src);
        else
            emit(CopyN, dst, src, size);
    }

    uint32_t componentCount(uint32_t type) const {
        const IdType& t = types[type];
        return t.kind == spv::OpTypeVector ? t.count : 1;
    }

    uint32_t sizeOf(uint32_t type) const { return types[type].size; }

    struct Analysis {
        std::map<uint32_t, uint32_t> useCount;
        std::map<uint32_t, uint32_t> lastUse;       // position of the last use, in this block's order
        std::map<uint32_t, uint32_t> defBlock;
        std::set<uint32_t> crossesBlock;
        std::map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> rootStores; // root -> (position, block)
        std::map<uint32_t, uint32_t> storeCount;
        std::set<uint32_t> elided;                  // instructions (by position) with nothing to emit
        std::map<uint32_t, const RawInstruction*> defInstruction;
    };

    static bool retargetable(spv::Op op){
        switch (op){
            case spv::OpFAdd: case spv::OpFSub: case spv::OpFMul: case spv::OpFDiv: case spv::OpFMod:
            case spv::OpFNegate: case spv::OpIAdd: case spv::OpISub: case spv::OpIMul: case spv::OpSDiv: case spv::OpUDiv:
            case spv::OpSMod: case spv::OpUMod: case spv::OpShiftLeftLogical: case spv::OpShiftRightLogical:
            case spv::OpShiftRightArithmetic: case spv::OpBitwiseAnd: case spv::OpBitwiseOr: case spv::OpBitwiseXor: case spv::OpNot:
            case spv::OpConvertFToS: case spv::OpConvertFToU: case spv::OpConvertSToF: case spv::OpConvertUToF: case spv::OpDot:
            case spv::OpVectorTimesScalar: case spv::OpSelect: case spv::OpExtInst: case spv::OpFOrdEqual: case spv::OpFOrdLessThan:
            case spv::OpFOrdGreaterThan: case spv::OpFOrdLessThanEqual: case spv::OpFOrdGreaterThanEqual: case spv::OpSLessThan:
            case spv::OpSGreaterThan: case spv::OpSLessThanEqual: case spv::OpSGreaterThanEqual: case spv::OpIEqual: case spv::OpINotEqual:
            case spv::OpLogicalAnd: case spv::OpLogicalOr: case spv::OpLogicalNot: case spv::OpSNegate: case spv::OpLoad:
            case spv::OpFunctionCall:
                return true;
            default:
                return false;
        }
    }

    bool emitProgram(){
        for (uint32_t functionId : functionOrder){
            if (!resolveStaticPointers(functions[functionId]))
                return false;
        }
        // a pointer parameter one call might write through is copied back after that call
        for (uint32_t functionId : functionOrder)
            findWrittenParams(functions[functionId]);
        bool changed = true;
        while (changed){
            changed = false;
            for (uint32_t functionId : functionOrder)
                changed = propagateWrittenParams(functions[functionId]) || changed;
        }
        // the entry stub: reset private variables, call main, stop
        program.entry = 0;
        for (uint32_t id : privateInits){
            const uint32_t init = variableInit[id];
            emitCopy(ids[id].loc, ids[init].loc, sizeOf(ids[id].pointee));
        }
        for (uint32_t id : resetEachPixel)
            emit(ClearN, ids[id].loc, sizeOf(ids[id].pointee));
        emit(Call, 0);
        const size_t entryCall = program.code.size() - 1;
        emit(Ret);
        for (uint32_t functionId : functionOrder){
            FunctionInfo& function = functions[functionId];
            function.start = (uint32_t)program.code.size();
            if (!emitFunction(function))
                return false;
        }
        for (const auto& [index, callee] : callPatches)
            program.code[index].a = functions[callee].start;
        program.code[entryCall].a = functions[entryFunction].start;
        program.maxCallDepth = 2 + callDepth(entryFunction);
        return true;
    }

    uint32_t callDepth(uint32_t functionId){
        auto known = callDepths.find(functionId);
        if (known != callDepths.end())
            return known->second;
        uint32_t depth = 0;
        for (uint32_t callee : functions[functionId].callees)
            depth = std::max(depth, 1 + callDepth(callee));
        callDepths[functionId] = depth;
        return depth;
    }

    std::map<uint32_t, uint32_t> callDepths;

    std::vector<std::pair<size_t, uint32_t>> callPatches;

    // Constant access chains have a register known now; a dynamic one gets a pointer register.
    bool resolveStaticPointers(FunctionInfo& function){
        for (const RawInstruction& raw : function.body){
            if (raw.op != spv::OpAccessChain && raw.op != spv::OpInBoundsAccessChain)
                continue;
            const uint32_t* w = raw.words;
            const IdInfo& base = ids[w[3]];
            if (base.kind != IdKind::StaticPointer && base.kind != IdKind::DynamicPointer)
                return fail("an access chain on something that is not a pointer");
            uint32_t pointee = base.pointee;
            uint32_t offset = base.loc;
            bool isStatic = base.kind == IdKind::StaticPointer;
            uint32_t i = 4;
            for (; i < raw.count && isStatic; i++){
                const IdInfo& index = ids[w[i]];
                auto constant = constantValues.find(w[i]);
                if (constant == constantValues.end() || index.kind != IdKind::Register){
                    isStatic = false;
                    break;
                }
                offset += memberOffset(pointee, constant->second);
                pointee = memberType(pointee, constant->second);
            }
            IdInfo& result = ids[w[2]];
            result.type = w[1];
            result.pointee = types[w[1]].element;
            result.root = base.root;
            if (isStatic){
                result.kind = IdKind::StaticPointer;
                result.loc = offset;
            } else {
                result.kind = IdKind::DynamicPointer;
                result.loc = allocate(1);
            }
        }
        return true;
    }

    bool hasMember(uint32_t type, uint32_t index){
        const IdType& t = types[type];
        const bool composite = t.kind == spv::OpTypeVector || t.kind == spv::OpTypeMatrix || t.kind == spv::OpTypeArray;
        if (t.kind == spv::OpTypeStruct ? index < t.members.size() : (composite && index < t.count))
            return true;
        return fail("an index out of its composite");
    }

    uint32_t memberOffset(uint32_t type, uint32_t index){
        if (!hasMember(type, index))
            return 0;
        const IdType& t = types[type];
        if (t.kind == spv::OpTypeStruct){
            uint32_t offset = 0;
            for (uint32_t i = 0; i < index; i++)
                offset += types[t.members[i]].size;
            return offset;
        }
        return index * t.elementSize;
    }

    uint32_t memberType(uint32_t type, uint32_t index){
        if (!hasMember(type, index))
            return 0;
        const IdType& t = types[type];
        return t.kind == spv::OpTypeStruct ? t.members[index] : t.element;
    }

    void findWrittenParams(FunctionInfo& function){
        for (const RawInstruction& raw : function.body){
            if (raw.op != spv::OpStore)
                continue;
            for (size_t i = 0; i < function.params.size(); i++)
                if (ids[raw.words[1]].root == function.params[i])
                    function.paramWritten[i] = true;
        }
    }

    bool propagateWrittenParams(FunctionInfo& function){
        bool changed = false;
        for (const RawInstruction& raw : function.body){
            if (raw.op != spv::OpFunctionCall)
                continue;
            const FunctionInfo& callee = functions[raw.words[3]];
            for (uint32_t i = 4; i < raw.count; i++){
                if (!callee.paramWritten[i - 4])
                    continue;
                const uint32_t root = ids[raw.words[i]].root;
                for (size_t p = 0; p < function.params.size(); p++){
                    if (root == function.params[p] && !function.paramWritten[p]){
                        function.paramWritten[p] = true;
                        changed = true;
                    }
                }
            }
        }
        return changed;
    }

    void analyse(const FunctionInfo& function, Analysis& analysis){
        for (const RawInstruction& raw : function.body){
            if (hasResult(raw.op)){
                analysis.defBlock[raw.words[2]] = raw.block;
                analysis.defInstruction[raw.words[2]] = &raw;
            }
            if (raw.op == spv::OpStore){
                analysis.rootStores[ids[raw.words[1]].root].push_back({raw.position, raw.block});
                analysis.storeCount[ids[raw.words[1]].root]++;
            } else if (raw.op == spv::OpFunctionCall){
                const FunctionInfo& callee = functions[raw.words[3]];
                for (uint32_t i = 4; i < raw.count; i++){
                    if (callee.paramWritten[i - 4]){
                        analysis.rootStores[ids[raw.words[i]].root].push_back({raw.position, raw.block});
                        analysis.storeCount[ids[raw.words[i]].root]++;
                    }
                }
            }
        }
        for (const RawInstruction& raw : function.body){
            forEachOperand(raw, [&](uint32_t id){
                analysis.useCount[id]++;
                auto def = analysis.defBlock.find(id);
                if (def != analysis.defBlock.end() && def->second != raw.block)
                    analysis.crossesBlock.insert(id);
                uint32_t& last = analysis.lastUse[id];
                last = std::max(last, raw.position);
            });
            if (raw.op == spv::OpPhi){
                for (uint32_t i = 3; i + 1 < raw.count; i += 2)
                    analysis.crossesBlock.insert(raw.words[i]);
            }
        }
    }

    // True when the op's result is the same bits as a part of its first operand: no instruction.
    bool aliasOf(const RawInstruction& raw, uint32_t& source, uint32_t& offset){
        const uint32_t* w = raw.words;
        switch (raw.op){
            case spv::OpCopyObject: case spv::OpCopyLogical: case spv::OpBitcast: case spv::OpSampledImage:
                source = w[3];
                offset = 0;
                return true;
            case spv::OpCompositeExtract: {
                uint32_t type = ids[w[3]].type;
                offset = 0;
                for (uint32_t i = 4; i < raw.count; i++){
                    offset += memberOffset(type, w[i]);
                    type = memberType(type, w[i]);
                }
                source = w[3];
                return true;
            }
            case spv::OpVectorShuffle: {
                const uint32_t leftSize = types[ids[w[3]].type].count;
                const uint32_t first = w[5];
                if (first == 0xffffffffu)
                    return false;
                for (uint32_t i = 5; i < raw.count; i++){
                    if (w[i] != first + (i - 5))
                        return false;
                }
                const uint32_t n = raw.count - 5;
                const bool left = first < leftSize;
                if (left ? first + n > leftSize : first - leftSize + n > types[ids[w[4]].type].count)
                    return false;
                source = left ? w[3] : w[4];
                offset = left ? first : first - leftSize;
                return true;
            }
            default:
                return false;
        }
    }

    // A value's reads, following the instructions that merely rename it
    void collectAliasUses(uint32_t id, const Analysis& analysis, const std::map<uint32_t, std::vector<uint32_t>>& children,
                          uint32_t& last, bool& crosses) const {
        auto use = analysis.lastUse.find(id);
        if (use != analysis.lastUse.end())
            last = std::max(last, use->second);
        if (analysis.crossesBlock.count(id))
            crosses = true;
        auto child = children.find(id);
        if (child != children.end()){
            for (uint32_t c : child->second)
                collectAliasUses(c, analysis, children, last, crosses);
        }
    }

    bool emitFunction(FunctionInfo& function){
        Analysis analysis;
        analyse(function, analysis);

        // renaming instructions
        std::map<uint32_t, std::vector<uint32_t>> children;
        for (const RawInstruction& raw : function.body){
            uint32_t source = 0, offset = 0;
            if (aliasOf(raw, source, offset))
                children[source].push_back(raw.words[2]);
        }

        // variables holding one constant for their whole life: glslang stores a const array into
        // a temporary to index it, which would copy the array on every access
        std::set<uint32_t> constantVariables;
        for (uint32_t variable : function.variables){
            auto init = variableInit.find(variable);
            const uint32_t stores = analysis.storeCount.count(variable) ? analysis.storeCount.at(variable) : 0;
            if (stores == 1 && init == variableInit.end()){
                const auto& position = analysis.rootStores.at(variable)[0];
                for (const RawInstruction& raw : function.body){
                    if (raw.position == position.first && raw.op == spv::OpStore && ids[raw.words[1]].kind == IdKind::StaticPointer
                            && raw.words[1] == variable && isConstant(raw.words[2])){
                        ids[variable].loc = ids[raw.words[2]].loc;
                        constantVariables.insert(variable);
                        analysis.elided.insert(raw.position);
                    }
                }
            } else if (stores == 0 && init != variableInit.end()){
                ids[variable].loc = ids[init->second].loc;
                constantVariables.insert(variable);
            }
        }
        // access chains into such a variable were resolved against its first register
        rebaseStaticPointers(function, constantVariables);

        // a temporary made only to pass a read-only argument can live in the parameter
        std::set<uint32_t> inParameter = aliasArgumentTemporaries(function, constantVariables);

        // a value computed just to be stored: compute it in the variable
        std::map<uint32_t, uint32_t> retarget;           // value id -> register
        std::map<uint32_t, uint32_t> retargetedStoreAt;  // store position -> def position
        for (size_t s = 0; s < function.body.size(); s++){
            const RawInstruction& store = function.body[s];
            if (store.op != spv::OpStore || analysis.elided.count(store.position))
                continue;
            const uint32_t pointer = store.words[1];
            const uint32_t value = store.words[2];
            if (ids[pointer].kind != IdKind::StaticPointer)
                continue;
            auto def = analysis.defInstruction.find(value);
            if (def == analysis.defInstruction.end() || def->second->block != store.block || analysis.useCount[value] != 1)
                continue;
            const RawInstruction& definition = *def->second;
            if (!retargetable(definition.op))
                continue;
            const uint32_t root = ids[pointer].root;
            bool clear = true;
            for (size_t k = definition.position + 1; k < store.position && clear; k++){
                const RawInstruction& between = function.body[k];
                if (between.op == spv::OpLoad && ids[between.words[3]].root == root)
                    clear = false;
                else if (between.op == spv::OpStore && ids[between.words[1]].root == root)
                    clear = false;
                else if (between.op == spv::OpFunctionCall){
                    for (uint32_t i = 4; i < between.count; i++)
                        if (ids[between.words[i]].root == root)
                            clear = false;
                    if (isGlobalRoot(root) || inParameter.count(root))
                        clear = false;
                }
            }
            if (!clear)
                continue;
            retarget[value] = ids[pointer].loc;
            analysis.elided.insert(store.position);
            retargetedStoreAt[store.position] = definition.position;
        }

        // first pass over the values: where each one lives
        std::set<uint32_t> loadAliases;
        for (const RawInstruction& raw : function.body){
            if (!hasResult(raw.op))
                continue;
            const uint32_t id = raw.words[2];
            if (raw.op == spv::OpAccessChain || raw.op == spv::OpInBoundsAccessChain)
                continue; // resolved above
            IdInfo& info = ids[id];
            info.kind = IdKind::Register;
            info.type = raw.words[1];
            uint32_t source = 0, offset = 0;
            if (raw.op == spv::OpLoad && ids[raw.words[3]].kind == IdKind::StaticPointer && !retarget.count(id)){
                const uint32_t root = ids[raw.words[3]].root;
                if (readOnlyRoots.count(root) || constantVariables.count(root) || isReadOnlyParam(function, root)){
                    info.loc = ids[raw.words[3]].loc;
                    loadAliases.insert(id);
                    continue;
                }
                if (canAliasLoad(raw, root, analysis, retargetedStoreAt, children)){
                    info.loc = ids[raw.words[3]].loc;
                    loadAliases.insert(id);
                    continue;
                }
            }
            if (aliasOf(raw, source, offset)){
                info.loc = ids[source].loc + offset;
                continue;
            }
            auto target = retarget.find(id);
            if (target != retarget.end())
                info.loc = target->second;
            else
                info.loc = allocate(sizeOf(raw.words[1]));
        }

        // entry: uninitialised variables start at zero so a pixel never reads its neighbour's value
        for (uint32_t variable : function.variables){
            if (constantVariables.count(variable) || inParameter.count(variable))
                continue;
            auto init = variableInit.find(variable);
            if (init != variableInit.end()){
                emitCopy(ids[variable].loc, ids[init->second].loc, sizeOf(ids[variable].pointee));
            } else if (!firstAccessIsStoreInEntry(function, variable)){
                emit(ClearN, ids[variable].loc, sizeOf(ids[variable].pointee));
            }
        }

        // the code
        labelPositions.clear();
        pendingJumps.clear();
        phis.clear();
        for (const RawInstruction& raw : function.body){
            if (raw.op == spv::OpPhi)
                phis[raw.block].push_back(&raw);
        }
        for (const RawInstruction& raw : function.body){
            if (analysis.elided.count(raw.position))
                continue;
            if (!emitInstruction(function, raw, loadAliases))
                return false;
        }
        for (const auto& jump : pendingJumps){
            auto label = labelPositions.find(jump.label);
            if (label == labelPositions.end())
                return fail("a branch to an unknown block");
            if (jump.operand == 0)
                program.code[jump.index].a = label->second;
            else
                program.code[jump.index].c = label->second;
        }
        return true;
    }

    // glslang stores each argument into a temporary right before the call, which the call would then
    // copy into the parameter. A temporary used for nothing else moves into the parameter's register.
    // No other call may run between the stores and this call: it could be to the same function.
    std::set<uint32_t> aliasArgumentTemporaries(const FunctionInfo& function, const std::set<uint32_t>& constantVariables){
        std::set<uint32_t> moved;
        for (uint32_t variable : function.variables){
            if (variableInit.count(variable) || constantVariables.count(variable))
                continue;
            const RawInstruction* call = nullptr;
            uint32_t firstStore = UINT32_MAX;
            bool usable = true;
            uint32_t parameter = 0;
            for (const RawInstruction& raw : function.body){
                if (raw.op == spv::OpStore && raw.words[1] == variable){
                    firstStore = std::min(firstStore, raw.position);
                    continue;
                }
                if (raw.op == spv::OpFunctionCall){
                    bool passed = false;
                    for (uint32_t i = 4; i < raw.count; i++){
                        if (raw.words[i] == variable){
                            const FunctionInfo& callee = functions[raw.words[3]];
                            if (passed || call != nullptr || callee.paramWritten[i - 4] || ids[callee.params[i - 4]].kind != IdKind::StaticPointer)
                                usable = false;
                            call = &raw;
                            parameter = callee.params[i - 4];
                            passed = true;
                        }
                    }
                    if (passed)
                        continue;
                }
                forEachOperand(raw, [&](uint32_t id){
                    if (id == variable)
                        usable = false;
                });
            }
            if (!usable || call == nullptr || firstStore == UINT32_MAX || firstStore > call->position)
                continue;
            for (const RawInstruction& raw : function.body){
                if (raw.position < firstStore || raw.position >= call->position)
                    continue;
                if (raw.op == spv::OpFunctionCall || raw.block != call->block)
                    usable = false;
            }
            if (!usable)
                continue;
            ids[variable].loc = ids[parameter].loc;
            moved.insert(variable);
        }
        return moved;
    }

    bool isGlobalRoot(uint32_t root) const {
        const uint32_t type = ids[root].type;
        return types[type].storage == spv::StorageClassPrivate || types[type].storage == spv::StorageClassOutput;
    }

    bool isConstant(uint32_t id) const { return constantIds.count(id) != 0; }

    void rebaseStaticPointers(const FunctionInfo& function, const std::set<uint32_t>& constantVariables){
        if (constantVariables.empty())
            return;
        for (const RawInstruction& raw : function.body){
            if (raw.op != spv::OpAccessChain && raw.op != spv::OpInBoundsAccessChain)
                continue;
            if (!constantVariables.count(ids[raw.words[3]].root) || ids[raw.words[2]].kind != IdKind::StaticPointer)
                continue;
            uint32_t offset = ids[raw.words[3]].loc;
            uint32_t pointee = ids[raw.words[3]].pointee;
            for (uint32_t i = 4; i < raw.count; i++){
                const uint32_t index = constantValues[raw.words[i]];
                offset += memberOffset(pointee, index);
                pointee = memberType(pointee, index);
            }
            ids[raw.words[2]].loc = offset;
        }
    }

    bool isReadOnlyParam(const FunctionInfo& function, uint32_t root) const {
        for (size_t i = 0; i < function.params.size(); i++){
            if (function.params[i] == root)
                return !function.paramWritten[i];
        }
        return false;
    }

    // A load can read the variable's register directly while every read of the value comes before
    // the next write to that variable, in one block.
    bool canAliasLoad(const RawInstruction& load, uint32_t root, const Analysis& analysis,
                      const std::map<uint32_t, uint32_t>& retargetedStoreAt,
                      const std::map<uint32_t, std::vector<uint32_t>>& children) const {
        if (isGlobalRoot(root))
            return false;
        uint32_t last = load.position;
        bool crosses = false;
        collectAliasUses(load.words[2], analysis, children, last, crosses);
        if (crosses)
            return false;
        auto stores = analysis.rootStores.find(root);
        if (stores == analysis.rootStores.end())
            return true;
        for (const auto& store : stores->second){
            if (store.second != load.block)
                continue;
            uint32_t position = store.first;
            auto moved = retargetedStoreAt.find(position);
            if (moved != retargetedStoreAt.end())
                position = moved->second;
            // the write at the last use is fine: an instruction reads before it writes
            if (position >= load.position && position < last)
                return false;
        }
        return true;
    }

    bool firstAccessIsStoreInEntry(const FunctionInfo& function, uint32_t variable) const {
        if (function.body.empty())
            return false;
        const uint32_t entryBlock = function.body.front().block;
        for (const RawInstruction& raw : function.body){
            if (raw.block != entryBlock)
                return false;
            if (raw.op == spv::OpStore && raw.words[1] == variable)
                return true;
            bool touches = false;
            forEachOperand(raw, [&](uint32_t id){
                if (id < ids.size() && ids[id].root == variable)
                    touches = true;
            });
            if (touches)
                return false;
        }
        return false;
    }

    struct PendingJump {
        size_t index;
        int operand;
        uint32_t label;
    };
    std::map<uint32_t, uint32_t> labelPositions;
    std::vector<PendingJump> pendingJumps;
    std::map<uint32_t, std::vector<const RawInstruction*>> phis;

    uint32_t currentBlock = 0;

    // The copies of the phis of `target` for arriving from the current block
    void emitPhiCopies(uint32_t target){
        auto found = phis.find(target);
        if (found == phis.end())
            return;
        std::vector<std::pair<uint32_t, uint32_t>> copies; // dst, src
        std::vector<uint32_t> sizes;
        for (const RawInstruction* phi : found->second){
            for (uint32_t i = 3; i + 1 < phi->count; i += 2){
                if (phi->words[i + 1] == currentBlock){
                    copies.push_back({ids[phi->words[2]].loc, ids[phi->words[i]].loc});
                    sizes.push_back(sizeOf(phi->words[1]));
                }
            }
        }
        // a phi may read a part of another phi's register, not only the whole of it
        bool conflict = false;
        for (size_t i = 0; i < copies.size(); i++)
            for (size_t j = 0; j < copies.size(); j++)
                if (i != j && copies[j].second < copies[i].first + sizes[i] && copies[i].first < copies[j].second + sizes[j])
                    conflict = true;
        if (conflict){
            for (size_t i = 0; i < copies.size(); i++){
                const uint32_t temp = allocate(sizes[i]);
                emitCopy(temp, copies[i].second, sizes[i]);
                copies[i].second = temp;
            }
        }
        for (size_t i = 0; i < copies.size(); i++)
            emitCopy(copies[i].first, copies[i].second, sizes[i]);
    }

    bool hasPhis(uint32_t target) const { return phis.find(target) != phis.end(); }

    void emitJumpTo(uint32_t label){
        emitPhiCopies(label);
        emit(Jump);
        pendingJumps.push_back({program.code.size() - 1, 0, label});
    }

    bool emitInstruction(FunctionInfo& function, const RawInstruction& raw, const std::set<uint32_t>& loadAliases);
    bool emitExtended(const RawInstruction& raw);
    bool emitCall(const RawInstruction& raw);
    void emitDynamicPointer(const RawInstruction& raw);

    void binaryFamily(uint32_t base, const uint32_t* w){
        emit(base + componentCount(w[1]) - 1, ids[w[2]].loc, ids[w[3]].loc, ids[w[4]].loc);
    }
    void unaryFamily2(uint32_t base, const uint32_t* w){
        emit(base + componentCount(w[1]) - 1, ids[w[2]].loc, ids[w[3]].loc);
    }
};

}

using jasperengine::editor::procmat::SpirvCompiler;

bool SpirvCompiler::emitCall(const RawInstruction& raw){
    const uint32_t* w = raw.words;
    const FunctionInfo& callee = functions[w[3]];
    for (uint32_t i = 4; i < raw.count; i++){
        const uint32_t param = callee.params[i - 4];
        const IdInfo& argument = ids[w[i]];
        const IdInfo& target = ids[param];
        if (target.kind == IdKind::StaticPointer){
            const uint32_t size = sizeOf(target.pointee);
            if (argument.kind == IdKind::StaticPointer)
                emitCopy(target.loc, argument.loc, size);
            else if (argument.kind == IdKind::DynamicPointer)
                emit(LoadInd, target.loc, argument.loc, size);
            else
                return fail("a pointer parameter given a value");
        } else {
            emitCopy(target.loc, argument.loc, sizeOf(target.type));
        }
    }
    emit(Call);
    callPatches.push_back({program.code.size() - 1, w[3]});
    for (uint32_t i = 4; i < raw.count; i++){
        if (!callee.paramWritten[i - 4])
            continue;
        const IdInfo& argument = ids[w[i]];
        const IdInfo& target = ids[callee.params[i - 4]];
        const uint32_t size = sizeOf(target.pointee);
        if (argument.kind == IdKind::StaticPointer)
            emitCopy(argument.loc, target.loc, size);
        else
            emit(StoreInd, argument.loc, target.loc, size);
    }
    if (types[w[1]].kind != spv::OpTypeVoid)
        emitCopy(ids[w[2]].loc, callee.returnLoc, sizeOf(w[1]));
    return true;
}

void SpirvCompiler::emitDynamicPointer(const RawInstruction& raw){
    const uint32_t* w = raw.words;
    const IdInfo& result = ids[w[2]];
    const IdInfo& base = ids[w[3]];
    uint32_t pointee = base.pointee;
    uint32_t staticOffset = base.kind == IdKind::StaticPointer ? base.loc : 0;
    bool haveRegister = base.kind == IdKind::DynamicPointer;
    uint32_t registerNow = base.loc;
    for (uint32_t i = 4; i < raw.count; i++){
        auto constant = constantValues.find(w[i]);
        const IdType& type = types[pointee];
        if (constant != constantValues.end()){
            staticOffset += memberOffset(pointee, constant->second);
            pointee = memberType(pointee, constant->second);
            continue;
        }
        if (type.kind != spv::OpTypeVector && type.kind != spv::OpTypeMatrix && type.kind != spv::OpTypeArray){
            fail("a dynamic index on something that is not an array, vector or matrix");
            return;
        }
        const uint32_t table = (uint32_t)program.tables.size();
        program.tables.push_back(type.elementSize);
        program.tables.push_back(type.count);
        if (haveRegister){
            emit(PtrIndexAdd, result.loc, registerNow, ids[w[i]].loc, table);
        } else {
            emit(PtrIndex, result.loc, staticOffset, ids[w[i]].loc, table);
            staticOffset = 0;
        }
        registerNow = result.loc;
        haveRegister = true;
        pointee = type.element;
    }
    if (staticOffset != 0){
        // constant members outside the dynamic indices: an index of 1 in a table of stride offset
        const uint32_t constantRegister = allocate(1);
        program.initial[constantRegister].u = 1;
        const uint32_t table = (uint32_t)program.tables.size();
        program.tables.push_back(staticOffset);
        program.tables.push_back(2);
        emit(PtrIndexAdd, result.loc, registerNow, constantRegister, table);
    } else if (registerNow != result.loc){
        emit(Copy1, result.loc, registerNow);
    }
}

bool SpirvCompiler::emitInstruction(FunctionInfo& function, const RawInstruction& raw, const std::set<uint32_t>& loadAliases){
    const uint32_t* w = raw.words;
    using namespace spv;
    const auto binary = [&](uint32_t base){ binaryFamily(base, w); return true; };
    const auto unary = [&](uint32_t base){ unaryFamily2(base, w); return true; };
    switch (raw.op){
        case OpLabel:
            currentBlock = w[1];
            labelPositions[w[1]] = (uint32_t)program.code.size();
            return true;
        case OpVariable: case OpNop: case OpSelectionMerge: case OpLoopMerge: case OpLine: case OpNoLine: case OpPhi:
        case OpCopyObject: case OpCopyLogical: case OpBitcast: case OpCompositeExtract: case OpSampledImage:
            return true;
        case OpUndef:
            emit(ClearN, ids[w[2]].loc, sizeOf(w[1]));
            return true;
        case OpVectorShuffle: {
            uint32_t source = 0, offset = 0;
            if (aliasOf(raw, source, offset))
                return true;
            const uint32_t leftSize = types[ids[w[3]].type].count;
            const uint32_t table = (uint32_t)program.tables.size();
            const uint32_t n = raw.count - 5;
            for (uint32_t i = 0; i < n; i++){
                const uint32_t c = w[5 + i];
                const uint32_t which = c == 0xffffffffu ? ids[w[3]].loc : (c < leftSize ? ids[w[3]].loc + c : ids[w[4]].loc + c - leftSize);
                program.tables.push_back(which);
            }
            emit(Shuffle, ids[w[2]].loc, table, n);
            return true;
        }
        case OpLoad: {
            if (loadAliases.count(w[2]))
                return true;
            const IdInfo& pointer = ids[w[3]];
            if (pointer.kind == IdKind::StaticPointer)
                emitCopy(ids[w[2]].loc, pointer.loc, sizeOf(w[1]));
            else
                emit(LoadInd, ids[w[2]].loc, pointer.loc, sizeOf(w[1]));
            return true;
        }
        case OpStore: {
            const IdInfo& pointer = ids[w[1]];
            const uint32_t size = sizeOf(ids[w[2]].type);
            if (pointer.kind == IdKind::StaticPointer)
                emitCopy(pointer.loc, ids[w[2]].loc, size);
            else
                emit(StoreInd, pointer.loc, ids[w[2]].loc, size);
            return true;
        }
        case OpAccessChain: case OpInBoundsAccessChain:
            if (ids[w[2]].kind == IdKind::DynamicPointer)
                emitDynamicPointer(raw);
            return true;
        case OpCompositeConstruct: {
            uint32_t offset = ids[w[2]].loc;
            for (uint32_t i = 3; i < raw.count; i++){
                const uint32_t size = sizeOf(ids[w[i]].type);
                emitCopy(offset, ids[w[i]].loc, size);
                offset += size;
            }
            return true;
        }
        case OpCompositeInsert: {
            uint32_t type = w[1];
            uint32_t offset = 0;
            for (uint32_t i = 5; i < raw.count; i++){
                offset += memberOffset(type, w[i]);
                type = memberType(type, w[i]);
            }
            emitCopy(ids[w[2]].loc, ids[w[4]].loc, sizeOf(w[1]));
            emitCopy(ids[w[2]].loc + offset, ids[w[3]].loc, sizeOf(type));
            return true;
        }
        case OpVectorExtractDynamic:
            emit(ExtractDyn, ids[w[2]].loc, ids[w[3]].loc, ids[w[4]].loc, componentCount(ids[w[3]].type));
            return true;
        case OpVectorInsertDynamic: {
            const uint32_t n = componentCount(w[1]);
            emitCopy(ids[w[2]].loc, ids[w[3]].loc, n);
            emit(InsertDyn, ids[w[2]].loc, ids[w[4]].loc, ids[w[5]].loc, n);
            return true;
        }
        case OpFAdd: return binary(FAdd1);
        case OpFSub: return binary(FSub1);
        case OpFMul: return binary(FMul1);
        case OpFDiv: return binary(FDiv1);
        case OpFMod: return binary(FMod1);
        case OpFNegate: return unary(FNeg1);
        case OpIAdd: return binary(IAdd1);
        case OpISub: return binary(ISub1);
        case OpIMul: return binary(IMul1);
        case OpUDiv: return binary(UDiv1);
        case OpSDiv: return binary(SDiv1);
        case OpUMod: return binary(UMod1);
        case OpSMod: return binary(SMod1);
        case OpSNegate: return unary(INeg1);
        case OpNot: return unary(Not1);
        case OpLogicalNot: return unary(BoolNot1);
        case OpBitwiseAnd: case OpLogicalAnd: return binary(And1);
        case OpBitwiseOr: case OpLogicalOr: return binary(Or1);
        case OpBitwiseXor: return binary(Xor1);
        case OpLogicalNotEqual: return binary(INe1);
        case OpLogicalEqual: return binary(IEq1);
        case OpShiftLeftLogical: return binary(Shl1);
        case OpShiftRightLogical: return binary(ShrL1);
        case OpShiftRightArithmetic: return binary(ShrA1);
        case OpFOrdEqual: return binary(FEq1);
        case OpFOrdNotEqual: return binary(FNe1);
        case OpFOrdLessThan: return binary(FLt1);
        case OpFOrdLessThanEqual: return binary(FLe1);
        case OpFOrdGreaterThan: return binary(FGt1);
        case OpFOrdGreaterThanEqual: return binary(FGe1);
        case OpFUnordNotEqual: return binary(FUNe1);
        case OpIEqual: return binary(IEq1);
        case OpINotEqual: return binary(INe1);
        case OpSLessThan: return binary(SLt1);
        case OpSLessThanEqual: return binary(SLe1);
        case OpSGreaterThan: return binary(SGt1);
        case OpSGreaterThanEqual: return binary(SGe1);
        case OpULessThan: return binary(ULt1);
        case OpULessThanEqual: return binary(ULe1);
        case OpUGreaterThan: return binary(UGt1);
        case OpUGreaterThanEqual: return binary(UGe1);
        case OpIsNan: return unary(IsNan1);
        case OpConvertFToS: return unary(F2S1);
        case OpConvertFToU: return unary(F2U1);
        case OpConvertSToF: return unary(S2F1);
        case OpConvertUToF: return unary(U2F1);
        case OpAny:
            emit(Any1 + componentCount(ids[w[3]].type) - 1, ids[w[2]].loc, ids[w[3]].loc);
            return true;
        case OpAll:
            emit(All1 + componentCount(ids[w[3]].type) - 1, ids[w[2]].loc, ids[w[3]].loc);
            return true;
        case OpSelect: {
            const uint32_t size = sizeOf(w[1]);
            const bool scalarCondition = componentCount(ids[w[3]].type) == 1;
            if (scalarCondition && size > 4)
                emit(SelectSN, ids[w[2]].loc, ids[w[3]].loc, ids[w[4]].loc, program.tables.size());
            else if (scalarCondition)
                emit(SelectS1 + size - 1, ids[w[2]].loc, ids[w[3]].loc, ids[w[4]].loc, ids[w[5]].loc);
            else
                emit(SelectV1 + size - 1, ids[w[2]].loc, ids[w[3]].loc, ids[w[4]].loc, ids[w[5]].loc);
            if (scalarCondition && size > 4){
                program.tables.push_back(ids[w[5]].loc);
                program.tables.push_back(size);
            }
            return true;
        }
        case OpDot:
            emit(Dot1 + componentCount(ids[w[3]].type) - 1, ids[w[2]].loc, ids[w[3]].loc, ids[w[4]].loc);
            return true;
        case OpVectorTimesScalar:
            emit(Vts1 + componentCount(w[1]) - 1, ids[w[2]].loc, ids[w[3]].loc, ids[w[4]].loc);
            return true;
        case OpMatrixTimesScalar:
            emit(VtsN, ids[w[2]].loc, ids[w[3]].loc, ids[w[4]].loc, sizeOf(w[1]));
            return true;
        case OpMatrixTimesVector: {
            const IdType& matrix = types[ids[w[3]].type];
            emit(MatVec, ids[w[2]].loc, ids[w[3]].loc, ids[w[4]].loc, (matrix.count << 8) | matrix.elementSize);
            return true;
        }
        case OpVectorTimesMatrix: {
            const IdType& matrix = types[ids[w[4]].type];
            emit(VecMat, ids[w[2]].loc, ids[w[3]].loc, ids[w[4]].loc, (matrix.count << 8) | matrix.elementSize);
            return true;
        }
        case OpMatrixTimesMatrix: {
            const IdType& left = types[ids[w[3]].type];
            const IdType& right = types[ids[w[4]].type];
            emit(MatMat, ids[w[2]].loc, ids[w[3]].loc, ids[w[4]].loc, (right.count << 16) | (left.count << 8) | left.elementSize);
            return true;
        }
        case OpExtInst:
            return emitExtended(raw);
        case OpFunctionCall:
            return emitCall(raw);
        case OpBranch:
            emitJumpTo(w[1]);
            return true;
        case OpBranchConditional: {
            const uint32_t trueLabel = w[2], falseLabel = w[3];
            emit(JumpIf, 0, ids[w[1]].loc, 0);
            const size_t index = program.code.size() - 1;
            if (hasPhis(trueLabel) || hasPhis(falseLabel)){
                // edges with copies get a stub each
                program.code[index].a = (uint32_t)program.code.size();
                emitJumpTo(trueLabel);
                program.code[index].c = (uint32_t)program.code.size();
                emitJumpTo(falseLabel);
            } else {
                pendingJumps.push_back({index, 0, trueLabel});
                pendingJumps.push_back({index, 2, falseLabel});
            }
            return true;
        }
        case OpReturn:
            emit(Ret);
            return true;
        case OpReturnValue:
            emitCopy(function.returnLoc, ids[w[1]].loc, sizeOf(function.returnType));
            emit(Ret);
            return true;
        case OpUnreachable:
            emit(Ret);
            return true;
        case OpImageSampleExplicitLod: {
            const uint32_t mask = w[5];
            if (mask != ImageOperandsLodMask)
                return fail("an explicit-lod sample with operands other than the lod");
            emit(Sample, ids[w[2]].loc, ids[w[3]].loc, ids[w[4]].loc, ids[w[6]].loc);
            return true;
        }
        default:
            return fail(std::string("unsupported SPIR-V instruction, opcode ") + std::to_string((int)raw.op));
    }
}

bool SpirvCompiler::emitExtended(const RawInstruction& raw){
    const uint32_t* w = raw.words;
    const uint32_t dst = ids[w[2]].loc;
    const uint32_t n = componentCount(w[1]);
    const uint32_t a = raw.count > 5 ? ids[w[5]].loc : 0;
    const uint32_t b = raw.count > 6 ? ids[w[6]].loc : 0;
    const uint32_t c = raw.count > 7 ? ids[w[7]].loc : 0;
    const uint32_t argumentN = raw.count > 5 ? componentCount(ids[w[5]].type) : n;
    const auto arguments = [&](uint32_t count){
        return raw.count == 5 + count || fail("an extended instruction with the wrong number of arguments");
    };
    const auto family2 = [&](uint32_t base){ emit(base + n - 1, dst, a, b); return arguments(2); };
    const auto family1 = [&](uint32_t base){ emit(base + n - 1, dst, a); return arguments(1); };
    const auto family3 = [&](uint32_t base){ emit(base + n - 1, dst, a, b, c); return arguments(3); };
    const auto math1 = [&](uint32_t index){ emit(Math1, dst, a, index, n); return arguments(1); };
    const auto math2 = [&](uint32_t index){ emit(Math2, dst, a, b, (index << 8) | n); return arguments(2); };
    switch (w[4]){
        case GLSLstd450Round: return family1(Round1);
        case GLSLstd450Trunc: return family1(Trunc1);
        case GLSLstd450FAbs: return family1(FAbs1);
        case GLSLstd450SAbs: return family1(SAbs1);
        case GLSLstd450FSign: return family1(FSign1);
        case GLSLstd450SSign: return family1(SSign1);
        case GLSLstd450Floor: return family1(Floor1);
        case GLSLstd450Ceil: return family1(Ceil1);
        case GLSLstd450Fract: return family1(Fract1);
        case GLSLstd450Sqrt: return family1(Sqrt1);
        case GLSLstd450Radians: return math1(0);
        case GLSLstd450Degrees: return math1(1);
        case GLSLstd450Sin: return math1(2);
        case GLSLstd450Cos: return math1(3);
        case GLSLstd450Tan: return math1(4);
        case GLSLstd450Asin: return math1(5);
        case GLSLstd450Acos: return math1(6);
        case GLSLstd450Atan: return math1(7);
        case GLSLstd450Sinh: return math1(8);
        case GLSLstd450Cosh: return math1(9);
        case GLSLstd450Tanh: return math1(10);
        case GLSLstd450Exp: return math1(11);
        case GLSLstd450Log: return math1(12);
        case GLSLstd450Exp2: return math1(13);
        case GLSLstd450Log2: return math1(14);
        case GLSLstd450Atan2: return math2(0);
        case GLSLstd450Pow: return math2(1);
        case GLSLstd450FMin: return family2(FMin1);
        case GLSLstd450FMax: return family2(FMax1);
        case GLSLstd450UMin: return family2(UMin1);
        case GLSLstd450UMax: return family2(UMax1);
        case GLSLstd450SMin: return family2(SMin1);
        case GLSLstd450SMax: return family2(SMax1);
        case GLSLstd450FClamp: return family3(Clamp1);
        case GLSLstd450SClamp: return family3(SClamp1);
        case GLSLstd450FMix: return family3(Mix1);
        case GLSLstd450Step: return family2(Step1);
        case GLSLstd450SmoothStep: return family3(SmoothStep1);
        case GLSLstd450Length:
            emit(Length1 + argumentN - 1, dst, a);
            return arguments(1);
        case GLSLstd450Distance:
            emit(Distance, dst, a, b, argumentN);
            return arguments(2);
        case GLSLstd450Normalize:
            emit(Normalize1 + n - 1, dst, a);
            return arguments(1);
        case GLSLstd450Cross:
            emit(Cross, dst, a, b);
            return arguments(2);
        default:
            return fail(std::string("unsupported GLSL.std.450 instruction ") + std::to_string(w[4]));
    }
}

std::unique_ptr<SpirvProgram> SpirvProgram::load(const uint32_t* words, size_t wordCount, std::string& error){
    std::unique_ptr<SpirvProgram> program(new SpirvProgram());
    SpirvCompiler compiler(words, wordCount, *program);
    if (!compiler.compile(error))
        return nullptr;
    return program;
}

int SpirvProgram::textureSlot(const std::string& name) const {
    auto found = textureNames.find(name);
    return found == textureNames.end() ? -1 : found->second;
}

SpirvMachine::SpirvMachine(const SpirvProgram& program) : program(program), registers(program.initial) {
    textures.resize(program.textureNames.size(), nullptr);
    callStack.resize(program.maxCallDepth + 2);
}

void SpirvMachine::setUniforms(const float* values, size_t count){
    count = std::min<size_t>(count, program.uniformSize);
    if (count > 0)
        std::memcpy(&registers[program.uniformLoc], values, count * sizeof(float));
}

void SpirvMachine::setTexture(int slot, const SpirvImage* image){
    if (slot >= 0 && slot < (int)textures.size())
        textures[slot] = image;
}

SpirvMachine::Result SpirvMachine::shade(float u, float v, float rgba[4]){
    if (program.inputSize >= 2){
        registers[program.inputLoc].f = u;
        registers[program.inputLoc + 1].f = v;
    }
    const Result result = execute();
    if (program.outputSize >= 4)
        std::memcpy(rgba, &registers[program.outputLoc], 4 * sizeof(float));
    return result;
}

// With GCC and Clang each operation jumps straight to the next one: the shared indirect branch of
// a switch mispredicts on nearly every instruction, and this loop is the whole cost of tier 3.
#if defined(__GNUC__)
#define OP_THREADED 1
#define OP_CASE_(NAME) case NAME: label_##NAME:
#define OP_NEXT { I = pc++; goto *dispatch[I->op]; }
#else
#define OP_THREADED 0
#define OP_CASE_(NAME) case NAME:
#define OP_NEXT break;
#endif
#define OP_CASE(NAME) OP_CASE_(NAME)

#define CASE_F2(NAME, F) \
    OP_CASE(NAME##1) r[I->a].f = F(r[I->b].f, r[I->c].f); OP_NEXT \
    OP_CASE(NAME##2) r[I->a].f = F(r[I->b].f, r[I->c].f); r[I->a + 1].f = F(r[I->b + 1].f, r[I->c + 1].f); OP_NEXT \
    OP_CASE(NAME##3) r[I->a].f = F(r[I->b].f, r[I->c].f); r[I->a + 1].f = F(r[I->b + 1].f, r[I->c + 1].f); r[I->a + 2].f = F(r[I->b + 2].f, r[I->c + 2].f); OP_NEXT \
    OP_CASE(NAME##4) r[I->a].f = F(r[I->b].f, r[I->c].f); r[I->a + 1].f = F(r[I->b + 1].f, r[I->c + 1].f); r[I->a + 2].f = F(r[I->b + 2].f, r[I->c + 2].f); r[I->a + 3].f = F(r[I->b + 3].f, r[I->c + 3].f); OP_NEXT

#define CASE_F1(NAME, F) \
    OP_CASE(NAME##1) r[I->a].f = F(r[I->b].f); OP_NEXT \
    OP_CASE(NAME##2) r[I->a].f = F(r[I->b].f); r[I->a + 1].f = F(r[I->b + 1].f); OP_NEXT \
    OP_CASE(NAME##3) r[I->a].f = F(r[I->b].f); r[I->a + 1].f = F(r[I->b + 1].f); r[I->a + 2].f = F(r[I->b + 2].f); OP_NEXT \
    OP_CASE(NAME##4) r[I->a].f = F(r[I->b].f); r[I->a + 1].f = F(r[I->b + 1].f); r[I->a + 2].f = F(r[I->b + 2].f); r[I->a + 3].f = F(r[I->b + 3].f); OP_NEXT

#define CASE_F3(NAME, F) \
    OP_CASE(NAME##1) r[I->a].f = F(r[I->b].f, r[I->c].f, r[I->d].f); OP_NEXT \
    OP_CASE(NAME##2) r[I->a].f = F(r[I->b].f, r[I->c].f, r[I->d].f); r[I->a + 1].f = F(r[I->b + 1].f, r[I->c + 1].f, r[I->d + 1].f); OP_NEXT \
    OP_CASE(NAME##3) r[I->a].f = F(r[I->b].f, r[I->c].f, r[I->d].f); r[I->a + 1].f = F(r[I->b + 1].f, r[I->c + 1].f, r[I->d + 1].f); r[I->a + 2].f = F(r[I->b + 2].f, r[I->c + 2].f, r[I->d + 2].f); OP_NEXT \
    OP_CASE(NAME##4) r[I->a].f = F(r[I->b].f, r[I->c].f, r[I->d].f); r[I->a + 1].f = F(r[I->b + 1].f, r[I->c + 1].f, r[I->d + 1].f); r[I->a + 2].f = F(r[I->b + 2].f, r[I->c + 2].f, r[I->d + 2].f); r[I->a + 3].f = F(r[I->b + 3].f, r[I->c + 3].f, r[I->d + 3].f); OP_NEXT

// F(x, y) on one union member; R is the result's
#define CASE_I2(NAME, M, F, R) \
    OP_CASE(NAME##1) r[I->a].R = F(r[I->b].M, r[I->c].M); OP_NEXT \
    OP_CASE(NAME##2) r[I->a].R = F(r[I->b].M, r[I->c].M); r[I->a + 1].R = F(r[I->b + 1].M, r[I->c + 1].M); OP_NEXT \
    OP_CASE(NAME##3) r[I->a].R = F(r[I->b].M, r[I->c].M); r[I->a + 1].R = F(r[I->b + 1].M, r[I->c + 1].M); r[I->a + 2].R = F(r[I->b + 2].M, r[I->c + 2].M); OP_NEXT \
    OP_CASE(NAME##4) r[I->a].R = F(r[I->b].M, r[I->c].M); r[I->a + 1].R = F(r[I->b + 1].M, r[I->c + 1].M); r[I->a + 2].R = F(r[I->b + 2].M, r[I->c + 2].M); r[I->a + 3].R = F(r[I->b + 3].M, r[I->c + 3].M); OP_NEXT

#define CASE_U1(NAME, M, F, R) \
    OP_CASE(NAME##1) r[I->a].R = F(r[I->b].M); OP_NEXT \
    OP_CASE(NAME##2) r[I->a].R = F(r[I->b].M); r[I->a + 1].R = F(r[I->b + 1].M); OP_NEXT \
    OP_CASE(NAME##3) r[I->a].R = F(r[I->b].M); r[I->a + 1].R = F(r[I->b + 1].M); r[I->a + 2].R = F(r[I->b + 2].M); OP_NEXT \
    OP_CASE(NAME##4) r[I->a].R = F(r[I->b].M); r[I->a + 1].R = F(r[I->b + 1].M); r[I->a + 2].R = F(r[I->b + 2].M); r[I->a + 3].R = F(r[I->b + 3].M); OP_NEXT

namespace {
    inline float opAdd(float x, float y){ return x + y; }
    inline float opSub(float x, float y){ return x - y; }
    inline float opMul(float x, float y){ return x * y; }
    inline float opDiv(float x, float y){ return x / y; }
    inline float opStep(float edge, float x){ return x < edge ? 0.0f : 1.0f; }
    inline float opNeg(float x){ return -x; }
    inline float opAbs(float x){ return std::fabs(x); }
    inline float opCeil(float x){ return ceilFast(x); }
    inline float opFract(float x){ return x - floorFast(x); }
    inline float opSqrt(float x){ return std::sqrt(x); }
    inline float opTrunc(float x){ return truncFast(x); }
    // to nearest, ties to even: what llvmpipe, lavapipe and the common GPUs do for round()
    inline float opRound(float x){ return std::nearbyint(x); }
    inline float opMix(float x, float y, float a){ return x * (1.0f - a) + y * a; }
    inline int32_t opIAdd(int32_t x, int32_t y){ return (int32_t)((uint32_t)x + (uint32_t)y); }
    inline int32_t opISub(int32_t x, int32_t y){ return (int32_t)((uint32_t)x - (uint32_t)y); }
    inline int32_t opIMul(int32_t x, int32_t y){ return (int32_t)((uint32_t)x * (uint32_t)y); }
    inline uint32_t opAnd(uint32_t x, uint32_t y){ return x & y; }
    inline uint32_t opOr(uint32_t x, uint32_t y){ return x | y; }
    inline uint32_t opXor(uint32_t x, uint32_t y){ return x ^ y; }
    inline uint32_t opShl(uint32_t x, uint32_t y){ return x << (y & 31u); }
    inline uint32_t opShrL(uint32_t x, uint32_t y){ return x >> (y & 31u); }
    inline int32_t opShrA(int32_t x, int32_t y){ return x >> ((uint32_t)y & 31u); }
    inline uint32_t opUMin(uint32_t x, uint32_t y){ return y < x ? y : x; }
    inline uint32_t opUMax(uint32_t x, uint32_t y){ return x < y ? y : x; }
    inline int32_t opSMin(int32_t x, int32_t y){ return y < x ? y : x; }
    inline int32_t opSMax(int32_t x, int32_t y){ return x < y ? y : x; }
    inline int32_t opINeg(int32_t x){ return (int32_t)(0u - (uint32_t)x); }
    inline uint32_t opNot(uint32_t x){ return ~x; }
    inline uint32_t opBoolNot(uint32_t x){ return x ^ 1u; }
    inline int32_t opSAbs(int32_t x){ return x < 0 ? opINeg(x) : x; }
    inline int32_t opSSign(int32_t x){ return x > 0 ? 1 : (x < 0 ? -1 : 0); }
    inline uint32_t cmpFEq(float x, float y){ return x == y; }
    inline uint32_t cmpFNe(float x, float y){ return !std::isnan(x) && !std::isnan(y) && x != y; }
    inline uint32_t cmpFLt(float x, float y){ return x < y; }
    inline uint32_t cmpFLe(float x, float y){ return x <= y; }
    inline uint32_t cmpFGt(float x, float y){ return x > y; }
    inline uint32_t cmpFGe(float x, float y){ return x >= y; }
    inline uint32_t cmpFUNe(float x, float y){ return x != y; }
    inline uint32_t cmpIEq(int32_t x, int32_t y){ return x == y; }
    inline uint32_t cmpINe(int32_t x, int32_t y){ return x != y; }
    inline uint32_t cmpSLt(int32_t x, int32_t y){ return x < y; }
    inline uint32_t cmpSLe(int32_t x, int32_t y){ return x <= y; }
    inline uint32_t cmpSGt(int32_t x, int32_t y){ return x > y; }
    inline uint32_t cmpSGe(int32_t x, int32_t y){ return x >= y; }
    inline uint32_t cmpULt(uint32_t x, uint32_t y){ return x < y; }
    inline uint32_t cmpULe(uint32_t x, uint32_t y){ return x <= y; }
    inline uint32_t cmpUGt(uint32_t x, uint32_t y){ return x > y; }
    inline uint32_t cmpUGe(uint32_t x, uint32_t y){ return x >= y; }
    inline uint32_t testNan(float x){ return x != x; }
    inline float fromInt(int32_t x){ return (float)x; }
    inline float fromUint(uint32_t x){ return (float)x; }
}

SpirvMachine::Result SpirvMachine::execute(){
    SpirvRegister* const r = registers.data();
    const uint64_t registerCount = registers.size();
    const SpirvInstruction* const code = program.code.data();
    const uint32_t* const tables = program.tables.data();
    uint32_t* const stack = callStack.data();
    const SpirvInstruction* pc = code + program.entry;
    uint32_t depth = 0;
    int64_t budget = backwardJumpLimit;
#if OP_THREADED
    static const void* const dispatch[] = {
#define OP_LABEL(NAME) &&label_##NAME,
        OP_LIST(OP_LABEL)
#undef OP_LABEL
    };
#endif
    const SpirvInstruction* I = nullptr;
    for (;;){
        I = pc++;
        switch (I->op){
            OP_CASE(Copy1) r[I->a] = r[I->b]; OP_NEXT
            OP_CASE(Copy2) { const SpirvRegister x = r[I->b], y = r[I->b + 1]; r[I->a] = x; r[I->a + 1] = y; OP_NEXT }
            OP_CASE(Copy3) { const SpirvRegister x = r[I->b], y = r[I->b + 1], z = r[I->b + 2]; r[I->a] = x; r[I->a + 1] = y; r[I->a + 2] = z; OP_NEXT }
            OP_CASE(Copy4) { const SpirvRegister x = r[I->b], y = r[I->b + 1], z = r[I->b + 2], w = r[I->b + 3]; r[I->a] = x; r[I->a + 1] = y; r[I->a + 2] = z; r[I->a + 3] = w; OP_NEXT }
            OP_CASE(CopyN) std::memmove(r + I->a, r + I->b, I->c * sizeof(SpirvRegister)); OP_NEXT
            OP_CASE(ClearN) std::memset(static_cast<void*>(r + I->a), 0, I->b * sizeof(SpirvRegister)); OP_NEXT

            CASE_F2(FAdd, opAdd)
            CASE_F2(FSub, opSub)
            CASE_F2(FMul, opMul)
            CASE_F2(FDiv, opDiv)
            CASE_F2(FMod, glslMod)
            CASE_F2(FMin, glslMin)
            CASE_F2(FMax, glslMax)
            CASE_F2(Step, opStep)
            CASE_F1(FNeg, opNeg)
            CASE_F1(FAbs, opAbs)
            CASE_F1(Floor, floorFast)
            CASE_F1(Ceil, opCeil)
            CASE_F1(Fract, opFract)
            CASE_F1(Sqrt, opSqrt)
            CASE_F1(FSign, glslSign)
            CASE_F1(Trunc, opTrunc)
            CASE_F1(Round, opRound)
            CASE_F3(Mix, opMix)
            CASE_F3(Clamp, glslClamp)
            CASE_F3(SmoothStep, glslSmoothStep)

            CASE_I2(IAdd, i, opIAdd, i)
            CASE_I2(ISub, i, opISub, i)
            CASE_I2(IMul, i, opIMul, i)
            CASE_I2(UDiv, u, safeUDiv, u)
            CASE_I2(SDiv, i, safeSDiv, i)
            CASE_I2(UMod, u, safeUMod, u)
            CASE_I2(SMod, i, safeSMod, i)
            CASE_I2(And, u, opAnd, u)
            CASE_I2(Or, u, opOr, u)
            CASE_I2(Xor, u, opXor, u)
            CASE_I2(Shl, u, opShl, u)
            CASE_I2(ShrL, u, opShrL, u)
            CASE_I2(UMin, u, opUMin, u)
            CASE_I2(UMax, u, opUMax, u)
            CASE_I2(SMin, i, opSMin, i)
            CASE_I2(SMax, i, opSMax, i)
            OP_CASE(ShrA1) r[I->a].i = opShrA(r[I->b].i, r[I->c].i); OP_NEXT
            OP_CASE(ShrA2) OP_CASE(ShrA3) OP_CASE(ShrA4) {
                const uint32_t n = I->op - ShrA1 + 1;
                for (uint32_t k = 0; k < n; k++)
                    r[I->a + k].i = opShrA(r[I->b + k].i, r[I->c + k].i);
                OP_NEXT
            }
            OP_CASE(SClamp1) OP_CASE(SClamp2) OP_CASE(SClamp3) OP_CASE(SClamp4) {
                const uint32_t n = I->op - SClamp1 + 1;
                for (uint32_t k = 0; k < n; k++)
                    r[I->a + k].i = opSMin(opSMax(r[I->b + k].i, r[I->c + k].i), r[I->d + k].i);
                OP_NEXT
            }
            CASE_U1(INeg, i, opINeg, i)
            CASE_U1(Not, u, opNot, u)
            CASE_U1(BoolNot, u, opBoolNot, u)
            CASE_U1(SAbs, i, opSAbs, i)
            CASE_U1(SSign, i, opSSign, i)

            CASE_I2(FEq, f, cmpFEq, u)
            CASE_I2(FNe, f, cmpFNe, u)
            CASE_I2(FLt, f, cmpFLt, u)
            CASE_I2(FLe, f, cmpFLe, u)
            CASE_I2(FGt, f, cmpFGt, u)
            CASE_I2(FGe, f, cmpFGe, u)
            CASE_I2(FUNe, f, cmpFUNe, u)
            CASE_I2(IEq, i, cmpIEq, u)
            CASE_I2(INe, i, cmpINe, u)
            CASE_I2(SLt, i, cmpSLt, u)
            CASE_I2(SLe, i, cmpSLe, u)
            CASE_I2(SGt, i, cmpSGt, u)
            CASE_I2(SGe, i, cmpSGe, u)
            CASE_I2(ULt, u, cmpULt, u)
            CASE_I2(ULe, u, cmpULe, u)
            CASE_I2(UGt, u, cmpUGt, u)
            CASE_I2(UGe, u, cmpUGe, u)

            CASE_U1(IsNan, f, testNan, u)

            CASE_U1(F2S, f, floatToInt, i)
            CASE_U1(F2U, f, floatToUint, u)
            CASE_U1(S2F, i, fromInt, f)
            CASE_U1(U2F, u, fromUint, f)

            OP_CASE(SelectS1) r[I->a] = r[I->b].u ? r[I->c] : r[I->d]; OP_NEXT
            OP_CASE(SelectS2) OP_CASE(SelectS3) OP_CASE(SelectS4) {
                const uint32_t n = I->op - SelectS1 + 1;
                const uint32_t from = r[I->b].u ? I->c : I->d;
                for (uint32_t k = 0; k < n; k++)
                    r[I->a + k] = r[from + k];
                OP_NEXT
            }
            OP_CASE(SelectV1) OP_CASE(SelectV2) OP_CASE(SelectV3) OP_CASE(SelectV4) {
                const uint32_t n = I->op - SelectV1 + 1;
                for (uint32_t k = 0; k < n; k++)
                    r[I->a + k] = r[I->b + k].u ? r[I->c + k] : r[I->d + k];
                OP_NEXT
            }
            OP_CASE(SelectSN) {
                const uint32_t from = r[I->b].u ? I->c : tables[I->d];
                std::memmove(r + I->a, r + from, tables[I->d + 1] * sizeof(SpirvRegister));
                OP_NEXT
            }
            OP_CASE(Any1) r[I->a].u = r[I->b].u; OP_NEXT
            OP_CASE(Any2) r[I->a].u = r[I->b].u | r[I->b + 1].u; OP_NEXT
            OP_CASE(Any3) r[I->a].u = r[I->b].u | r[I->b + 1].u | r[I->b + 2].u; OP_NEXT
            OP_CASE(Any4) r[I->a].u = r[I->b].u | r[I->b + 1].u | r[I->b + 2].u | r[I->b + 3].u; OP_NEXT
            OP_CASE(All1) r[I->a].u = r[I->b].u; OP_NEXT
            OP_CASE(All2) r[I->a].u = r[I->b].u & r[I->b + 1].u; OP_NEXT
            OP_CASE(All3) r[I->a].u = r[I->b].u & r[I->b + 1].u & r[I->b + 2].u; OP_NEXT
            OP_CASE(All4) r[I->a].u = r[I->b].u & r[I->b + 1].u & r[I->b + 2].u & r[I->b + 3].u; OP_NEXT

            OP_CASE(Dot1) r[I->a].f = r[I->b].f * r[I->c].f; OP_NEXT
            OP_CASE(Dot2) r[I->a].f = r[I->b].f * r[I->c].f + r[I->b + 1].f * r[I->c + 1].f; OP_NEXT
            OP_CASE(Dot3) r[I->a].f = r[I->b].f * r[I->c].f + r[I->b + 1].f * r[I->c + 1].f + r[I->b + 2].f * r[I->c + 2].f; OP_NEXT
            OP_CASE(Dot4) r[I->a].f = r[I->b].f * r[I->c].f + r[I->b + 1].f * r[I->c + 1].f + r[I->b + 2].f * r[I->c + 2].f + r[I->b + 3].f * r[I->c + 3].f; OP_NEXT
            OP_CASE(Length1) r[I->a].f = std::fabs(r[I->b].f); OP_NEXT
            OP_CASE(Length2) r[I->a].f = std::sqrt(r[I->b].f * r[I->b].f + r[I->b + 1].f * r[I->b + 1].f); OP_NEXT
            OP_CASE(Length3) r[I->a].f = std::sqrt(r[I->b].f * r[I->b].f + r[I->b + 1].f * r[I->b + 1].f + r[I->b + 2].f * r[I->b + 2].f); OP_NEXT
            OP_CASE(Length4) r[I->a].f = std::sqrt(r[I->b].f * r[I->b].f + r[I->b + 1].f * r[I->b + 1].f + r[I->b + 2].f * r[I->b + 2].f + r[I->b + 3].f * r[I->b + 3].f); OP_NEXT
            OP_CASE(Normalize1) OP_CASE(Normalize2) OP_CASE(Normalize3) OP_CASE(Normalize4) {
                const uint32_t n = I->op - Normalize1 + 1;
                float sum = 0.0f;
                for (uint32_t k = 0; k < n; k++)
                    sum += r[I->b + k].f * r[I->b + k].f;
                const float scale = 1.0f / std::sqrt(sum);
                for (uint32_t k = 0; k < n; k++)
                    r[I->a + k].f = r[I->b + k].f * scale;
                OP_NEXT
            }
            OP_CASE(Distance) {
                float sum = 0.0f;
                for (uint32_t k = 0; k < I->d; k++){
                    const float x = r[I->b + k].f - r[I->c + k].f;
                    sum += x * x;
                }
                r[I->a].f = std::sqrt(sum);
                OP_NEXT
            }
            OP_CASE(Cross) {
                const float x = r[I->b + 1].f * r[I->c + 2].f - r[I->b + 2].f * r[I->c + 1].f;
                const float y = r[I->b + 2].f * r[I->c].f - r[I->b].f * r[I->c + 2].f;
                const float z = r[I->b].f * r[I->c + 1].f - r[I->b + 1].f * r[I->c].f;
                r[I->a].f = x;
                r[I->a + 1].f = y;
                r[I->a + 2].f = z;
                OP_NEXT
            }
            OP_CASE(Vts1) r[I->a].f = r[I->b].f * r[I->c].f; OP_NEXT
            OP_CASE(Vts2) { const float s = r[I->c].f; r[I->a].f = r[I->b].f * s; r[I->a + 1].f = r[I->b + 1].f * s; OP_NEXT }
            OP_CASE(Vts3) { const float s = r[I->c].f; r[I->a].f = r[I->b].f * s; r[I->a + 1].f = r[I->b + 1].f * s; r[I->a + 2].f = r[I->b + 2].f * s; OP_NEXT }
            OP_CASE(Vts4) { const float s = r[I->c].f; r[I->a].f = r[I->b].f * s; r[I->a + 1].f = r[I->b + 1].f * s; r[I->a + 2].f = r[I->b + 2].f * s; r[I->a + 3].f = r[I->b + 3].f * s; OP_NEXT }
            OP_CASE(VtsN) {
                const float s = r[I->c].f;
                for (uint32_t k = 0; k < I->d; k++)
                    r[I->a + k].f = r[I->b + k].f * s;
                OP_NEXT
            }
            OP_CASE(MatVec) {
                const uint32_t cols = I->d >> 8, rows = I->d & 0xff;
                float out[4] = {};
                for (uint32_t col = 0; col < cols; col++){
                    const float x = r[I->c + col].f;
                    for (uint32_t row = 0; row < rows; row++)
                        out[row] += r[I->b + col * rows + row].f * x;
                }
                for (uint32_t row = 0; row < rows; row++)
                    r[I->a + row].f = out[row];
                OP_NEXT
            }
            OP_CASE(VecMat) {
                const uint32_t cols = I->d >> 8, rows = I->d & 0xff;
                float out[4] = {};
                for (uint32_t col = 0; col < cols; col++){
                    for (uint32_t row = 0; row < rows; row++)
                        out[col] += r[I->b + row].f * r[I->c + col * rows + row].f;
                }
                for (uint32_t col = 0; col < cols; col++)
                    r[I->a + col].f = out[col];
                OP_NEXT
            }
            OP_CASE(MatMat) {
                const uint32_t rightCols = I->d >> 16, leftCols = (I->d >> 8) & 0xff, rows = I->d & 0xff;
                float out[16] = {};
                for (uint32_t col = 0; col < rightCols; col++){
                    for (uint32_t k = 0; k < leftCols; k++){
                        const float x = r[I->c + col * leftCols + k].f;
                        for (uint32_t row = 0; row < rows; row++)
                            out[col * rows + row] += r[I->b + k * rows + row].f * x;
                    }
                }
                for (uint32_t k = 0; k < rightCols * rows; k++)
                    r[I->a + k].f = out[k];
                OP_NEXT
            }
            OP_CASE(Shuffle) {
                SpirvRegister out[4];
                for (uint32_t k = 0; k < I->c; k++)
                    out[k] = r[tables[I->b + k]];
                for (uint32_t k = 0; k < I->c; k++)
                    r[I->a + k] = out[k];
                OP_NEXT
            }
            OP_CASE(ExtractDyn) {
                const uint32_t k = (uint32_t)std::clamp<int64_t>(r[I->c].i, 0, (int64_t)I->d - 1);
                r[I->a] = r[I->b + k];
                OP_NEXT
            }
            OP_CASE(InsertDyn) {
                const uint32_t k = (uint32_t)std::clamp<int64_t>(r[I->c].i, 0, (int64_t)I->d - 1);
                r[I->a + k] = r[I->b];
                OP_NEXT
            }
            OP_CASE(PtrIndex) {
                const uint32_t stride = tables[I->d], count = tables[I->d + 1];
                const uint32_t k = (uint32_t)std::clamp<int64_t>(r[I->c].i, 0, (int64_t)count - 1);
                r[I->a].u = I->b + k * stride;
                OP_NEXT
            }
            OP_CASE(PtrIndexAdd) {
                const uint32_t stride = tables[I->d], count = tables[I->d + 1];
                const uint32_t k = (uint32_t)std::clamp<int64_t>(r[I->c].i, 0, (int64_t)count - 1);
                r[I->a].u = r[I->b].u + k * stride;
                OP_NEXT
            }
            OP_CASE(LoadInd)
                if (r[I->b].u + (uint64_t)I->c > registerCount)
                    return Result::TooLong;
                std::memmove(r + I->a, r + r[I->b].u, I->c * sizeof(SpirvRegister));
                OP_NEXT
            OP_CASE(StoreInd)
                if (r[I->a].u + (uint64_t)I->c > registerCount)
                    return Result::TooLong;
                std::memmove(r + r[I->a].u, r + I->b, I->c * sizeof(SpirvRegister));
                OP_NEXT
            OP_CASE(Math1) {
                const Math1Function function = math1Functions[I->c];
                for (uint32_t k = 0; k < I->d; k++)
                    r[I->a + k].f = function(r[I->b + k].f);
                OP_NEXT
            }
            OP_CASE(Math2) {
                const Math2Function function = math2Functions[I->d >> 8];
                const uint32_t n = I->d & 0xff;
                for (uint32_t k = 0; k < n; k++)
                    r[I->a + k].f = function(r[I->b + k].f, r[I->c + k].f);
                OP_NEXT
            }
            OP_CASE(Jump) {
                const SpirvInstruction* target = code + I->a;
                if (target <= pc && --budget < 0)
                    return Result::TooLong;
                pc = target;
                OP_NEXT
            }
            OP_CASE(JumpIf) {
                const SpirvInstruction* target = code + (r[I->b].u ? I->a : I->c);
                if (target <= pc && --budget < 0)
                    return Result::TooLong;
                pc = target;
                OP_NEXT
            }
            OP_CASE(Call)
                stack[depth++] = (uint32_t)(pc - code);
                pc = code + I->a;
                OP_NEXT
            OP_CASE(Ret)
                if (depth == 0)
                    return Result::Ok;
                pc = code + stack[--depth];
                OP_NEXT
            OP_CASE(Sample) {
                float out[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                const uint32_t slot = r[I->b].u;
                const SpirvImage* image = slot < textures.size() ? textures[slot] : nullptr;
                if (image != nullptr){
                    const float lod = r[I->d].f;
                    const float u = r[I->c].f, v = r[I->c + 1].f;
                    const int top = image->mipCount() - 1;
                    const float clamped = glslClamp(lod, 0.0f, (float)top);
                    const int low = (int)clamped;
                    const float blend = clamped - (float)low;
                    image->sampleBilinear(u, v, low, out);
                    if (blend > 0.0f && low < top){
                        float next[4];
                        image->sampleBilinear(u, v, low + 1, next);
                        for (int k = 0; k < 4; k++)
                            out[k] = out[k] * (1.0f - blend) + next[k] * blend;
                    }
                }
                for (uint32_t k = 0; k < 4; k++)
                    r[I->a + k].f = out[k];
                OP_NEXT
            }
            default:
                return Result::TooLong;
        }
    }
}

float jasperengine::editor::procmat::roundedToHalf(float value){
    if (!std::isfinite(value) || value == 0.0f)
        return value;
    const float magnitude = std::fabs(value);
    // past the largest half, what rounds up to 65536 is infinity
    if (magnitude >= 65520.0f)
        return std::copysign(HUGE_VALF, value);
    int exponent = 0;
    std::frexp(magnitude, &exponent);
    // 11 significant bits, and subnormals stop at 2^-24
    const float step = std::ldexp(1.0f, std::max(exponent - 11, -24));
    return std::copysign(std::nearbyint(magnitude / step) * step, value);
}

SpirvImage::SpirvImage(int width, int height, std::vector<float> rgba) : SpirvImage(width, height, std::move(rgba), true, false) {}

SpirvImage::SpirvImage(int width, int height, std::vector<float> rgba, bool mipChain, bool halfPrecision){
    // an image that is not what it says becomes one transparent texel, not a read out of bounds
    if (width <= 0 || height <= 0 || rgba.size() < (size_t)width * height * 4){
        width = height = 1;
        rgba.assign(4, 0.0f);
    }
    if (halfPrecision){
        for (float& value : rgba)
            value = roundedToHalf(value);
    }
    mips.push_back({width, height, std::move(rgba)});
    while (mipChain && (mips.back().width > 1 || mips.back().height > 1)){
        const Mip& previous = mips.back();
        Mip next;
        next.width = std::max(1, previous.width / 2);
        next.height = std::max(1, previous.height / 2);
        next.rgba.resize((size_t)next.width * next.height * 4);
        for (int y = 0; y < next.height; y++){
            for (int x = 0; x < next.width; x++){
                float sum[4] = {};
                const int fromX = std::min(x * 2, previous.width - 1), toX = std::min(x * 2 + 1, previous.width - 1);
                const int fromY = std::min(y * 2, previous.height - 1), toY = std::min(y * 2 + 1, previous.height - 1);
                for (int sy : {fromY, toY}){
                    for (int sx : {fromX, toX}){
                        const float* p = &previous.rgba[((size_t)sy * previous.width + sx) * 4];
                        for (int k = 0; k < 4; k++)
                            sum[k] += p[k];
                    }
                }
                float* out = &next.rgba[((size_t)y * next.width + x) * 4];
                for (int k = 0; k < 4; k++)
                    out[k] = halfPrecision ? roundedToHalf(sum[k] * 0.25f) : sum[k] * 0.25f;
            }
        }
        mips.push_back(std::move(next));
    }
}

void SpirvImage::sampleBilinear(float u, float v, int mip, float out[4]) const {
    const Mip& level = mips[std::clamp(mip, 0, (int)mips.size() - 1)];
    // NaN and infinity would reach an int conversion below; a tier has no defined answer for them
    if (!std::isfinite(u))
        u = 0.0f;
    if (!std::isfinite(v))
        v = 0.0f;
    const float x = (u - std::floor(u)) * (float)level.width - 0.5f;
    const float y = (v - std::floor(v)) * (float)level.height - 0.5f;
    const float floorX = std::floor(x), floorY = std::floor(y);
    const float tx = x - floorX, ty = y - floorY;
    const int wrapX0 = ((int)floorX % level.width + level.width) % level.width;
    const int wrapY0 = ((int)floorY % level.height + level.height) % level.height;
    const int wrapX1 = (wrapX0 + 1) % level.width;
    const int wrapY1 = (wrapY0 + 1) % level.height;
    const float* p00 = &level.rgba[((size_t)wrapY0 * level.width + wrapX0) * 4];
    const float* p10 = &level.rgba[((size_t)wrapY0 * level.width + wrapX1) * 4];
    const float* p01 = &level.rgba[((size_t)wrapY1 * level.width + wrapX0) * 4];
    const float* p11 = &level.rgba[((size_t)wrapY1 * level.width + wrapX1) * 4];
    for (int k = 0; k < 4; k++){
        const float top = p00[k] * (1.0f - tx) + p10[k] * tx;
        const float bottom = p01[k] * (1.0f - tx) + p11[k] * tx;
        out[k] = top * (1.0f - ty) + bottom * ty;
    }
}
