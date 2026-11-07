// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lua's noise (T2 R67, R68): Noise, static functions with the kernels' bits, and NoiseGraph, a compiled
// Noise Graph with its grids and tasks. Only with the JASPERENGINE_NOISE module: ModuleUsage makes a
// project that names them carry it. Main thread, except what a task's jobs run, which touches no Lua.

#include "LuaBinding.h"

#if JASPERENGINE_NOISE

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "Log.h"
#include "graph/Graph.h"
#include "io/Data.h"
#include "jobs/JobSystem.h"
#include "noise/NoiseGraphRuntime.h"
#include "noise/NoiseKernels.h"
#include "noise/NoiseNodes.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace jasperengine;

namespace {

    struct NoiseStatics {};

    constexpr int64_t kMaxGridPoints = 4000000;
    // A job of an async grid: small enough for 1 ms on the slowest target and the web's 4 ms pump (R68)
    constexpr size_t kAsyncBatchPoints = 512;

    uint32_t seedArg(lua_State* L, int index) {
        if (lua_isnoneornil(L, index)) return 0;
        return static_cast<uint32_t>(static_cast<int64_t>(luaL_checknumber(L, index)));
    }

    noise::Basis basisArg(lua_State* L, int index) {
        if (lua_isnoneornil(L, index)) return noise::Basis::Perlin;
        const std::string name = luaL_checkstring(L, index);
        if (name == "perlin") return noise::Basis::Perlin;
        if (name == "simplex") return noise::Basis::Simplex;
        if (name == "value") return noise::Basis::Value;
        if (name == "cellular") return noise::Basis::Cellular;
        luaL_error(L, "unknown basis '%s': perlin, simplex, value or cellular", name.c_str());
        return noise::Basis::Perlin;
    }

    noise::CellularReturn cellularReturnArg(lua_State* L, int index) {
        if (lua_isnoneornil(L, index)) return noise::CellularReturn::F1;
        const std::string name = luaL_checkstring(L, index);
        if (name == "f1") return noise::CellularReturn::F1;
        if (name == "f2") return noise::CellularReturn::F2;
        if (name == "f2_minus_f1") return noise::CellularReturn::F2MinusF1;
        if (name == "cell_value") return noise::CellularReturn::CellValue;
        luaL_error(L, "unknown cellular return '%s': f1, f2, f2_minus_f1 or cell_value", name.c_str());
        return noise::CellularReturn::F1;
    }

    float floatArg(lua_State* L, int index) {
        return static_cast<float>(luaL_checknumber(L, index));
    }

    template<float (*Fn)(float, float, uint32_t)>
    int point2(lua_State* L) {
        lua_pushnumber(L, Fn(floatArg(L, 1), floatArg(L, 2), seedArg(L, 3)));
        return 1;
    }

    template<float (*Fn)(float, float, float, uint32_t)>
    int point3(lua_State* L) {
        lua_pushnumber(L, Fn(floatArg(L, 1), floatArg(L, 2), floatArg(L, 3), seedArg(L, 4)));
        return 1;
    }

    noise::FractalParams fractalArgs(lua_State* L) {
        noise::FractalParams params;
        params.basis.seed = seedArg(L, 3);
        params.basis.frequency = 1.0f;
        params.octaves = lua_isnoneornil(L, 4) ? 4 : static_cast<int>(luaL_checkinteger(L, 4));
        params.lacunarity = lua_isnoneornil(L, 5) ? 2.0f : floatArg(L, 5);
        params.gain = lua_isnoneornil(L, 6) ? 0.5f : floatArg(L, 6);
        params.basis.basis = basisArg(L, 7);
        return params;
    }

    std::string hashText(uint64_t hash) {
        char text[32];
        std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
        return text;
    }

    struct NoiseGrid {
        std::shared_ptr<const std::vector<float>> values;
        int width = 0, depth = 0;
        float low = 0.0f, high = 0.0f;
        std::string hash;

        float get(int i, int j) const {
            if (!values || i < 0 || j < 0 || i >= width || j >= depth) return 0.0f;
            return (*values)[static_cast<size_t>(j) * static_cast<size_t>(width) + static_cast<size_t>(i)];
        }
    };

