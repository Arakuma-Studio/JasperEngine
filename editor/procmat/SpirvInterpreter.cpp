// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpirvInterpreter.h"

#include "SPIRV/GLSL.std.450.h"
#include "SPIRV/spirv.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>

using namespace jasperengine::editor::procmat;

namespace {

// A runaway loop stops a pixel instead of the whole bake: a real node runs far fewer backward jumps
const int64_t backwardJumpLimit = 1 << 22;

#define FAMILY(NAME) NAME##1, NAME##2, NAME##3, NAME##4

enum Op : uint32_t {
    FAMILY(Copy), CopyN, ClearN,
    // float, per component
    FAMILY(FAdd), FAMILY(FSub), FAMILY(FMul), FAMILY(FDiv), FAMILY(FMod), FAMILY(FRem), FAMILY(FMin), FAMILY(FMax), FAMILY(Step),
    FAMILY(FNeg), FAMILY(FAbs), FAMILY(Floor), FAMILY(Ceil), FAMILY(Fract), FAMILY(Sqrt), FAMILY(FSign), FAMILY(Trunc), FAMILY(Round),
    FAMILY(Mix), FAMILY(Clamp), FAMILY(SmoothStep), FAMILY(Fma),
    // integer, per component
    FAMILY(IAdd), FAMILY(ISub), FAMILY(IMul), FAMILY(UDiv), FAMILY(SDiv), FAMILY(UMod), FAMILY(SMod), FAMILY(SRem),
    FAMILY(And), FAMILY(Or), FAMILY(Xor), FAMILY(Shl), FAMILY(ShrL), FAMILY(ShrA),
    FAMILY(UMin), FAMILY(UMax), FAMILY(SMin), FAMILY(SMax), FAMILY(UClamp), FAMILY(SClamp),
    FAMILY(INeg), FAMILY(Not), FAMILY(BoolNot), FAMILY(SAbs), FAMILY(SSign),
    // comparisons
    FAMILY(FEq), FAMILY(FNe), FAMILY(FLt), FAMILY(FLe), FAMILY(FGt), FAMILY(FGe),
    FAMILY(FUEq), FAMILY(FUNe), FAMILY(FULt), FAMILY(FULe), FAMILY(FUGt), FAMILY(FUGe),
    FAMILY(IEq), FAMILY(INe), FAMILY(SLt), FAMILY(SLe), FAMILY(SGt), FAMILY(SGe), FAMILY(ULt), FAMILY(ULe), FAMILY(UGt), FAMILY(UGe),
    FAMILY(IsNan), FAMILY(IsInf),
    // conversions
    FAMILY(F2S), FAMILY(F2U), FAMILY(S2F), FAMILY(U2F),
    // selection
    FAMILY(SelectS), FAMILY(SelectV), SelectSN,
    FAMILY(Any), FAMILY(All),
    // vectors and matrices; these read their sources before they write
    FAMILY(Dot), FAMILY(Length), FAMILY(Normalize), Cross, Distance,
    FAMILY(Vts), VtsN, MatVec, VecMat, MatMat, Outer, Transpose, Determinant, Inverse,
    Reflect, Refract, FaceForward,
    Shuffle, ExtractDyn, InsertDyn,
    // pointers: a register holds a register number
    PtrIndex, PtrIndexAdd, LoadInd, StoreInd,
    // the rest of GLSL.std.450, by function
    Math1, Math2,
    // packing
    PackUnorm4x8, UnpackUnorm4x8, PackSnorm4x8, UnpackSnorm4x8, PackUnorm2x16, UnpackUnorm2x16, PackSnorm2x16, UnpackSnorm2x16, PackHalf2x16, UnpackHalf2x16,
    // control
    Jump, JumpIf, Switch, Call, Ret, Kill,
    Sample,
};

typedef float (*Math1Function)(float);
typedef float (*Math2Function)(float, float);

float mathRadians(float x){ return x * 0.01745329251994f; }
float mathDegrees(float x){ return x * 57.29577951308f; }
float mathInverseSqrt(float x){ return 1.0f / std::sqrt(x); }
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
float mathAsinh(float x){ return std::asinh(x); }
float mathAcosh(float x){ return std::acosh(x); }
float mathAtanh(float x){ return std::atanh(x); }
float mathExp(float x){ return std::exp(x); }
float mathLog(float x){ return std::log(x); }
float mathAtan2(float y, float x){ return std::atan2(y, x); }
float mathPow(float x, float y){ return std::pow(x, y); }

const Math1Function math1Functions[] = {
    mathRadians, mathDegrees, mathSin, mathCos, mathTan, mathAsin, mathAcos, mathAtan, mathSinh, mathCosh, mathTanh,
    mathAsinh, mathAcosh, mathAtanh, mathExp, mathLog, mathExp2, mathLog2, mathInverseSqrt,
};
const Math2Function math2Functions[] = {mathAtan2, mathPow};

inline float floorFast(float x){ return std::floor(x); }
inline float glslMod(float x, float y){ return x - y * std::floor(x / y); }
inline float glslMin(float x, float y){ return y < x ? y : x; }
inline float glslMax(float x, float y){ return x < y ? y : x; }
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
inline int32_t safeSRem(int32_t x, int32_t y){
    if (y == 0 || y == -1)
        return 0;
    return x % y;
}
inline int32_t safeSMod(int32_t x, int32_t y){
    const int32_t r = safeSRem(x, y);
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
        if (!parseModule() || !allocateFunctions() || !emitProgram()){
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
    std::vector<uint32_t> interfaceInputs;
    uint32_t bound = 0;
    std::vector<uint32_t> privateInits;
    std::map<uint32_t, std::string> names;
    std::set<uint32_t> constantIds;

    uint32_t allocate(uint32_t size){
        const uint32_t loc = (uint32_t)program.initial.size();
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

    bool parseModule(){
        if (wordCount < 5 || words[0] != spv::MagicNumber)
            return fail("not a SPIR-V module");
        bound = words[3];
        if (bound > (1u << 24))
            return fail("id bound too large");
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
            const uint32_t* w = words + i;
            if (!parseInstruction(op, w, count, function, block, position))
                return false;
            i += count;
        }
        if (entryFunction == 0)
            return fail("no fragment entry point");
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
            case spv::OpMemberDecorate: case spv::OpSourceContinued: case spv::OpExtension: case spv::OpSelectionMerge: case spv::OpLoopMerge: case spv::OpNop:
                return true;
            case spv::OpName:
                names[w[1]] = text(w + 2, count - 2);
                return true;
            case spv::OpDecorate:
                return true;
            case spv::OpTypeVoid: case spv::OpTypeBool: case spv::OpTypeFloat: case spv::OpTypeInt: case spv::OpTypeVector:
            case spv::OpTypeMatrix: case spv::OpTypeArray: case spv::OpTypeRuntimeArray: case spv::OpTypeStruct: case spv::OpTypePointer:
            case spv::OpTypeFunction: case spv::OpTypeImage: case spv::OpTypeSampler: case spv::OpTypeSampledImage:
                return parseType(op, w, count);
            case spv::OpConstant: case spv::OpConstantTrue: case spv::OpConstantFalse: case spv::OpConstantNull: case spv::OpConstantComposite: case spv::OpUndef:
                if (function == nullptr)
                    return parseConstant(op, w, count);
                break;
            case spv::OpVariable:
                if (function == nullptr)
                    return parseGlobal(w, count);
                function->variables.push_back(w[2]);
                ids[w[2]].type = w[1];
                ids[w[2]].kind = IdKind::StaticPointer;
                ids[w[2]].pointee = types[w[1]].element;
                ids[w[2]].root = w[2];
                ids[w[2]].loc = allocate(types[types[w[1]].element].size);
                if (count > 4){
                    if (ids[w[4]].kind != IdKind::Register)
                        return fail("a variable initializer that is not a constant");
                    variableInit[w[2]] = w[4];
                }
                return true;
            case spv::OpFunction:
                function = &functions[w[2]];
                function->id = w[2];
                function->returnType = w[1];
                functionOrder.push_back(w[2]);
                ids[w[2]].kind = IdKind::Function;
                position = 0;
                block = 0;
                return true;
            case spv::OpFunctionParameter:
                if (function == nullptr)
                    return fail("a parameter outside a function");
                function->params.push_back(w[2]);
                ids[w[2]].type = w[1];
                return true;
            case spv::OpFunctionEnd:
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
            return fail(std::string("unsupported instruction ") + std::to_string((int)op) + " outside a function");
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
        type.kind = op;
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
            case spv::OpTypeVector: case spv::OpTypeMatrix:
                type.element = w[2];
                type.count = w[3];
                type.elementSize = types[w[2]].size;
                type.size = type.count * type.elementSize;
                break;
            case spv::OpTypeArray: {
                type.element = w[2];
                type.count = constantValues[w[3]];
                type.elementSize = types[w[2]].size;
                type.size = type.count * type.elementSize;
                break;
            }
            case spv::OpTypeStruct:
                for (uint32_t i = 2; i < count; i++){
                    type.members.push_back(w[i]);
                    type.size += types[w[i]].size;
                }
                break;
            case spv::OpTypePointer:
                type.storage = (spv::StorageClass)w[2];
                type.element = w[3];
                break;
            case spv::OpTypeFunction:
                break;
            case spv::OpTypeImage: case spv::OpTypeSampler: case spv::OpTypeSampledImage:
                type.size = 1; // the register holds the texture's slot
                break;
            default:
                return fail("unsupported type");
        }
        return true;
    }

    std::map<uint32_t, uint32_t> constantValues;
    std::map<uint32_t, uint32_t> variableInit;

    bool parseConstant(spv::Op op, const uint32_t* w, uint32_t count){
        const uint32_t type = w[1];
        const uint32_t id = w[2];
        const uint32_t loc = allocate(types[type].size);
        ids[id].kind = IdKind::Register;
        ids[id].type = type;
        ids[id].loc = loc;
        constantIds.insert(id);
        switch (op){
            case spv::OpConstant:
                program.initial[loc].u = w[3];
                constantValues[id] = w[3];
                break;
            case spv::OpConstantTrue:
                program.initial[loc].u = 1;
                break;
            case spv::OpConstantFalse: case spv::OpConstantNull: case spv::OpUndef:
                break;
            case spv::OpConstantComposite: {
                uint32_t offset = loc;
                for (uint32_t i = 3; i < count; i++){
                    const IdInfo& part = ids[w[i]];
                    const uint32_t size = types[part.type].size;
                    for (uint32_t k = 0; k < size; k++)
                        program.initial[offset + k] = program.initial[part.loc + k];
                    offset += size;
                }
                break;
            }
            default:
                return fail("unsupported constant");
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
                break;
            case spv::StorageClassUniform:
                if (program.uniformSize != 0)
                    return fail("more than one uniform block");
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
                if (count > 4)
                    privateInits.push_back(id), variableInit[id] = w[4];
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
            case spv::OpLabel: case spv::OpSelectionMerge: case spv::OpLoopMerge: case spv::OpBranch: case spv::OpKill:
            case spv::OpReturn: case spv::OpUnreachable: case spv::OpNop:
                break;
            case spv::OpBranchConditional: case spv::OpSwitch: case spv::OpReturnValue:
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
            case spv::OpStore: case spv::OpBranch: case spv::OpBranchConditional: case spv::OpSwitch: case spv::OpReturn:
            case spv::OpReturnValue: case spv::OpKill: case spv::OpUnreachable: case spv::OpNop: case spv::OpLabel:
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
        for (uint32_t functionId : functionOrder){
            std::set<uint32_t> visiting;
            if (!checkAcyclic(functionId, visiting))
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

    bool checkAcyclic(uint32_t functionId, std::set<uint32_t>& visiting){
        if (!visiting.insert(functionId).second)
            return fail("a recursive call");
        for (uint32_t callee : functions[functionId].callees){
            if (functions.find(callee) == functions.end())
                return fail("a call to an unknown function");
            if (!checkAcyclic(callee, visiting))
                return false;
        }
        visiting.erase(functionId);
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
        std::map<uint32_t, uint32_t> defPosition;
        std::set<uint32_t> crossesBlock;
        std::map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> rootStores; // root -> (position, block)
        std::map<uint32_t, uint32_t> storeCount;
        std::set<uint32_t> elided;                  // instructions (by position) with nothing to emit
        std::map<uint32_t, const RawInstruction*> defInstruction;
    };

    static bool retargetable(spv::Op op){
        switch (op){
            case spv::OpFAdd: case spv::OpFSub: case spv::OpFMul: case spv::OpFDiv: case spv::OpFMod: case spv::OpFRem:
            case spv::OpFNegate: case spv::OpIAdd: case spv::OpISub: case spv::OpIMul: case spv::OpSDiv: case spv::OpUDiv:
            case spv::OpSMod: case spv::OpSRem: case spv::OpUMod: case spv::OpShiftLeftLogical: case spv::OpShiftRightLogical:
            case spv::OpShiftRightArithmetic: case spv::OpBitwiseAnd: case spv::OpBitwiseOr: case spv::OpBitwiseXor: case spv::OpNot:
            case spv::OpConvertFToS: case spv::OpConvertFToU: case spv::OpConvertSToF: case spv::OpConvertUToF: case spv::OpDot:
            case spv::OpVectorTimesScalar: case spv::OpSelect: case spv::OpExtInst: case spv::OpFOrdEqual: case spv::OpFOrdLessThan:
            case spv::OpFOrdGreaterThan: case spv::OpFOrdLessThanEqual: case spv::OpFOrdGreaterThanEqual: case spv::OpSLessThan:
            case spv::OpSGreaterThan: case spv::OpSLessThanEqual: case spv::OpSGreaterThanEqual: case spv::OpIEqual: case spv::OpINotEqual:
            case spv::OpLogicalAnd: case spv::OpLogicalOr: case spv::OpLogicalNot: case spv::OpSNegate: case spv::OpLoad:
                return true;
            default:
                return false;
        }
    }

    // The elementwise GLSL.std.450 functions; the rest build their value in several writes
    static bool retargetableExt(uint32_t e){
        switch (e){
            case GLSLstd450Determinant: case GLSLstd450MatrixInverse: case GLSLstd450Cross: case GLSLstd450Normalize:
            case GLSLstd450Reflect: case GLSLstd450Refract: case GLSLstd450FaceForward: case GLSLstd450Frexp: case GLSLstd450FrexpStruct:
                return false;
            default:
                return true;
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
        uint32_t depth = 0;
        for (uint32_t callee : functions[functionId].callees)
            depth = std::max(depth, 1 + callDepth(callee));
        return depth;
    }

    std::vector<std::pair<size_t, uint32_t>> callPatches;

    // Constant access chains have a register known now; a dynamic one gets a pointer register.
    bool resolveStaticPointers(FunctionInfo& function){
        for (const RawInstruction& raw : function.body){
            if (raw.op != spv::OpAccessChain && raw.op != spv::OpInBoundsAccessChain)
                continue;
            const uint32_t* w = raw.words;
            const IdInfo& base = ids[w[3]];
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

    uint32_t memberOffset(uint32_t type, uint32_t index) const {
        const IdType& t = types[type];
        if (t.kind == spv::OpTypeStruct){
            uint32_t offset = 0;
            for (uint32_t i = 0; i < index; i++)
                offset += types[t.members[i]].size;
            return offset;
        }
        return index * t.elementSize;
    }

    uint32_t memberType(uint32_t type, uint32_t index) const {
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
                analysis.defPosition[raw.words[2]] = raw.position;
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
    bool aliasOf(const RawInstruction& raw, uint32_t& source, uint32_t& offset) const {
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
            if (definition.op == spv::OpExtInst && !retargetableExt(definition.words[4]))
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
                    if (isGlobalRoot(root))
                        clear = false;
                }
            }
            if (!clear || definition.op == spv::OpPhi)
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
            if (constantVariables.count(variable))
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
            if (jump.tableIndex != UINT32_MAX)
                program.tables[jump.tableIndex] = label->second;
            else if (jump.operand == 0)
                program.code[jump.index].a = label->second;
            else
                program.code[jump.index].c = label->second;
        }
        return true;
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
        uint32_t tableIndex;
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
        bool conflict = false;
        for (size_t i = 0; i < copies.size(); i++)
            for (size_t j = 0; j < copies.size(); j++)
                if (i != j && copies[j].second == copies[i].first)
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
        pendingJumps.push_back({program.code.size() - 1, 0, label, UINT32_MAX});
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
        const uint32_t table = (uint32_t)program.tables.size();
        program.tables.push_back(type.elementSize);
        program.tables.push_back(type.kind == spv::OpTypeVector || type.kind == spv::OpTypeMatrix || type.kind == spv::OpTypeArray ? type.count : 1);
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
        case OpFRem: return binary(FRem1);
        case OpFNegate: return unary(FNeg1);
        case OpIAdd: return binary(IAdd1);
        case OpISub: return binary(ISub1);
        case OpIMul: return binary(IMul1);
        case OpUDiv: return binary(UDiv1);
        case OpSDiv: return binary(SDiv1);
        case OpUMod: return binary(UMod1);
        case OpSMod: return binary(SMod1);
        case OpSRem: return binary(SRem1);
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
        case OpFUnordEqual: return binary(FUEq1);
        case OpFUnordNotEqual: return binary(FUNe1);
        case OpFUnordLessThan: return binary(FULt1);
        case OpFUnordLessThanEqual: return binary(FULe1);
        case OpFUnordGreaterThan: return binary(FUGt1);
        case OpFUnordGreaterThanEqual: return binary(FUGe1);
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
        case OpIsInf: return unary(IsInf1);
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
        case OpOuterProduct: {
            const uint32_t rows = componentCount(ids[w[3]].type);
            const uint32_t cols = componentCount(ids[w[4]].type);
            emit(Outer, ids[w[2]].loc, ids[w[3]].loc, ids[w[4]].loc, (cols << 8) | rows);
            return true;
        }
        case OpTranspose: {
            const IdType& result = types[w[1]];
            emit(Transpose, ids[w[2]].loc, ids[w[3]].loc, (result.count << 8) | result.elementSize);
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
                const uint32_t source = currentBlock;
                program.code[index].a = (uint32_t)program.code.size();
                currentBlock = source;
                emitJumpTo(trueLabel);
                program.code[index].c = (uint32_t)program.code.size();
                currentBlock = source;
                emitJumpTo(falseLabel);
            } else {
                pendingJumps.push_back({index, 0, trueLabel, UINT32_MAX});
                pendingJumps.push_back({index, 2, falseLabel, UINT32_MAX});
            }
            return true;
        }
        case OpSwitch: {
            const uint32_t table = (uint32_t)program.tables.size();
            const uint32_t cases = (raw.count - 3) / 2;
            emit(Switch, ids[w[1]].loc, table, cases);
            const size_t index = program.code.size() - 1;
            program.tables.resize(program.tables.size() + 2 * cases + 1);
            const uint32_t source = currentBlock;
            const auto edge = [&](uint32_t label, uint32_t tableIndex){
                if (hasPhis(label)){
                    program.tables[tableIndex] = (uint32_t)program.code.size();
                    currentBlock = source;
                    emitJumpTo(label);
                } else {
                    pendingJumps.push_back({index, 0, label, tableIndex});
                }
            };
            for (uint32_t i = 0; i < cases; i++){
                program.tables[table + 2 * i] = w[3 + 2 * i];
                edge(w[4 + 2 * i], table + 2 * i + 1);
            }
            edge(w[2], table + 2 * cases);
            return true;
        }
        case OpReturn:
            emit(Ret);
            return true;
        case OpReturnValue:
            emitCopy(function.returnLoc, ids[w[1]].loc, sizeOf(function.returnType));
            emit(Ret);
            return true;
        case OpKill:
            emit(Kill);
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
    const auto family2 = [&](uint32_t base){ emit(base + n - 1, dst, a, b); return true; };
    const auto family1 = [&](uint32_t base){ emit(base + n - 1, dst, a); return true; };
    const auto family3 = [&](uint32_t base){ emit(base + n - 1, dst, a, b, c); return true; };
    const auto math1 = [&](uint32_t index){ emit(Math1, dst, a, index, n); return true; };
    const auto math2 = [&](uint32_t index){ emit(Math2, dst, a, b, (index << 8) | n); return true; };
    switch (w[4]){
        case GLSLstd450Round: case GLSLstd450RoundEven: return family1(Round1);
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
        case GLSLstd450Asinh: return math1(11);
        case GLSLstd450Acosh: return math1(12);
        case GLSLstd450Atanh: return math1(13);
        case GLSLstd450Exp: return math1(14);
        case GLSLstd450Log: return math1(15);
        case GLSLstd450Exp2: return math1(16);
        case GLSLstd450Log2: return math1(17);
        case GLSLstd450InverseSqrt: return math1(18);
        case GLSLstd450Atan2: return math2(0);
        case GLSLstd450Pow: return math2(1);
        case GLSLstd450FMin: case GLSLstd450NMin: return family2(FMin1);
        case GLSLstd450FMax: case GLSLstd450NMax: return family2(FMax1);
        case GLSLstd450UMin: return family2(UMin1);
        case GLSLstd450UMax: return family2(UMax1);
        case GLSLstd450SMin: return family2(SMin1);
        case GLSLstd450SMax: return family2(SMax1);
        case GLSLstd450FClamp: case GLSLstd450NClamp: return family3(Clamp1);
        case GLSLstd450UClamp: return family3(UClamp1);
        case GLSLstd450SClamp: return family3(SClamp1);
        case GLSLstd450FMix: return family3(Mix1);
        case GLSLstd450Step: return family2(Step1);
        case GLSLstd450SmoothStep: return family3(SmoothStep1);
        case GLSLstd450Fma: return family3(Fma1);
        case GLSLstd450Length:
            emit(Length1 + argumentN - 1, dst, a);
            return true;
        case GLSLstd450Distance:
            emit(Distance, dst, a, b, argumentN);
            return true;
        case GLSLstd450Normalize:
            emit(Normalize1 + n - 1, dst, a);
            return true;
        case GLSLstd450Cross:
            emit(Cross, dst, a, b);
            return true;
        case GLSLstd450Reflect:
            emit(Reflect, dst, a, b, n);
            return true;
        case GLSLstd450Refract:
            emit(Refract, dst, a, b, (c << 3) | n);
            return true;
        case GLSLstd450FaceForward:
            emit(FaceForward, dst, a, b, (c << 3) | n);
            return true;
        case GLSLstd450Determinant:
            emit(Determinant, dst, a, types[ids[w[5]].type].count);
            return true;
        case GLSLstd450MatrixInverse:
            emit(Inverse, dst, a, types[w[1]].count);
            return true;
        case GLSLstd450PackUnorm4x8: emit(PackUnorm4x8, dst, a); return true;
        case GLSLstd450UnpackUnorm4x8: emit(UnpackUnorm4x8, dst, a); return true;
        case GLSLstd450PackSnorm4x8: emit(PackSnorm4x8, dst, a); return true;
        case GLSLstd450UnpackSnorm4x8: emit(UnpackSnorm4x8, dst, a); return true;
        case GLSLstd450PackUnorm2x16: emit(PackUnorm2x16, dst, a); return true;
        case GLSLstd450UnpackUnorm2x16: emit(UnpackUnorm2x16, dst, a); return true;
        case GLSLstd450PackSnorm2x16: emit(PackSnorm2x16, dst, a); return true;
        case GLSLstd450UnpackSnorm2x16: emit(UnpackSnorm2x16, dst, a); return true;
        case GLSLstd450PackHalf2x16: emit(PackHalf2x16, dst, a); return true;
        case GLSLstd450UnpackHalf2x16: emit(UnpackHalf2x16, dst, a); return true;
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

#define CASE_F2(NAME, F) \
    case NAME##1: r[I.a].f = F(r[I.b].f, r[I.c].f); break; \
    case NAME##2: r[I.a].f = F(r[I.b].f, r[I.c].f); r[I.a + 1].f = F(r[I.b + 1].f, r[I.c + 1].f); break; \
    case NAME##3: r[I.a].f = F(r[I.b].f, r[I.c].f); r[I.a + 1].f = F(r[I.b + 1].f, r[I.c + 1].f); r[I.a + 2].f = F(r[I.b + 2].f, r[I.c + 2].f); break; \
    case NAME##4: r[I.a].f = F(r[I.b].f, r[I.c].f); r[I.a + 1].f = F(r[I.b + 1].f, r[I.c + 1].f); r[I.a + 2].f = F(r[I.b + 2].f, r[I.c + 2].f); r[I.a + 3].f = F(r[I.b + 3].f, r[I.c + 3].f); break;

#define CASE_F1(NAME, F) \
    case NAME##1: r[I.a].f = F(r[I.b].f); break; \
    case NAME##2: r[I.a].f = F(r[I.b].f); r[I.a + 1].f = F(r[I.b + 1].f); break; \
    case NAME##3: r[I.a].f = F(r[I.b].f); r[I.a + 1].f = F(r[I.b + 1].f); r[I.a + 2].f = F(r[I.b + 2].f); break; \
    case NAME##4: r[I.a].f = F(r[I.b].f); r[I.a + 1].f = F(r[I.b + 1].f); r[I.a + 2].f = F(r[I.b + 2].f); r[I.a + 3].f = F(r[I.b + 3].f); break;

#define CASE_F3(NAME, F) \
    case NAME##1: r[I.a].f = F(r[I.b].f, r[I.c].f, r[I.d].f); break; \
    case NAME##2: r[I.a].f = F(r[I.b].f, r[I.c].f, r[I.d].f); r[I.a + 1].f = F(r[I.b + 1].f, r[I.c + 1].f, r[I.d + 1].f); break; \
    case NAME##3: r[I.a].f = F(r[I.b].f, r[I.c].f, r[I.d].f); r[I.a + 1].f = F(r[I.b + 1].f, r[I.c + 1].f, r[I.d + 1].f); r[I.a + 2].f = F(r[I.b + 2].f, r[I.c + 2].f, r[I.d + 2].f); break; \
    case NAME##4: r[I.a].f = F(r[I.b].f, r[I.c].f, r[I.d].f); r[I.a + 1].f = F(r[I.b + 1].f, r[I.c + 1].f, r[I.d + 1].f); r[I.a + 2].f = F(r[I.b + 2].f, r[I.c + 2].f, r[I.d + 2].f); r[I.a + 3].f = F(r[I.b + 3].f, r[I.c + 3].f, r[I.d + 3].f); break;

// F(x, y) on (member-typed) integer operands; R is the result's union member
#define CASE_I2(NAME, M, F, R) \
    case NAME##1: r[I.a].R = F(r[I.b].M, r[I.c].M); break; \
    case NAME##2: r[I.a].R = F(r[I.b].M, r[I.c].M); r[I.a + 1].R = F(r[I.b + 1].M, r[I.c + 1].M); break; \
    case NAME##3: r[I.a].R = F(r[I.b].M, r[I.c].M); r[I.a + 1].R = F(r[I.b + 1].M, r[I.c + 1].M); r[I.a + 2].R = F(r[I.b + 2].M, r[I.c + 2].M); break; \
    case NAME##4: r[I.a].R = F(r[I.b].M, r[I.c].M); r[I.a + 1].R = F(r[I.b + 1].M, r[I.c + 1].M); r[I.a + 2].R = F(r[I.b + 2].M, r[I.c + 2].M); r[I.a + 3].R = F(r[I.b + 3].M, r[I.c + 3].M); break;

#define CASE_U1(NAME, M, F, R) \
    case NAME##1: r[I.a].R = F(r[I.b].M); break; \
    case NAME##2: r[I.a].R = F(r[I.b].M); r[I.a + 1].R = F(r[I.b + 1].M); break; \
    case NAME##3: r[I.a].R = F(r[I.b].M); r[I.a + 1].R = F(r[I.b + 1].M); r[I.a + 2].R = F(r[I.b + 2].M); break; \
    case NAME##4: r[I.a].R = F(r[I.b].M); r[I.a + 1].R = F(r[I.b + 1].M); r[I.a + 2].R = F(r[I.b + 2].M); r[I.a + 3].R = F(r[I.b + 3].M); break;

namespace {
    inline float opAdd(float x, float y){ return x + y; }
    inline float opSub(float x, float y){ return x - y; }
    inline float opMul(float x, float y){ return x * y; }
    inline float opDiv(float x, float y){ return x / y; }
    inline float opRem(float x, float y){ return std::fmod(x, y); }
    inline float opStep(float edge, float x){ return x < edge ? 0.0f : 1.0f; }
    inline float opNeg(float x){ return -x; }
    inline float opAbs(float x){ return std::fabs(x); }
    inline float opCeil(float x){ return std::ceil(x); }
    inline float opFract(float x){ return x - std::floor(x); }
    inline float opSqrt(float x){ return std::sqrt(x); }
    inline float opTrunc(float x){ return std::trunc(x); }
    // to nearest, ties to even: what llvmpipe, lavapipe and the common GPUs do for round()
    inline float opRound(float x){ return std::nearbyint(x); }
    inline float opMix(float x, float y, float a){ return x * (1.0f - a) + y * a; }
    inline float opFma(float a, float b, float c){ return a * b + c; }
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
    inline uint32_t cmpFUEq(float x, float y){ return !(x != y); }
    inline uint32_t cmpFUNe(float x, float y){ return x != y; }
    inline uint32_t cmpFULt(float x, float y){ return !(x >= y); }
    inline uint32_t cmpFULe(float x, float y){ return !(x > y); }
    inline uint32_t cmpFUGt(float x, float y){ return !(x <= y); }
    inline uint32_t cmpFUGe(float x, float y){ return !(x < y); }
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
    inline uint32_t testNan(float x){ return std::isnan(x); }
    inline uint32_t testInf(float x){ return std::isinf(x); }
    inline float fromInt(int32_t x){ return (float)x; }
    inline float fromUint(uint32_t x){ return (float)x; }

    // Half-float rounding for packHalf2x16: a Buffer's own precision follows R12's later pass
    uint32_t floatToHalf(float value){
        uint32_t bits;
        std::memcpy(&bits, &value, 4);
        const uint32_t sign = (bits >> 16) & 0x8000u;
        const int32_t exponent = (int32_t)((bits >> 23) & 0xffu) - 127 + 15;
        const uint32_t mantissa = bits & 0x7fffffu;
        if (exponent >= 31)
            return sign | 0x7c00u | (((bits >> 23) & 0xffu) == 0xffu && mantissa ? 0x200u : 0u);
        if (exponent <= 0){
            if (exponent < -10)
                return sign;
            return sign | ((mantissa | 0x800000u) >> (14 - exponent));
        }
        return sign | ((uint32_t)exponent << 10) | (mantissa >> 13);
    }

    float halfToFloat(uint32_t half){
        const uint32_t sign = (half & 0x8000u) << 16;
        const uint32_t exponent = (half >> 10) & 0x1fu;
        const uint32_t mantissa = half & 0x3ffu;
        uint32_t bits;
        if (exponent == 0){
            if (mantissa == 0){
                bits = sign;
            } else {
                int shift = 0;
                uint32_t m = mantissa;
                while (!(m & 0x400u)){
                    m <<= 1;
                    shift++;
                }
                bits = sign | ((uint32_t)(127 - 15 + 1 - shift) << 23) | ((m & 0x3ffu) << 13);
            }
        } else if (exponent == 31){
            bits = sign | 0x7f800000u | (mantissa << 13);
        } else {
            bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
        }
        float value;
        std::memcpy(&value, &bits, 4);
        return value;
    }

    float determinantOf(const SpirvRegister* m, uint32_t n){
        if (n == 2)
            return m[0].f * m[3].f - m[1].f * m[2].f;
        if (n == 3){
            return m[0].f * (m[4].f * m[8].f - m[5].f * m[7].f)
                 - m[3].f * (m[1].f * m[8].f - m[2].f * m[7].f)
                 + m[6].f * (m[1].f * m[5].f - m[2].f * m[4].f);
        }
        float result = 0.0f;
        for (uint32_t col = 0; col < 4; col++){
            SpirvRegister minor[9];
            uint32_t k = 0;
            for (uint32_t c = 0; c < 4; c++){
                if (c == col)
                    continue;
                for (uint32_t row = 1; row < 4; row++)
                    minor[k++] = m[c * 4 + row];
            }
            const float d = m[col * 4].f * determinantOf(minor, 3);
            result += (col & 1) ? -d : d;
        }
        return result;
    }

    // Gauss-Jordan: a shader's inverse() of a singular matrix is undefined, here it gives infinities
    void inverseOf(const SpirvRegister* m, uint32_t n, SpirvRegister* out){
        float a[4][8];
        for (uint32_t row = 0; row < n; row++){
            for (uint32_t col = 0; col < n; col++){
                a[row][col] = m[col * n + row].f;
                a[row][col + n] = row == col ? 1.0f : 0.0f;
            }
        }
        for (uint32_t col = 0; col < n; col++){
            uint32_t pivot = col;
            for (uint32_t row = col + 1; row < n; row++)
                if (std::fabs(a[row][col]) > std::fabs(a[pivot][col]))
                    pivot = row;
            if (pivot != col){
                for (uint32_t k = 0; k < 2 * n; k++)
                    std::swap(a[pivot][k], a[col][k]);
            }
            const float scale = 1.0f / a[col][col];
            for (uint32_t k = 0; k < 2 * n; k++)
                a[col][k] *= scale;
            for (uint32_t row = 0; row < n; row++){
                if (row == col)
                    continue;
                const float factor = a[row][col];
                for (uint32_t k = 0; k < 2 * n; k++)
                    a[row][k] -= factor * a[col][k];
            }
        }
        for (uint32_t row = 0; row < n; row++)
            for (uint32_t col = 0; col < n; col++)
                out[col * n + row].f = a[row][col + n];
    }
}

SpirvMachine::Result SpirvMachine::execute(){
    SpirvRegister* const r = registers.data();
    const SpirvInstruction* const code = program.code.data();
    const uint32_t* const tables = program.tables.data();
    uint32_t* const stack = callStack.data();
    const SpirvInstruction* pc = code + program.entry;
    uint32_t depth = 0;
    int64_t budget = backwardJumpLimit;
    for (;;){
        const SpirvInstruction& I = *pc++;
        switch (I.op){
            case Copy1: r[I.a] = r[I.b]; break;
            case Copy2: { const SpirvRegister x = r[I.b], y = r[I.b + 1]; r[I.a] = x; r[I.a + 1] = y; break; }
            case Copy3: { const SpirvRegister x = r[I.b], y = r[I.b + 1], z = r[I.b + 2]; r[I.a] = x; r[I.a + 1] = y; r[I.a + 2] = z; break; }
            case Copy4: { const SpirvRegister x = r[I.b], y = r[I.b + 1], z = r[I.b + 2], w = r[I.b + 3]; r[I.a] = x; r[I.a + 1] = y; r[I.a + 2] = z; r[I.a + 3] = w; break; }
            case CopyN: std::memmove(r + I.a, r + I.b, I.c * sizeof(SpirvRegister)); break;
            case ClearN: std::memset(static_cast<void*>(r + I.a), 0, I.b * sizeof(SpirvRegister)); break;

            CASE_F2(FAdd, opAdd)
            CASE_F2(FSub, opSub)
            CASE_F2(FMul, opMul)
            CASE_F2(FDiv, opDiv)
            CASE_F2(FMod, glslMod)
            CASE_F2(FRem, opRem)
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
            CASE_F3(Fma, opFma)

            CASE_I2(IAdd, i, opIAdd, i)
            CASE_I2(ISub, i, opISub, i)
            CASE_I2(IMul, i, opIMul, i)
            CASE_I2(UDiv, u, safeUDiv, u)
            CASE_I2(SDiv, i, safeSDiv, i)
            CASE_I2(UMod, u, safeUMod, u)
            CASE_I2(SMod, i, safeSMod, i)
            CASE_I2(SRem, i, safeSRem, i)
            CASE_I2(And, u, opAnd, u)
            CASE_I2(Or, u, opOr, u)
            CASE_I2(Xor, u, opXor, u)
            CASE_I2(Shl, u, opShl, u)
            CASE_I2(ShrL, u, opShrL, u)
            CASE_I2(UMin, u, opUMin, u)
            CASE_I2(UMax, u, opUMax, u)
            CASE_I2(SMin, i, opSMin, i)
            CASE_I2(SMax, i, opSMax, i)
            case ShrA1: r[I.a].i = opShrA(r[I.b].i, r[I.c].i); break;
            case ShrA2: case ShrA3: case ShrA4: {
                const uint32_t n = I.op - ShrA1 + 1;
                for (uint32_t k = 0; k < n; k++)
                    r[I.a + k].i = opShrA(r[I.b + k].i, r[I.c + k].i);
                break;
            }
            case UClamp1: case UClamp2: case UClamp3: case UClamp4: {
                const uint32_t n = I.op - UClamp1 + 1;
                for (uint32_t k = 0; k < n; k++)
                    r[I.a + k].u = opUMin(opUMax(r[I.b + k].u, r[I.c + k].u), r[I.d + k].u);
                break;
            }
            case SClamp1: case SClamp2: case SClamp3: case SClamp4: {
                const uint32_t n = I.op - SClamp1 + 1;
                for (uint32_t k = 0; k < n; k++)
                    r[I.a + k].i = opSMin(opSMax(r[I.b + k].i, r[I.c + k].i), r[I.d + k].i);
                break;
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
            CASE_I2(FUEq, f, cmpFUEq, u)
            CASE_I2(FUNe, f, cmpFUNe, u)
            CASE_I2(FULt, f, cmpFULt, u)
            CASE_I2(FULe, f, cmpFULe, u)
            CASE_I2(FUGt, f, cmpFUGt, u)
            CASE_I2(FUGe, f, cmpFUGe, u)
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
            CASE_U1(IsInf, f, testInf, u)

            CASE_U1(F2S, f, floatToInt, i)
            CASE_U1(F2U, f, floatToUint, u)
            CASE_U1(S2F, i, fromInt, f)
            CASE_U1(U2F, u, fromUint, f)

            case SelectS1: r[I.a] = r[I.b].u ? r[I.c] : r[I.d]; break;
            case SelectS2: case SelectS3: case SelectS4: {
                const uint32_t n = I.op - SelectS1 + 1;
                const uint32_t from = r[I.b].u ? I.c : I.d;
                for (uint32_t k = 0; k < n; k++)
                    r[I.a + k] = r[from + k];
                break;
            }
            case SelectV1: case SelectV2: case SelectV3: case SelectV4: {
                const uint32_t n = I.op - SelectV1 + 1;
                for (uint32_t k = 0; k < n; k++)
                    r[I.a + k] = r[I.b + k].u ? r[I.c + k] : r[I.d + k];
                break;
            }
            case SelectSN: {
                const uint32_t from = r[I.b].u ? I.c : tables[I.d];
                std::memmove(r + I.a, r + from, tables[I.d + 1] * sizeof(SpirvRegister));
                break;
            }
            case Any1: r[I.a].u = r[I.b].u; break;
            case Any2: r[I.a].u = r[I.b].u | r[I.b + 1].u; break;
            case Any3: r[I.a].u = r[I.b].u | r[I.b + 1].u | r[I.b + 2].u; break;
            case Any4: r[I.a].u = r[I.b].u | r[I.b + 1].u | r[I.b + 2].u | r[I.b + 3].u; break;
            case All1: r[I.a].u = r[I.b].u; break;
            case All2: r[I.a].u = r[I.b].u & r[I.b + 1].u; break;
            case All3: r[I.a].u = r[I.b].u & r[I.b + 1].u & r[I.b + 2].u; break;
            case All4: r[I.a].u = r[I.b].u & r[I.b + 1].u & r[I.b + 2].u & r[I.b + 3].u; break;

            case Dot1: r[I.a].f = r[I.b].f * r[I.c].f; break;
            case Dot2: r[I.a].f = r[I.b].f * r[I.c].f + r[I.b + 1].f * r[I.c + 1].f; break;
            case Dot3: r[I.a].f = r[I.b].f * r[I.c].f + r[I.b + 1].f * r[I.c + 1].f + r[I.b + 2].f * r[I.c + 2].f; break;
            case Dot4: r[I.a].f = r[I.b].f * r[I.c].f + r[I.b + 1].f * r[I.c + 1].f + r[I.b + 2].f * r[I.c + 2].f + r[I.b + 3].f * r[I.c + 3].f; break;
            case Length1: r[I.a].f = std::fabs(r[I.b].f); break;
            case Length2: r[I.a].f = std::sqrt(r[I.b].f * r[I.b].f + r[I.b + 1].f * r[I.b + 1].f); break;
            case Length3: r[I.a].f = std::sqrt(r[I.b].f * r[I.b].f + r[I.b + 1].f * r[I.b + 1].f + r[I.b + 2].f * r[I.b + 2].f); break;
            case Length4: r[I.a].f = std::sqrt(r[I.b].f * r[I.b].f + r[I.b + 1].f * r[I.b + 1].f + r[I.b + 2].f * r[I.b + 2].f + r[I.b + 3].f * r[I.b + 3].f); break;
            case Normalize1: case Normalize2: case Normalize3: case Normalize4: {
                const uint32_t n = I.op - Normalize1 + 1;
                float sum = 0.0f;
                for (uint32_t k = 0; k < n; k++)
                    sum += r[I.b + k].f * r[I.b + k].f;
                const float scale = 1.0f / std::sqrt(sum);
                for (uint32_t k = 0; k < n; k++)
                    r[I.a + k].f = r[I.b + k].f * scale;
                break;
            }
            case Distance: {
                float sum = 0.0f;
                for (uint32_t k = 0; k < I.d; k++){
                    const float x = r[I.b + k].f - r[I.c + k].f;
                    sum += x * x;
                }
                r[I.a].f = std::sqrt(sum);
                break;
            }
            case Cross: {
                const float x = r[I.b + 1].f * r[I.c + 2].f - r[I.b + 2].f * r[I.c + 1].f;
                const float y = r[I.b + 2].f * r[I.c].f - r[I.b].f * r[I.c + 2].f;
                const float z = r[I.b].f * r[I.c + 1].f - r[I.b + 1].f * r[I.c].f;
                r[I.a].f = x;
                r[I.a + 1].f = y;
                r[I.a + 2].f = z;
                break;
            }
            case Vts1: r[I.a].f = r[I.b].f * r[I.c].f; break;
            case Vts2: { const float s = r[I.c].f; r[I.a].f = r[I.b].f * s; r[I.a + 1].f = r[I.b + 1].f * s; break; }
            case Vts3: { const float s = r[I.c].f; r[I.a].f = r[I.b].f * s; r[I.a + 1].f = r[I.b + 1].f * s; r[I.a + 2].f = r[I.b + 2].f * s; break; }
            case Vts4: { const float s = r[I.c].f; r[I.a].f = r[I.b].f * s; r[I.a + 1].f = r[I.b + 1].f * s; r[I.a + 2].f = r[I.b + 2].f * s; r[I.a + 3].f = r[I.b + 3].f * s; break; }
            case VtsN: {
                const float s = r[I.c].f;
                for (uint32_t k = 0; k < I.d; k++)
                    r[I.a + k].f = r[I.b + k].f * s;
                break;
            }
            case MatVec: {
                const uint32_t cols = I.d >> 8, rows = I.d & 0xff;
                float out[4] = {};
                for (uint32_t col = 0; col < cols; col++){
                    const float x = r[I.c + col].f;
                    for (uint32_t row = 0; row < rows; row++)
                        out[row] += r[I.b + col * rows + row].f * x;
                }
                for (uint32_t row = 0; row < rows; row++)
                    r[I.a + row].f = out[row];
                break;
            }
            case VecMat: {
                const uint32_t cols = I.d >> 8, rows = I.d & 0xff;
                float out[4] = {};
                for (uint32_t col = 0; col < cols; col++){
                    for (uint32_t row = 0; row < rows; row++)
                        out[col] += r[I.b + row].f * r[I.c + col * rows + row].f;
                }
                for (uint32_t col = 0; col < cols; col++)
                    r[I.a + col].f = out[col];
                break;
            }
            case MatMat: {
                const uint32_t rightCols = I.d >> 16, leftCols = (I.d >> 8) & 0xff, rows = I.d & 0xff;
                float out[16] = {};
                for (uint32_t col = 0; col < rightCols; col++){
                    for (uint32_t k = 0; k < leftCols; k++){
                        const float x = r[I.c + col * leftCols + k].f;
                        for (uint32_t row = 0; row < rows; row++)
                            out[col * rows + row] += r[I.b + k * rows + row].f * x;
                    }
                }
                for (uint32_t k = 0; k < rightCols * rows; k++)
                    r[I.a + k].f = out[k];
                break;
            }
            case Outer: {
                const uint32_t cols = I.d >> 8, rows = I.d & 0xff;
                float out[16];
                for (uint32_t col = 0; col < cols; col++)
                    for (uint32_t row = 0; row < rows; row++)
                        out[col * rows + row] = r[I.b + row].f * r[I.c + col].f;
                for (uint32_t k = 0; k < cols * rows; k++)
                    r[I.a + k].f = out[k];
                break;
            }
            case Transpose: {
                const uint32_t cols = I.c >> 8, rows = I.c & 0xff;
                float out[16];
                for (uint32_t col = 0; col < cols; col++)
                    for (uint32_t row = 0; row < rows; row++)
                        out[col * rows + row] = r[I.b + row * cols + col].f;
                for (uint32_t k = 0; k < cols * rows; k++)
                    r[I.a + k].f = out[k];
                break;
            }
            case Determinant:
                r[I.a].f = determinantOf(r + I.b, I.c);
                break;
            case Inverse: {
                SpirvRegister out[16];
                inverseOf(r + I.b, I.c, out);
                for (uint32_t k = 0; k < I.c * I.c; k++)
                    r[I.a + k] = out[k];
                break;
            }
            case Reflect: {
                float dot = 0.0f;
                for (uint32_t k = 0; k < I.d; k++)
                    dot += r[I.b + k].f * r[I.c + k].f;
                for (uint32_t k = 0; k < I.d; k++)
                    r[I.a + k].f = r[I.b + k].f - 2.0f * dot * r[I.c + k].f;
                break;
            }
            case Refract: {
                const uint32_t n = I.d & 7u;
                const float eta = r[I.d >> 3].f;
                float dot = 0.0f;
                for (uint32_t k = 0; k < n; k++)
                    dot += r[I.b + k].f * r[I.c + k].f;
                const float root = 1.0f - eta * eta * (1.0f - dot * dot);
                for (uint32_t k = 0; k < n; k++)
                    r[I.a + k].f = root < 0.0f ? 0.0f : eta * r[I.b + k].f - (eta * dot + std::sqrt(root)) * r[I.c + k].f;
                break;
            }
            case FaceForward: {
                const uint32_t n = I.d & 7u;
                const uint32_t reference = I.d >> 3;
                float dot = 0.0f;
                for (uint32_t k = 0; k < n; k++)
                    dot += r[reference + k].f * r[I.c + k].f;
                for (uint32_t k = 0; k < n; k++)
                    r[I.a + k].f = dot < 0.0f ? r[I.b + k].f : -r[I.b + k].f;
                break;
            }
            case Shuffle: {
                SpirvRegister out[4];
                for (uint32_t k = 0; k < I.c; k++)
                    out[k] = r[tables[I.b + k]];
                for (uint32_t k = 0; k < I.c; k++)
                    r[I.a + k] = out[k];
                break;
            }
            case ExtractDyn: {
                const uint32_t k = (uint32_t)std::clamp<int64_t>(r[I.c].i, 0, (int64_t)I.d - 1);
                r[I.a] = r[I.b + k];
                break;
            }
            case InsertDyn: {
                const uint32_t k = (uint32_t)std::clamp<int64_t>(r[I.c].i, 0, (int64_t)I.d - 1);
                r[I.a + k] = r[I.b];
                break;
            }
            case PtrIndex: {
                const uint32_t stride = tables[I.d], count = tables[I.d + 1];
                const uint32_t k = (uint32_t)std::clamp<int64_t>(r[I.c].i, 0, (int64_t)count - 1);
                r[I.a].u = I.b + k * stride;
                break;
            }
            case PtrIndexAdd: {
                const uint32_t stride = tables[I.d], count = tables[I.d + 1];
                const uint32_t k = (uint32_t)std::clamp<int64_t>(r[I.c].i, 0, (int64_t)count - 1);
                r[I.a].u = r[I.b].u + k * stride;
                break;
            }
            case LoadInd:
                std::memmove(r + I.a, r + r[I.b].u, I.c * sizeof(SpirvRegister));
                break;
            case StoreInd:
                std::memmove(r + r[I.a].u, r + I.b, I.c * sizeof(SpirvRegister));
                break;
            case Math1: {
                const Math1Function function = math1Functions[I.c];
                for (uint32_t k = 0; k < I.d; k++)
                    r[I.a + k].f = function(r[I.b + k].f);
                break;
            }
            case Math2: {
                const Math2Function function = math2Functions[I.d >> 8];
                const uint32_t n = I.d & 0xff;
                for (uint32_t k = 0; k < n; k++)
                    r[I.a + k].f = function(r[I.b + k].f, r[I.c + k].f);
                break;
            }
            case PackUnorm4x8: {
                uint32_t result = 0;
                for (uint32_t k = 0; k < 4; k++)
                    result |= (uint32_t)std::lround(glslClamp(r[I.b + k].f, 0.0f, 1.0f) * 255.0f) << (8 * k);
                r[I.a].u = result;
                break;
            }
            case UnpackUnorm4x8: {
                const uint32_t value = r[I.b].u;
                for (uint32_t k = 0; k < 4; k++)
                    r[I.a + k].f = (float)((value >> (8 * k)) & 0xffu) / 255.0f;
                break;
            }
            case PackSnorm4x8: {
                uint32_t result = 0;
                for (uint32_t k = 0; k < 4; k++)
                    result |= ((uint32_t)std::lround(glslClamp(r[I.b + k].f, -1.0f, 1.0f) * 127.0f) & 0xffu) << (8 * k);
                r[I.a].u = result;
                break;
            }
            case UnpackSnorm4x8: {
                const uint32_t value = r[I.b].u;
                for (uint32_t k = 0; k < 4; k++)
                    r[I.a + k].f = glslClamp((float)(int8_t)((value >> (8 * k)) & 0xffu) / 127.0f, -1.0f, 1.0f);
                break;
            }
            case PackUnorm2x16: {
                uint32_t result = 0;
                for (uint32_t k = 0; k < 2; k++)
                    result |= (uint32_t)std::lround(glslClamp(r[I.b + k].f, 0.0f, 1.0f) * 65535.0f) << (16 * k);
                r[I.a].u = result;
                break;
            }
            case UnpackUnorm2x16: {
                const uint32_t value = r[I.b].u;
                for (uint32_t k = 0; k < 2; k++)
                    r[I.a + k].f = (float)((value >> (16 * k)) & 0xffffu) / 65535.0f;
                break;
            }
            case PackSnorm2x16: {
                uint32_t result = 0;
                for (uint32_t k = 0; k < 2; k++)
                    result |= ((uint32_t)std::lround(glslClamp(r[I.b + k].f, -1.0f, 1.0f) * 32767.0f) & 0xffffu) << (16 * k);
                r[I.a].u = result;
                break;
            }
            case UnpackSnorm2x16: {
                const uint32_t value = r[I.b].u;
                for (uint32_t k = 0; k < 2; k++)
                    r[I.a + k].f = glslClamp((float)(int16_t)((value >> (16 * k)) & 0xffffu) / 32767.0f, -1.0f, 1.0f);
                break;
            }
            case PackHalf2x16:
                r[I.a].u = floatToHalf(r[I.b].f) | (floatToHalf(r[I.b + 1].f) << 16);
                break;
            case UnpackHalf2x16: {
                const uint32_t value = r[I.b].u;
                r[I.a].f = halfToFloat(value & 0xffffu);
                r[I.a + 1].f = halfToFloat(value >> 16);
                break;
            }
            case Jump: {
                const SpirvInstruction* target = code + I.a;
                if (target <= pc && --budget < 0)
                    return Result::TooLong;
                pc = target;
                break;
            }
            case JumpIf: {
                const SpirvInstruction* target = code + (r[I.b].u ? I.a : I.c);
                if (target <= pc && --budget < 0)
                    return Result::TooLong;
                pc = target;
                break;
            }
            case Switch: {
                const int32_t value = r[I.a].i;
                uint32_t target = tables[I.b + 2 * I.c];
                for (uint32_t k = 0; k < I.c; k++){
                    if ((int32_t)tables[I.b + 2 * k] == value){
                        target = tables[I.b + 2 * k + 1];
                        break;
                    }
                }
                pc = code + target;
                break;
            }
            case Call:
                stack[depth++] = (uint32_t)(pc - code);
                pc = code + I.a;
                break;
            case Ret:
                if (depth == 0)
                    return Result::Ok;
                pc = code + stack[--depth];
                break;
            case Kill:
                return Result::Discarded;
            case Sample: {
                float out[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                const uint32_t slot = r[I.b].u;
                const SpirvImage* image = slot < textures.size() ? textures[slot] : nullptr;
                if (image != nullptr){
                    const float lod = r[I.d].f;
                    const float u = r[I.c].f, v = r[I.c + 1].f;
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
                    r[I.a + k].f = out[k];
                break;
            }
            default:
                return Result::TooLong;
        }
    }
}

SpirvImage::SpirvImage(int width, int height, std::vector<float> rgba){
    mips.push_back({width, height, std::move(rgba)});
    while (mips.back().width > 1 || mips.back().height > 1){
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
                    out[k] = sum[k] * 0.25f;
            }
        }
        mips.push_back(std::move(next));
    }
}

void SpirvImage::sampleBilinear(float u, float v, int mip, float out[4]) const {
    const Mip& level = mips[std::clamp(mip, 0, (int)mips.size() - 1)];
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
