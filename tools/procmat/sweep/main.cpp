// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The measurements of S90 M0, over the corpus tools/procmat/expand_nodes.py writes:
//
//   procmat-sweep compile --corpus DIR --out report.json [--jobs N] [--filter TEXT]
//       compiles every shader to SPIR-V, GLSL 410, GLSL 300 es, MSL (macOS, iOS) and HLSL through
//       engine/libs/shadercompiler; counts the SPIR-V opcodes and GLSL.std.450 instructions;
//       with --jobs 1, the timings are the per-node cold compile times of the spec.
//   procmat-sweep interpret --corpus DIR --out report.json [--size N] [--threads N] [--filter TEXT]
//       runs each compiled shader through SpirvInterpreter; a shader outside the covered
//       instruction set is reported, not run.
//   procmat-sweep crosstier --corpus DIR --out report.json [--size N] [--threads N] [--filter TEXT]
//       the same shaders on GL (llvmpipe here) against the interpreter: per-channel 8-bit difference
//       and the timings of both.
//   procmat-sweep bench --corpus DIR --shader NAME [--size N] [--threads N] [--repeat N]
//       one shader at one size: interpreter cost per Mpixel; on GL, cold compile, first and later draw.

#include "GlPass.h"
#include "SpirvInterpreter.h"
#include "shadercompiler.h"

#include "SPIRV/GLSL.std.450.h"
#include "SPIRV/spirv.hpp"
#include "json.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;
using namespace jasperengine::editor::procmat;

namespace {

const char* vertexSource = R"(#version 450
out vec2 v_uv;
void main() {
    vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_uv = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    #ifndef IS_GLSL
        v_uv.y = 1.0 - v_uv.y;
    #endif
    #ifdef IS_VULKAN
        gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
    #endif
}
)";

struct Target {
    const char* name;
    shadercompiler::lang_type_t lang;
    int version;
    bool es;
    shadercompiler::platform_t platform;
};

const Target targets[] = {
    {"glsl410", shadercompiler::LANG_GLSL, 410, false, shadercompiler::SHADER_DEFAULT},
    {"spirv", shadercompiler::LANG_SPIRV, 10, false, shadercompiler::SHADER_DEFAULT},
    {"msl_macos", shadercompiler::LANG_MSL, 20100, false, shadercompiler::SHADER_MACOS},
    {"msl_ios", shadercompiler::LANG_MSL, 20100, false, shadercompiler::SHADER_IOS},
    {"hlsl", shadercompiler::LANG_HLSL, 50, false, shadercompiler::SHADER_DEFAULT},
    {"glsl300es", shadercompiler::LANG_GLSL, 300, true, shadercompiler::SHADER_DEFAULT},
};