    NoiseGrid gridFrom(std::vector<float>&& values, int width, int depth) {
        NoiseGrid grid;
        grid.width = width;
        grid.depth = depth;
        if (!values.empty()) {
            const auto bounds = std::minmax_element(values.begin(), values.end());
            grid.low = *bounds.first;
            grid.high = *bounds.second;
        }
        grid.hash = hashText(noise::hashFloats(values.data(), values.size()));
        grid.values = std::make_shared<const std::vector<float>>(std::move(values));
        return grid;
    }

    // What a grid asks for, from sampleGrid's table
    struct GridRequest {
        noise::GridDomain domain;
        uint32_t seed = 1;
        int output = 0;
    };

    struct TaskState {
        std::shared_ptr<const noise::CompiledGraph> graph;
        GridRequest request;
        std::vector<std::pair<std::string, float>> variables;
        float maxHeight = 100.0f;
        std::vector<float> values;
        noise::EvalScratch scratch;
        std::atomic<size_t> finished{0};
        std::atomic<bool> cancelled{false};
        std::atomic<bool> done{false};
        size_t total = 0;
    };

    void runTaskBatch(const std::shared_ptr<TaskState>& state);

    void submitTaskBatch(const std::shared_ptr<TaskState>& state) {
        jobs::SubmitOptions options;
        options.lane = jobs::Lane::Background;
        options.name = "noise-grid";
        jobs::submit([state]() { runTaskBatch(state); }, options);
    }

    void runTaskBatch(const std::shared_ptr<TaskState>& state) {
        if (state->cancelled.load(std::memory_order_relaxed)) {
            state->done.store(true, std::memory_order_release);
            return;
        }
        const size_t start = state->finished.load(std::memory_order_relaxed);
        const size_t count = std::min(kAsyncBatchPoints, state->total - start);
        const noise::GridDomain& domain = state->request.domain;
        const size_t width = static_cast<size_t>(domain.width);
        std::vector<float> x(count), y(count, static_cast<float>(domain.y)), z(count);
        for (size_t k = 0; k < count; k++) {
            const size_t index = start + k;
            x[k] = static_cast<float>((static_cast<double>(domain.startX + static_cast<int64_t>(index % width)) + 0.5) * domain.unitsPerTexel);
            z[k] = static_cast<float>((static_cast<double>(domain.startZ + static_cast<int64_t>(index / width)) + 0.5) * domain.unitsPerTexel);
        }
        noise::EvalContext context;
        context.seed = state->request.seed;
        context.maxHeight = state->maxHeight;
        context.variables = state->variables;
        state->graph->evaluatePoints(x.data(), y.data(), z.data(), count, context, state->request.output, state->values.data() + start, state->scratch);
        const size_t now = start + count;
        state->finished.store(now, std::memory_order_release);
        if (now >= state->total) {
            state->done.store(true, std::memory_order_release);
            return;
        }
        submitTaskBatch(state);
    }

    struct NoiseGridTask {
        std::shared_ptr<TaskState> state;

        bool isDone() const { return state->done.load(std::memory_order_acquire); }
        float getProgress() const {
            return state->total > 0 ? static_cast<float>(state->finished.load(std::memory_order_acquire)) / static_cast<float>(state->total) : 1.0f;
        }
        void cancel() { state->cancelled.store(true, std::memory_order_relaxed); }
    };

    struct NoiseGraphHandle {
        std::shared_ptr<const noise::CompiledGraph> compiled;
        std::shared_ptr<std::vector<std::pair<std::string, float>>> variables;
        float maxHeight = 100.0f;
        double metresPerUnit = 1.0;     // terrain space: the terrain width its preset recommends

        bool isGridOnly() const { return compiled->isGridOnly(); }
    };

    int outputArg(lua_State* L, const NoiseGraphHandle& handle, int index) {
        const std::vector<noise::OutputInfo>& outputs = handle.compiled->outputs();
        if (lua_isnoneornil(L, index)) {
            const int height = handle.compiled->outputIndex("height");
            return height >= 0 ? height : 0;
        }
        const std::string name = luaL_checkstring(L, index);
        const int output = handle.compiled->outputIndex(name);
        if (output < 0) {
            std::string names;
            for (const noise::OutputInfo& info : outputs) names += (names.empty() ? "" : ", ") + info.name;
            luaL_error(L, "the graph has no output '%s' (it has %s)", name.c_str(), names.c_str());
        }
        return output;
    }

    const char* gridOnlyNode(const NoiseGraphHandle& handle, const std::string*& type) {
        for (const noise::CompiledNodeInfo& info : handle.compiled->nodes()) {
            if (!info.dead && noise::isGridOnlyType(info.type)) {
                type = &info.type;
                return info.id.c_str();
            }
        }
        return nullptr;
    }

    void refuseGridOnly(lua_State* L, const NoiseGraphHandle& handle) {
        const std::string* type = nullptr;
        if (const char* node = gridOnlyNode(handle, type)) {
            luaL_error(L, "node %s (%s) needs a whole grid: use sampleGrid, not sample", node, type->c_str());
        }
    }

    float samplePoint(lua_State* L, const NoiseGraphHandle& handle, float x, float y, float z, uint32_t seed, int output) {
        refuseGridOnly(L, handle);
        noise::EvalScratch scratch(handle.compiled->scratchBytes());
        noise::EvalContext context;
        context.seed = seed;
        context.maxHeight = handle.maxHeight;
        context.variables = *handle.variables;
        float value = 0.0f;
        handle.compiled->evaluatePoints(&x, &y, &z, 1, context, output, &value, scratch);
        return value;
    }

    GridRequest gridArgs(lua_State* L, const NoiseGraphHandle& handle, int index) {
        luaL_checktype(L, index, LUA_TTABLE);
        const luabridge::LuaRef table = luabridge::LuaRef::fromStack(L, index);
        const auto number = [&table](const char* key, double fallback) {
            const luabridge::LuaRef value = table[key];
            return value.isNumber() ? value.unsafe_cast<double>() : fallback;
        };
        GridRequest request;
        const double step = number("step", 1.0);
        const int64_t width = static_cast<int64_t>(number("width", 0.0));
        const int64_t depth = static_cast<int64_t>(number("depth", 0.0));
        if (!(step > 0.0)) luaL_error(L, "sampleGrid: step must be above 0");
        if (width <= 0 || depth <= 0) luaL_error(L, "sampleGrid: width and depth must be above 0");
        if (width * depth > kMaxGridPoints) luaL_error(L, "sampleGrid: at most 4,000,000 points, not %lld", static_cast<long long>(width * depth));
        // Texels, as a Terrain's are: the sample of (i, j) is at the centre of texel start + i, so it has
        // the bits a generation gives there (R23)
        request.domain.startX = static_cast<int64_t>(std::floor(number("x", 0.0) / step + 0.5));
        request.domain.startZ = static_cast<int64_t>(std::floor(number("z", 0.0) / step + 0.5));
        request.domain.width = static_cast<int>(width);
        request.domain.depth = static_cast<int>(depth);
        request.domain.unitsPerTexel = step;
        request.seed = static_cast<uint32_t>(static_cast<int64_t>(number("seed", 1.0)));
        const luabridge::LuaRef output = table["output"];
        if (output.isString()) {
            const std::string name = output.unsafe_cast<std::string>();
            request.output = handle.compiled->outputIndex(name);
            if (request.output < 0) luaL_error(L, "sampleGrid: the graph has no output '%s'", name.c_str());
        } else {
            const int height = handle.compiled->outputIndex("height");
            request.output = height >= 0 ? height : 0;
        }
        return request;
    }