double millisecondsSince(std::chrono::steady_clock::time_point start){
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

std::string readText(const fs::path& path){
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

struct CompileResult {
    bool ok = false;
    double spirvMs = 0.0;
    double crossMs = 0.0;
    std::string log;
    std::string output;
    std::string vertexOutput;
    std::vector<uint32_t> spirv;
};

CompileResult compileShader(const std::string& name, const std::string& fragment, const Target& target){
    CompileResult result;
    shadercompiler::args_t args = shadercompiler::initialize_args();
    args.useBuffers = true;
    args.fileBuffers["procmat.vert"] = vertexSource;
    args.fileBuffers[name + ".frag"] = fragment;
    args.vert_file = "procmat.vert";
    args.frag_file = name + ".frag";
    args.lang = target.lang;
    args.version = target.version;
    args.es = target.es;
    args.platform = target.platform;
    args.error_output = &result.log;

    std::vector<shadercompiler::input_t> inputs;
    if (!shadercompiler::load_input(inputs, args)){
        result.log += "cannot load the input\n";
        return result;
    }
    std::vector<shadercompiler::spirv_t> spirv(inputs.size());
    auto start = std::chrono::steady_clock::now();
    if (!shadercompiler::compile_to_spirv(spirv, inputs, args)){
        result.spirvMs = millisecondsSince(start);
        return result;
    }
    result.spirvMs = millisecondsSince(start);
    start = std::chrono::steady_clock::now();
    std::vector<shadercompiler::spirvcross_t> cross(inputs.size());
    try {
        result.ok = shadercompiler::compile_to_lang(cross, spirv, inputs, args);
    } catch (const std::exception& e) {
        result.log += std::string("SPIRV-Cross: ") + e.what() + "\n";
    }
    result.crossMs = millisecondsSince(start);
    for (size_t i = 0; i < inputs.size(); i++){
        if (inputs[i].stage_type == shadercompiler::STAGE_FRAGMENT){
            result.spirv = spirv[i].bytecode;
            result.output = cross[i].source;
        } else if (i < cross.size()) {
            result.vertexOutput = cross[i].source;
        }
    }
    return result;
}

// The instruction set a module uses: opcodes, and GLSL.std.450 numbers in extended instructions
void countInstructions(const std::vector<uint32_t>& words, std::map<int, int>& ops, std::map<int, int>& ext){
    std::set<int> seenOps, seenExt;
    for (size_t i = 5; i < words.size();){
        const uint32_t count = words[i] >> 16;
        const int op = (int)(words[i] & 0xffff);
        if (count == 0)
            break;
        seenOps.insert(op);
        if (op == spv::OpExtInst && count > 4)
            seenExt.insert((int)words[i + 4]);
        i += count;
    }
    for (int op : seenOps)
        ops[op]++;
    for (int e : seenExt)
        ext[e]++;
}

std::string option(int argc, char** argv, const std::string& name, const std::string& fallback = ""){
    for (int i = 0; i + 1 < argc; i++)
        if (name == argv[i])
            return argv[i + 1];
    return fallback;
}

int runCompile(int argc, char** argv){
    const fs::path corpus = option(argc, argv, "--corpus");
    const std::string outPath = option(argc, argv, "--out");
    const std::string filter = option(argc, argv, "--filter");
    const int jobs = std::stoi(option(argc, argv, "--jobs", "4"));
    const bool writeOutputs = option(argc, argv, "--keep-output", "0") == "1";
    if (corpus.empty() || outPath.empty()){
        fprintf(stderr, "usage: procmat-sweep compile --corpus DIR --out FILE [--jobs N] [--filter TEXT]\n");
        return 2;
    }
    const json manifest = json::parse(readText(corpus / "manifest.json"));
    std::vector<json> shaders;
    for (const json& shader : manifest["shaders"])
        if (filter.empty() || shader["shader"].get<std::string>().find(filter) != std::string::npos)
            shaders.push_back(shader);

    fs::create_directories(corpus / "spv");
    shadercompiler::initialize_process();

    std::vector<json> results(shaders.size());
    std::map<int, int> ops, ext;
    std::mutex mutex;
    std::atomic<size_t> next{0};
    auto worker = [&](){
        for (;;){
            const size_t index = next.fetch_add(1);
            if (index >= shaders.size())
                return;
            const std::string name = shaders[index]["shader"];
            const std::string fragment = readText(corpus / (name + ".frag"));
            json row = {{"shader", name}, {"node", shaders[index]["node"]}, {"variant", shaders[index]["variant"]},
                        {"bytes", shaders[index]["bytes"]}, {"kind", shaders[index]["kind"]}};
            std::map<int, int> shaderOps, shaderExt;
            bool all = true;
            for (const Target& target : targets){
                const CompileResult compiled = compileShader(name, fragment, target);
                row["targets"][target.name] = {{"ok", compiled.ok}, {"spirv_ms", compiled.spirvMs}, {"cross_ms", compiled.crossMs},
                                               {"output_bytes", compiled.output.size()}};
                if (!compiled.ok){
                    all = false;
                    row["targets"][target.name]["log"] = compiled.log.substr(0, 2000);
                }
                if (target.lang == shadercompiler::LANG_SPIRV && !compiled.spirv.empty()){
                    std::ofstream file(corpus / "spv" / (name + ".spv"), std::ios::binary);
                    file.write(reinterpret_cast<const char*>(compiled.spirv.data()), (std::streamsize)(compiled.spirv.size() * 4));
                    countInstructions(compiled.spirv, shaderOps, shaderExt);
                    row["spirv_words"] = compiled.spirv.size();
                }
                if (writeOutputs && !compiled.output.empty()){
                    std::ofstream file(corpus / (name + "." + target.name + ".txt"), std::ios::binary);
                    file << compiled.output;
                }
            }
            row["ok"] = all;
            std::lock_guard<std::mutex> lock(mutex);
            results[index] = std::move(row);
            for (const auto& [op, n] : shaderOps)
                ops[op] += n;
            for (const auto& [e, n] : shaderExt)
                ext[e] += n;
        }
    };
    std::vector<std::thread> threads;
    for (int i = 0; i < jobs; i++)
        threads.emplace_back(worker);
    for (std::thread& thread : threads)
        thread.join();
    shadercompiler::finalize_process();

    json report = {{"shaders", results}, {"expansion_failures", manifest["expansion_failures"]}};
    for (const auto& [op, n] : ops)
        report["opcodes"][std::to_string(op)] = n;
    for (const auto& [e, n] : ext)
        report["glsl_std_450"][std::to_string(e)] = n;
    std::ofstream(outPath) << report.dump(1);
    int failed = 0;
    for (const json& row : results)
        failed += row["ok"].get<bool>() ? 0 : 1;
    printf("procmat-sweep: %zu shaders, %d failed on at least one target\n", results.size(), failed);
    return failed > 0 || !manifest["expansion_failures"].empty() ? 1 : 0;
}

// A smooth, periodic image every sampled input gets, the same on every tier: multiples of 1/256,
// which RGBA16F holds exactly.
std::vector<float> probeTexture(int size){
    std::vector<float> rgba((size_t)size * size * 4);
    const auto quantize = [](float x){ return std::floor(x * 256.0f) / 256.0f; };
    for (int y = 0; y < size; y++){
        for (int x = 0; x < size; x++){
            const float u = ((float)x + 0.5f) / (float)size, v = ((float)y + 0.5f) / (float)size;
            float* p = &rgba[((size_t)y * size + x) * 4];
            p[0] = quantize(0.5f + 0.5f * (float)std::sin(6.283185307 * u * 2.0));
            p[1] = quantize(0.5f + 0.5f * (float)std::cos(6.283185307 * v * 3.0));
            p[2] = quantize(u * (1.0f - u) * 4.0f);
            p[3] = 1.0f;
        }
    }
    return rgba;
}

std::vector<float> floatsOf(const json& array){
    std::vector<float> values;
    for (const json& value : array)
        values.push_back(value.get<float>());
    return values;
}

// The quantize pass of R12: both tiers end in this, so an out-of-range value has one meaning
uint8_t quantize(float value){
    const float clamped = value != value ? 0.0f : std::min(std::max(value, 0.0f), 1.0f);
    return (uint8_t)std::floor(clamped * 255.0f + 0.5f);
}

struct InterpretResult {
    int tooLong = 0;
    double seconds = 0.0;
};

// Runs a whole image on `threads` workers, row by row off a shared counter. The S06 job system
// takes this place in the editor; the bench only needs the same shape of work.
InterpretResult interpret(const SpirvProgram& program, const std::vector<float>& uniforms, const SpirvImage* image,
                          int size, int threads, std::vector<uint8_t>* out){
    InterpretResult result;
    std::atomic<int> nextRow{0}, tooLong{0};
    if (out)
        out->resize((size_t)size * size * 4);
    const auto worker = [&](){
        SpirvMachine machine(program);
        machine.setUniforms(uniforms.data(), uniforms.size());
        for (int slot = 0; slot < program.textureCount(); slot++)
            machine.setTexture(slot, image);
        for (;;){
            const int y = nextRow.fetch_add(1);
            if (y >= size)
                return;
            const float v = ((float)y + 0.5f) / (float)size;
            for (int x = 0; x < size; x++){
                float rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                if (machine.shade(((float)x + 0.5f) / (float)size, v, rgba) == SpirvMachine::Result::TooLong)
                    tooLong++;
                if (out){
                    uint8_t* pixel = out->data() + ((size_t)y * size + x) * 4;
                    for (int k = 0; k < 4; k++)
                        pixel[k] = quantize(rgba[k]);
                }
            }
        }
    };
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> pool;
    for (int i = 0; i < threads; i++)
        pool.emplace_back(worker);
    for (std::thread& thread : pool)
        thread.join();
    result.seconds = millisecondsSince(start) / 1000.0;
    result.tooLong = tooLong;
    return result;
}

std::vector<uint32_t> readSpirv(const fs::path& path){
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        return {};
    const std::streamsize bytes = file.tellg();
    std::vector<uint32_t> words((size_t)bytes / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(words.data()), bytes);
    return words;
}

struct Selection {
    std::vector<json> shaders;
    fs::path corpus;
    int size;
    int threads;
};

Selection selectShaders(int argc, char** argv, const char* defaultSize){
    Selection selection;
    selection.corpus = option(argc, argv, "--corpus");
    selection.size = std::stoi(option(argc, argv, "--size", defaultSize));
    selection.threads = std::stoi(option(argc, argv, "--threads", "4"));
    const std::string filter = option(argc, argv, "--filter");
    const json manifest = json::parse(readText(selection.corpus / "manifest.json"));
    for (const json& shader : manifest["shaders"]){
        const std::string name = shader["shader"];
        if (!filter.empty() && name.find(filter) == std::string::npos)
            continue;
        selection.shaders.push_back(shader);
    }
    return selection;
}

int runInterpret(int argc, char** argv){
    const Selection selection = selectShaders(argc, argv, "256");
    const std::string outPath = option(argc, argv, "--out");
    const int textureSize = 256;
    const SpirvImage image(textureSize, textureSize, probeTexture(textureSize));
    json rows = json::array();
    int ran = 0, refused = 0;
    std::map<std::string, int> reasons;
    for (const json& shader : selection.shaders){
        const std::string name = shader["shader"];
        const std::vector<uint32_t> words = readSpirv(selection.corpus / "spv" / (name + ".spv"));
        json row = {{"shader", name}, {"node", shader["node"]}, {"variant", shader["variant"]}};
        if (words.empty()){
            row["error"] = "no SPIR-V";
            rows.push_back(row);
            continue;
        }
        std::string error;
        const auto start = std::chrono::steady_clock::now();
        const std::unique_ptr<SpirvProgram> program = SpirvProgram::load(words.data(), words.size(), error);
        row["load_ms"] = millisecondsSince(start);
        if (!program){
            row["error"] = error;
            reasons[error]++;
            refused++;
            rows.push_back(row);
            continue;
        }
        const InterpretResult result = interpret(*program, floatsOf(shader["uniforms"]), &image, selection.size, selection.threads, nullptr);
        const double mpixels = (double)selection.size * selection.size / 1e6;
        row["instructions"] = program->instructionCount();
        row["registers"] = program->registerCount();
        row["seconds_per_mpixel"] = result.seconds / mpixels;
        row["too_long"] = result.tooLong;
        rows.push_back(row);
        ran++;
    }
    std::ofstream(outPath) << json({{"shaders", rows}, {"size", selection.size}, {"threads", selection.threads}}).dump(1);
    printf("procmat-sweep interpret: %d ran, %d refused\n", ran, refused);
    for (const auto& [reason, count] : reasons)
        printf("  %5d %s\n", count, reason.c_str());
    return refused > 0 ? 1 : 0;
}

int runCrossTier(int argc, char** argv){
    const Selection selection = selectShaders(argc, argv, "256");
    const std::string outPath = option(argc, argv, "--out");
    const int textureSize = 256;
    const std::vector<float> texture = probeTexture(textureSize);
    const SpirvImage image(textureSize, textureSize, texture);
    GlPass gl;
    std::string error;
    if (!gl.create(error)){
        fprintf(stderr, "procmat-sweep: %s\n", error.c_str());
        return 1;
    }
    printf("procmat-sweep crosstier: GL_RENDERER %s\n", gl.renderer().c_str());
    const Target glTarget = targets[0];
    json rows = json::array();
    int errors = 0, overLimit = 0;
    shadercompiler::initialize_process();
    for (const json& shader : selection.shaders){
        const std::string name = shader["shader"];
        json row = {{"shader", name}, {"node", shader["node"]}, {"variant", shader["variant"]}};
        const CompileResult compiled = compileShader(name, readText(selection.corpus / (name + ".frag")), glTarget);
        if (!compiled.ok){
            row["error"] = "compile: " + compiled.log.substr(0, 500);
            rows.push_back(row);
            errors++;
            continue;
        }
        std::vector<float> glFloats;
        std::vector<uint8_t> glPixels, cpuPixels;
        GlPass::Timings timings;
        const std::vector<float> uniforms = floatsOf(shader["uniforms"]);
        if (!gl.render(compiled.vertexOutput, compiled.output, uniforms, textureSize, texture, selection.size, glFloats, timings, error)){
            row["error"] = "gl: " + error.substr(0, 500);
            rows.push_back(row);
            errors++;
            continue;
        }
        glPixels.resize(glFloats.size());
        for (size_t i = 0; i < glFloats.size(); i++)
            glPixels[i] = quantize(glFloats[i]);
        row["gl_compile_link_ms"] = timings.compileLinkMs;
        row["gl_first_draw_ms"] = timings.firstDrawMs;
        row["gl_draw_ms"] = timings.drawMs;
        const std::unique_ptr<SpirvProgram> program = SpirvProgram::load(compiled.spirv.data(), compiled.spirv.size(), error);
        if (!program){
            row["error"] = "interpreter: " + error;
            rows.push_back(row);
            errors++;
            continue;
        }
        const InterpretResult result = interpret(*program, uniforms, &image, selection.size, selection.threads, &cpuPixels);
        const double mpixels = (double)selection.size * selection.size / 1e6;
        row["cpu_seconds_per_mpixel"] = result.seconds / mpixels;
        row["gl_seconds_per_mpixel"] = timings.drawMs / 1000.0 / mpixels;
        row["too_long"] = result.tooLong;
        // the worst channel's statistics, in 8-bit steps
        double worstMean = 0.0;
        int worstP99 = 0, worstMax = 0;
        for (int channel = 0; channel < 4; channel++){
            int histogram[256] = {};
            for (size_t i = (size_t)channel; i < glPixels.size(); i += 4)
                histogram[std::abs((int)glPixels[i] - (int)cpuPixels[i])]++;
            const size_t pixels = glPixels.size() / 4;
            size_t sum = 0, seen = 0;
            int p99 = -1, maximum = 0;
            for (int d = 0; d < 256; d++){
                sum += (size_t)histogram[d] * d;
                seen += histogram[d];
                if (histogram[d])
                    maximum = d;
                if (p99 < 0 && seen * 100 >= pixels * 99)
                    p99 = d;
            }
            worstMean = std::max(worstMean, (double)sum / (double)pixels);
            worstP99 = std::max(worstP99, p99);
            worstMax = std::max(worstMax, maximum);
        }
        row["mean"] = worstMean;
        row["p99"] = worstP99;
        row["max"] = worstMax;
        // AC8's limits, per output and per pass
        if (worstMean > 2.0 || worstP99 > 4){
            printf("  over AC8: %s (%.2f mean, %d at p99)\n", name.c_str(), worstMean, worstP99);
            overLimit++;
        }
        rows.push_back(row);
    }
    shadercompiler::finalize_process();
    std::ofstream(outPath) << json({{"shaders", rows}, {"size", selection.size}, {"renderer", gl.renderer()}}).dump(1);
    printf("procmat-sweep crosstier: %zu shaders, %d errors, %d over AC8\n", rows.size(), errors, overLimit);
    return errors > 0 || overLimit > 0 ? 1 : 0;
}

int runBench(int argc, char** argv){
    const std::string shaderName = option(argc, argv, "--shader");
    Selection selection = selectShaders(argc, argv, "1024");
    const int repeat = std::stoi(option(argc, argv, "--repeat", "3"));
    const bool useGl = option(argc, argv, "--gl", "1") == "1";
    for (const json& shader : selection.shaders){
        const std::string name = shader["shader"];
        if (name != shaderName)
            continue;
        const std::vector<float> uniforms = floatsOf(shader["uniforms"]);
        const int textureSize = 256;
        const std::vector<float> texture = probeTexture(textureSize);
        const SpirvImage image(textureSize, textureSize, texture);
        shadercompiler::initialize_process();
        const Target target = targets[0];
        const auto compileStart = std::chrono::steady_clock::now();
        const CompileResult compiled = compileShader(name, readText(selection.corpus / (name + ".frag")), target);
        const double compileMs = millisecondsSince(compileStart);
        std::string error;
        const std::unique_ptr<SpirvProgram> program = SpirvProgram::load(compiled.spirv.data(), compiled.spirv.size(), error);
        if (!program){
            printf("%s: %s\n", name.c_str(), error.c_str());
            return 1;
        }
        const double mpixels = (double)selection.size * selection.size / 1e6;
        double best = 1e30;
        for (int i = 0; i < repeat; i++){
            const InterpretResult result = interpret(*program, uniforms, &image, selection.size, selection.threads, nullptr);
            best = std::min(best, result.seconds);
        }
        printf("%s: glslang+SPIRV-Cross %.1f ms; interpreter (%d threads, %zu instructions): %.3f s at %d^2, %.3f s per Mpixel\n",
               name.c_str(), compileMs, selection.threads, program->instructionCount(), best, selection.size, best / mpixels);
        if (useGl){
            GlPass gl;
            std::vector<float> pixels;
            GlPass::Timings timings;
            if (!gl.create(error) || !gl.render(compiled.vertexOutput, compiled.output, uniforms, textureSize, texture, selection.size, pixels, timings, error)){
                printf("%s: GL failed: %s\n", name.c_str(), error.c_str());
                return 1;
            }
            printf("%s: %s: compile+link %.1f ms, first draw %.1f ms, draw %.1f ms at %d^2 (%.3f s per Mpixel)\n",
                   name.c_str(), gl.renderer().c_str(), timings.compileLinkMs, timings.firstDrawMs, timings.drawMs, selection.size, timings.drawMs / 1000.0 / mpixels);
        }
        shadercompiler::finalize_process();
        return 0;
    }
    fprintf(stderr, "procmat-sweep: no shader %s\n", shaderName.c_str());
    return 1;
}

// One fragment shader to SPIR-V: the fixtures of the interpreter's unit test
int runSpirv(int argc, char** argv){
    const fs::path frag = option(argc, argv, "--frag");
    shadercompiler::initialize_process();
    const std::string source = readText(frag);
    shadercompiler::args_t args = shadercompiler::initialize_args();
    args.useBuffers = true;
    args.fileBuffers["fixture.frag"] = source;
    args.frag_file = "fixture.frag";
    args.lang = shadercompiler::LANG_SPIRV;
    args.version = 10;
    std::string log;
    args.error_output = &log;
    std::vector<shadercompiler::input_t> inputs;
    std::vector<shadercompiler::spirv_t> spirv(1);
    if (!shadercompiler::load_input(inputs, args) || !shadercompiler::compile_to_spirv(spirv, inputs, args)){
        fprintf(stderr, "%s: %s\n", frag.c_str(), log.c_str());
        return 1;
    }
    shadercompiler::finalize_process();
    std::ofstream file(option(argc, argv, "--out"), std::ios::binary);
    file.write(reinterpret_cast<const char*>(spirv[0].bytecode.data()), (std::streamsize)(spirv[0].bytecode.size() * 4));
    return 0;
}

}

int main(int argc, char** argv){
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "spirv")
        return runSpirv(argc, argv);
    if (mode == "compile")
        return runCompile(argc, argv);
    if (mode == "interpret")
        return runInterpret(argc, argv);
    if (mode == "crosstier")
        return runCrossTier(argc, argv);
    if (mode == "bench")
        return runBench(argc, argv);
    fprintf(stderr, "usage: procmat-sweep compile|interpret|crosstier|bench ...\n");
    return 2;
}