    int noiseGraphLoad(lua_State* L) {
        const char* path = luaL_checkstring(L, 1);
        noise::registerNoiseKind();
        Data data;
        if (data.open(path) != FileErrors::FILEDATA_OK) {
            Log::error("[NoiseGraph] Cannot read %s", path);
            lua_pushnil(L);
            return 1;
        }
        graph::LoadReport report;
        const graph::Graph source = graph::load(data.readString(), report);
        if (!report.ok || source.kind != noise::kNoiseKindId) {
            Log::error("[NoiseGraph] %s is not a Noise Graph%s%s", path, report.error.empty() ? "" : ": ", report.error.c_str());
            lua_pushnil(L);
            return 1;
        }
        float maxHeight = 100.0f;
        double terrainSize = 1000.0;
        const auto preset = source.metadata.find("preset");
        if (preset != source.metadata.end() && preset->is_object()) {
            maxHeight = preset->value("max_height", maxHeight);
            terrainSize = preset->value("terrain_size", terrainSize);
        }
        noise::CompileOptions options;
        options.forbidBakeOnly = true;      // N7
        options.maxHeight = maxHeight;
        const noise::CompileResult result = noise::compile(source, options);
        for (const graph::Issue& issue : result.issues) {
            if (issue.severity == graph::Severity::Error) {
                Log::error("[NoiseGraph] %s: %s%s%s", path, issue.nodeId.empty() ? "" : issue.nodeId.c_str(), issue.nodeId.empty() ? "" : ": ", issue.message.c_str());
            }
        }
        if (!result.graph) {
            lua_pushnil(L);
            return 1;
        }
        NoiseGraphHandle handle;
        handle.compiled = result.graph;
        handle.variables = std::make_shared<std::vector<std::pair<std::string, float>>>();
        handle.maxHeight = maxHeight;
        handle.metresPerUnit = terrainSize;
        if (!luabridge::push(L, handle)) lua_pushnil(L);
        return 1;
    }

}

void LuaBinding::registerNoiseClasses(lua_State *L){
    // A class of statics, not a namespace: the Lua API data lists classes, and that list is how
    // ModuleUsage finds that a script needs this module (R43)
    luabridge::getGlobalNamespace(L)
        .beginClass<NoiseStatics>("Noise")
        .addStaticFunction("perlin", &point2<noise::perlin2>)
        .addStaticFunction("perlin3", &point3<noise::perlin3>)
        .addStaticFunction("simplex", &point2<noise::simplex2>)
        .addStaticFunction("simplex3", &point3<noise::simplex3>)
        .addStaticFunction("value", &point2<noise::value2>)
        .addStaticFunction("value3", &point3<noise::value3>)
        .addStaticFunction("cellular", +[](lua_State* state) -> int {
            lua_pushnumber(state, noise::cellular2(floatArg(state, 1), floatArg(state, 2), seedArg(state, 3), cellularReturnArg(state, 4)));
            return 1;
        })
        .addStaticFunction("cellular3", +[](lua_State* state) -> int {
            lua_pushnumber(state, noise::cellular3(floatArg(state, 1), floatArg(state, 2), floatArg(state, 3), seedArg(state, 4), cellularReturnArg(state, 5)));
            return 1;
        })
        .addStaticFunction("fbm", +[](lua_State* state) -> int {
            lua_pushnumber(state, noise::fbmPoint(fractalArgs(state), floatArg(state, 1), 0.0f, floatArg(state, 2)));
            return 1;
        })
        .addStaticFunction("ridged", +[](lua_State* state) -> int {
            lua_pushnumber(state, noise::ridgedPoint(fractalArgs(state), floatArg(state, 1), 0.0f, floatArg(state, 2)));
            return 1;
        })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<NoiseGrid>("NoiseGrid")
        .addProperty("width", +[](const NoiseGrid* self) { return self->width; })
        .addProperty("depth", +[](const NoiseGrid* self) { return self->depth; })
        .addProperty("min", +[](const NoiseGrid* self) { return self->low; })
        .addProperty("max", +[](const NoiseGrid* self) { return self->high; })
        .addProperty("hash", +[](const NoiseGrid* self) { return self->hash; })
        .addFunction("get", &NoiseGrid::get)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<NoiseGridTask>("NoiseGridTask")
        .addProperty("done", &NoiseGridTask::isDone)
        .addProperty("progress", &NoiseGridTask::getProgress)
        .addFunction("cancel", &NoiseGridTask::cancel)
        .addFunction("result", +[](NoiseGridTask* self, lua_State* state) -> int {
            if (!self->isDone() || self->state->cancelled.load()) {
                lua_pushnil(state);
                return 1;
            }
            std::vector<float> values = self->state->values;
            NoiseGrid grid = gridFrom(std::move(values), self->state->request.domain.width, self->state->request.domain.depth);
            if (!luabridge::push(state, grid)) lua_pushnil(state);
            return 1;
        })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<NoiseGraphHandle>("NoiseGraph")
        .addStaticFunction("load", &noiseGraphLoad)
        .addProperty("isGridOnly", &NoiseGraphHandle::isGridOnly)
        .addProperty("outputs", +[](const NoiseGraphHandle* self, lua_State* state) {
            luabridge::LuaRef names = luabridge::newTable(state);
            int index = 1;
            for (const noise::OutputInfo& info : self->compiled->outputs()) names[index++] = info.name;
            return names;
        })
        .addFunction("setVariable", +[](NoiseGraphHandle* self, const std::string& name, float value) {
            for (auto& [known, current] : *self->variables) {
                if (known == name) {
                    current = value;
                    return;
                }
            }
            self->variables->emplace_back(name, value);
        })
        .addFunction("rangeOf", +[](const NoiseGraphHandle* self, lua_State* state) -> int {
            const int output = outputArg(state, *self, 2);
            const noise::Interval range = self->compiled->rangeOf(output);
            lua_pushnumber(state, range.min);
            lua_pushnumber(state, range.max);
            return 2;
        })
        .addFunction("sample", +[](const NoiseGraphHandle* self, lua_State* state) -> int {
            const uint32_t seed = lua_isnoneornil(state, 4) ? 1u : seedArg(state, 4);
            lua_pushnumber(state, samplePoint(state, *self, floatArg(state, 2), 0.0f, floatArg(state, 3), seed, outputArg(state, *self, 5)));
            return 1;
        })
        .addFunction("sample3", +[](const NoiseGraphHandle* self, lua_State* state) -> int {
            const uint32_t seed = lua_isnoneornil(state, 5) ? 1u : seedArg(state, 5);
            lua_pushnumber(state, samplePoint(state, *self, floatArg(state, 2), floatArg(state, 3), floatArg(state, 4), seed, outputArg(state, *self, 6)));
            return 1;
        })
        .addFunction("sampleGrid", +[](const NoiseGraphHandle* self, lua_State* state) -> int {
            const GridRequest request = gridArgs(state, *self, 2);
            noise::EvalContext context;
            context.seed = request.seed;
            context.maxHeight = self->maxHeight;
            context.metresPerTexel = self->metresPerUnit * request.domain.unitsPerTexel;
            context.variables = *self->variables;
            std::vector<float> values(static_cast<size_t>(request.domain.width) * static_cast<size_t>(request.domain.depth));
            self->compiled->evaluateGrid(request.domain, context, request.output, values.data());
            NoiseGrid grid = gridFrom(std::move(values), request.domain.width, request.domain.depth);
            if (!luabridge::push(state, grid)) lua_pushnil(state);
            return 1;
        })
        .addFunction("sampleGridAsync", +[](const NoiseGraphHandle* self, lua_State* state) -> int {
            refuseGridOnly(state, *self);
            auto task = std::make_shared<TaskState>();
            task->graph = self->compiled;
            task->request = gridArgs(state, *self, 2);
            task->variables = *self->variables;
            task->maxHeight = self->maxHeight;
            task->total = static_cast<size_t>(task->request.domain.width) * static_cast<size_t>(task->request.domain.depth);
            task->values.resize(task->total);
            task->scratch.reserve(self->compiled->scratchBytes());
            submitTaskBatch(task);
            NoiseGridTask handle{task};
            if (!luabridge::push(state, handle)) lua_pushnil(state);
            return 1;
        })
        .endClass();
}

#endif // JASPERENGINE_NOISE
