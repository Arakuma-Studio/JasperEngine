// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The GPU fluid of a FluidSimulationComponent: Pavel Dobryakov's WebGL-Fluid-Simulation (MIT,
// Copyright (c) 2017 Pavel Dobryakov) step(), render(), applyBloom(), applySunrays(), splat()
// and multipleSplats() on the engine's render layer. Every pass is a fullscreen triangle into
// a whole target in its own render pass (CameraRender), drawn with the dynamic pipeline path
// (ObjectRender::endLoadDynamic, beginDraw(PipelineState)); the kernels are fluid.frag's, the
// shared GLSL of effects/lib/fam_fluid_solver.glsl and fam_fluid_look.glsl.
//
// What differs from his page, on purpose: a fixed-step clock (simulationRate Hz, at most
// maxSubsteps a frame, the rest of the time dropped) instead of min(frame dt, 1/60); a seeded
// random source (seed 0: a new seed at each start) instead of Math.random(); a pointer's
// movement over a frame is spread over the frame's steps (one splat each, at the stroke's
// point by then) instead of one splat a frame; the bloom's additive upsample is a sum into a
// copy of each level instead of a ONE, ONE blend into it (the same image, no float blending
// needed); the dithering noise is procedural; the display writes an RGBA8 image (opaque: over
// the back colour; transparent: premultiplied) rather than the canvas.

#include "FluidSimulator.h"

#include "pool/ShaderPool.h"
#include "render/CameraRender.h"
#include "render/ObjectRender.h"
#include "render/ShaderRender.h"
#include "render/SystemRender.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <vector>

using namespace jasperengine;

namespace {

    // fluid.frag's passes (fluid.target.z)
    enum SolverPass : int {
        PASS_CURL = 0,
        PASS_VORTICITY = 1,
        PASS_DIVERGENCE = 2,
        PASS_PRESSURE_CLEAR = 3,
        PASS_JACOBI = 4,
        PASS_GRADIENT = 5,
        PASS_ADVECT = 6,
        PASS_SPLAT = 7,
        PASS_COPY = 8
    };

    enum LookPass : int {
        LOOK_BLOOM_PREFILTER = 0,
        LOOK_BLOOM_BLUR = 1,
        LOOK_BLOOM_UP = 2,
        LOOK_BLOOM_FINAL = 3,
        LOOK_SUNRAYS_MASK = 4,
        LOOK_SUNRAYS = 5,
        LOOK_BLUR = 6,
        LOOK_DISPLAY = 7,
        LOOK_COMPOSITE = 8
    };

    // ShaderType::FLUID properties ('Lok', 'Mfl')
    const uint32_t PROPERTY_LOOK = 1u << 0;
    const uint32_t PROPERTY_MANUAL_FILTERING = 1u << 1;

    // u_fs_fluid, member for member (fluid.frag)
    struct FluidBlock {
        float target[4];
        float texel[4];
        float params[4];
        float vector[4];
        float color[4];
        float flags[4];
    };
    static_assert(sizeof(FluidBlock) == 96, "u_fs_fluid is 6 x vec4");

    void set4(float out[4], float x, float y, float z, float w){
        out[0] = x;
        out[1] = y;
        out[2] = z;
        out[3] = w;
    }

    uint32_t& disabledFeatures(){
        static uint32_t features = 0;
        return features;
    }

    int& testMaxTextureSize(){
        static int size = 0;
        return size;
    }

    // what the device offers, the test overrides applied
    struct Caps {
        bool floatTargets = false;      // RGBA16F renderable
        bool linear = false;            // RGBA16F filterable
        bool renderRG = false, filterRG = false;
        bool renderR = false, filterR = false;
    };

    Caps currentCaps(){
        const RenderFeatures& f = SystemRender::getFeatures();
        const uint32_t off = disabledFeatures();
        Caps caps;
        caps.floatTargets = f.renderRGBA16F && !(off & FLUID_TEST_NO_FLOAT_TARGETS);
        caps.linear = f.filterRGBA16F && !(off & FLUID_TEST_NO_LINEAR_FILTER);
        caps.renderRG = f.renderRG16F && !(off & FLUID_TEST_NO_RG16F);
        caps.filterRG = caps.renderRG && f.filterRG16F && caps.linear;
        caps.renderR = f.renderR16F && !(off & FLUID_TEST_NO_R16F);
        caps.filterR = caps.renderR && f.filterR16F && caps.linear;
        return caps;
    }

    size_t bytesPerTexel(ColorFormat format){
        switch (format){
            case ColorFormat::R16F: return 2;
            case ColorFormat::RG16F: return 4;
            case ColorFormat::RGBA16F: return 8;
            case ColorFormat::RGBA32F: return 16;
            case ColorFormat::R32F: return 4;
            default: return 4;
        }
    }

    // PCG32 (O'Neill, pcg-random.org): every random choice of a simulation, from its seed
    struct Pcg32 {
        uint64_t state = 0x853c49e6748fea9bULL;
        uint64_t inc = 0xda3e39cb94b95bdbULL;

        void seed(uint64_t value){
            state = 0u;
            inc = (value << 1u) | 1u;
            next();
            state += value;
            next();
        }

        uint32_t next(){
            const uint64_t old = state;
            state = old * 6364136223846793005ULL + inc;
            const uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
            const uint32_t rot = (uint32_t)(old >> 59u);
            return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
        }

        // [0, 1), as Math.random()
        float uniform(){
            return (float)(next() >> 8) * (1.0f / 16777216.0f);
        }
    };

    // his HSVtoRGB
    Vector3 hsvToRgb(float h, float s, float v){
        const float i = std::floor(h * 6.0f);
        const float f = h * 6.0f - i;
        const float p = v * (1.0f - s);
        const float q = v * (1.0f - f * s);
        const float t = v * (1.0f - (1.0f - f) * s);
        switch (((int)i % 6 + 6) % 6){
            case 0: return Vector3(v, t, p);
            case 1: return Vector3(q, v, p);
            case 2: return Vector3(p, v, t);
            case 3: return Vector3(p, q, v);
            case 4: return Vector3(t, p, v);
            default: return Vector3(v, p, q);
        }
    }

    struct Size {
        int width = 0, height = 0;
    };

    const int MAX_SIDE = 8192;

    // the largest side a target may have: the device's (its largest 2D texture), at most
    // MAX_SIDE
    int maxSide(){
        int side = MAX_SIDE;
        const int device = SystemRender::getFeatures().maxTextureSize2D;
        if (device > 0)
            side = std::min(side, device);
        if (testMaxTextureSize() > 0)
            side = std::min(side, testMaxTextureSize());
        return std::max(side, 1);
    }

    // his getResolution: `resolution` on the short side, resolution x aspect on the long one;
    // a long side over `limit` is brought to it, the short one scaled with it (the aspect kept)
    Size resolutionOf(unsigned int resolution, float aspect, int limit){
        double ratio = (std::isfinite(aspect) && aspect > 0.0f) ? (double)aspect : 1.0;
        if (ratio < 1.0)
            ratio = 1.0 / ratio;
        double shortSide = (double)std::max(1u, resolution);
        double longSide = shortSide * ratio;
        if (longSide > (double)limit){
            shortSide *= (double)limit / longSide;
            longSide = (double)limit;
        }
        const int shortPixels = std::clamp((int)std::lround(std::min(shortSide, (double)limit)), 1, limit);
        const int longPixels = std::clamp((int)std::lround(longSide), 1, limit);
        Size size;
        if (aspect > 1.0f){
            size.width = longPixels;
            size.height = shortPixels;
        }else{
            size.width = shortPixels;
            size.height = longPixels;
        }
        return size;
    }

    uint64_t fnv(uint64_t h, const void* data, size_t size){
        const unsigned char* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; i++){
            h ^= p[i];
            h *= 1099511628211ull;
        }
        return h;
    }

    template<typename T>
    uint64_t fnvValue(uint64_t h, const T& value){
        return fnv(h, &value, sizeof(T));
    }

    // one render target of the simulation: colour only, one sample, clamped
    struct FluidTarget {
        Framebuffer framebuffer;
        int width = 0, height = 0;
        ColorFormat format = ColorFormat::RGBA16F;
        bool linear = true;
        size_t bytes = 0;

        bool create(int w, int h, ColorFormat f, bool lin, const char* label){
            FramebufferDesc desc;
            desc.width = w;
            desc.height = h;
            desc.color = f;
            desc.depth = DepthStencilFormat::NONE;
            desc.sampleCount = 1;
            desc.minFilter = desc.magFilter = lin ? TextureFilter::LINEAR : TextureFilter::NEAREST;
            desc.wrapU = desc.wrapV = TextureWrap::CLAMP_TO_EDGE;
            desc.label = label;
            if (!framebuffer.create(desc))
                return false;
            width = w;
            height = h;
            format = f;
            linear = lin;
            bytes = (size_t)w * (size_t)h * bytesPerTexel(f);
            return true;
        }

        bool matches(int w, int h, ColorFormat f, bool lin){
            return framebuffer.isCreated() && width == w && height == h && format == f && linear == lin;
        }

        TextureRender& texture(){
            return framebuffer.getRender().getColorTexture();
        }

        ~FluidTarget(){
            framebuffer.destroy();
        }
    };

    using TargetPtr = std::unique_ptr<FluidTarget>;

    // his DoubleFBO: read the current image, write the other, swap
    struct FluidPair {
        TargetPtr images[2];
        int current = 0;

        FluidTarget& read(){ return *images[current]; }
        FluidTarget& write(){ return *images[1 - current]; }
        void swap(){ current = 1 - current; }
        bool exists() const{ return images[0] && images[1]; }
        void reset(){
            images[0].reset();
            images[1].reset();
            current = 0;
        }
    };

}

struct FluidSimulator::Impl {
    struct Program {
        std::shared_ptr<ShaderRender> shader;
        uint32_t properties = 0;
        bool requested = false;
        bool loaded = false;
        ObjectRender object;
        int blockSlot = -1;
        std::pair<int, int> textures[3] = {{-1, -1}, {-1, -1}, {-1, -1}};
    };

    struct Pointer {
        bool down = false;
        Vector2 position = Vector2(0.0f, 0.0f);   // where it is
        Vector2 applied = Vector2(0.0f, 0.0f);    // where its splats reached
        Vector3 color = Vector3(0.0f, 0.0f, 0.0f);
    };

    Impl() {}

    Program solver;
    Program look;
    TextureRender placeholder;
    bool placeholderCreated = false;
    CameraRender pass;

    FluidPair velocity, dye, pressure;
    TargetPtr divergence, curl;
    TargetPtr bloom;
    std::vector<TargetPtr> bloomDown, bloomUp;
    TargetPtr sunrays, sunraysTemp;
    // stable, shared with the textures showing it (they keep the object, not its GPU side,
    // alive after the simulator): outputTexture is one of them
    std::shared_ptr<Framebuffer> output = std::make_shared<Framebuffer>();
    Texture outputTexture{output.get()};
    int outputWidth = 0, outputHeight = 0;

    // targets that could not be made: not tried again every frame. Optional ones (bloom,
    // sunrays) stay off while their size is the failed one; the required ones wait
    // REQUIRED_RETRY_FRAMES updates before the next attempt
    static constexpr int REQUIRED_RETRY_FRAMES = 120;
    uint64_t bloomFailedKey = 0, sunraysFailedKey = 0;
    uint64_t requiredFailedKey = 0;
    int requiredRetryIn = 0;
    std::string requiredError;
    std::string optionalFailure;                // the last optional target that failed
    std::string bloomFailure, sunraysFailure;   // why bloom / sunrays are off
    std::string degraded;       // what this frame had to turn off (getError() after a draw)

    bool manual = false;
    bool compact = false;
    float aspect = 1.0f;

    Pcg32 random;
    uint32_t seed = 0;
    double accumulator = 0.0;
    double colorTimer = 0.0;
    std::map<int, Pointer> pointers;

    bool started = false;       // reset at least once since the targets were made
    bool needClear = false;     // zero the fields at the next frame (a reset)
    int startBurst = -1;        // the start burst's splats at the next frame (-1 none, 0 random)
    bool outputDirty = true;
    uint64_t lookKey = 0;

    FluidStats stats;
    std::string error;

    // --- programs ---

    bool ensurePlaceholder(){
        if (placeholderCreated)
            return true;
        static unsigned char texel[4] = {0, 0, 0, 0};
        void* data[1] = {texel};
        size_t size[1] = {4};
        placeholderCreated = placeholder.createTexture("fluid placeholder", 1, 1, ColorFormat::RGBA, TextureType::TEXTURE_2D, 1, data, size,
                                                       TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::CLAMP_TO_EDGE,
                                                       TextureWrap::CLAMP_TO_EDGE);
        return placeholderCreated;
    }

    void unload(Program& program){
        if (program.loaded)
            program.object.destroy();
        program.loaded = false;
        program.blockSlot = -1;
        for (auto& slot : program.textures)
            slot = {-1, -1};
        if (program.requested){
            program.shader.reset();
            ShaderPool::remove(ShaderType::FLUID, program.properties);
        }
        program.requested = false;
    }

    // false while the variant builds (the editor and runtime compile in the background) or
    // when it could not be built (error)
    bool load(Program& program, uint32_t properties){
        if (program.requested && program.properties != properties)
            unload(program);
        if (program.loaded)
            return true;
        program.properties = properties;
        program.requested = true;
        if (ShaderPool::isShaderBuildFailed(ShaderType::FLUID, properties)){
            error = "the fluid shader " + ShaderPool::getShaderStr(ShaderType::FLUID, properties) + " could not be built";
            return false;
        }
        program.shader = ShaderPool::get(ShaderType::FLUID, properties);
        if (!program.shader || !program.shader->isCreated()){
            error = ShaderPool::isShaderBuildFailed(ShaderType::FLUID, properties)
                    ? "the fluid shader " + ShaderPool::getShaderStr(ShaderType::FLUID, properties) + " could not be built"
                    : "the fluid shaders are being compiled";
            return false;
        }
        if (!ensurePlaceholder()){
            error = "the fluid's placeholder texture could not be made";
            return false;
        }
        ShaderData& data = program.shader->shaderData;
        program.blockSlot = data.getUniformBlockIndexByName("u_fs_fluid");
        // (u_fluidRaw<i>: the manual filtering variants' unfilterable images, fluid.frag)
        for (int i = 0; i < 3; i++){
            program.textures[i] = data.getTextureIndexByName("u_fluidTex" + std::to_string(i));
            if (program.textures[i].first < 0)
                program.textures[i] = data.getTextureIndexByName("u_fluidRaw" + std::to_string(i));
        }
        program.object.beginLoad(PrimitiveType::TRIANGLES);
        program.object.setShader(program.shader.get());
        for (const auto& slot : program.textures){
            if (slot.first >= 0)
                program.object.addTexture(slot, ShaderStageType::FRAGMENT, &placeholder);
        }
        if (program.blockSlot < 0 || !program.object.endLoadDynamic(PrimitiveType::TRIANGLES, IndexType::NONE)){
            program.object.destroy();
            error = "the fluid program " + ShaderPool::getShaderStr(ShaderType::FLUID, properties) + " could not be loaded";
            return false;
        }
        program.loaded = true;
        return true;
    }

    bool ensurePrograms(bool manualFiltering){
        // both asked for at once, so they build together; without linear filtering both read
        // the half floats as unfilterable images (sokol validates a filterable one otherwise)
        const uint32_t manualBit = manualFiltering ? PROPERTY_MANUAL_FILTERING : 0u;
        const bool solverReady = load(solver, manualBit);
        const bool lookReady = load(look, PROPERTY_LOOK | manualBit);
        return solverReady && lookReady;
    }

    // --- passes ---

    void bind(Program& program, int index, TextureRender* texture){
        const std::pair<int, int>& slot = program.textures[index];
        if (slot.first >= 0)
            program.object.addTexture(slot, ShaderStageType::FRAGMENT, texture ? texture : &placeholder);
    }

    // a fullscreen pass of `program` writing all of a target
    void draw(Program& program, Framebuffer& framebuffer, int width, int height, ColorFormat format, int passId, FluidBlock& block,
              TextureRender* t0, TextureRender* t1 = nullptr, TextureRender* t2 = nullptr){
        set4(block.target, 1.0f / (float)width, 1.0f / (float)height, (float)passId, 0.0f);
        pass.setColorActions(LoadAction::DONTCARE, StoreAction::STORE);
        pass.setDepthStencilActions(LoadAction::DONTCARE, StoreAction::DONTCARE);
        pass.setLabel("fluid");
        pass.startRenderPass(&framebuffer.getRender());
        PipelineState state;
        state.target.color = format;
        state.target.depth = DepthStencilFormat::NONE;
        state.target.sampleCount = 1;
        state.blend = BlendMode::NONE;
        if (program.object.beginDraw(state)){
            bind(program, 0, t0);
            bind(program, 1, t1);
            bind(program, 2, t2);
            program.object.applyUniformBlock(program.blockSlot, sizeof(FluidBlock), &block);
            program.object.draw(0, 3, 1);
        }
        pass.endRenderPass();
        stats.passesLastFrame++;
    }

    void draw(Program& program, FluidTarget& target, int passId, FluidBlock& block, TextureRender* t0, TextureRender* t1 = nullptr,
              TextureRender* t2 = nullptr){
        draw(program, target.framebuffer, target.width, target.height, target.format, passId, block, t0, t1, t2);
    }

    void clear(FluidTarget& target){
        pass.setColorActions(LoadAction::CLEAR, StoreAction::STORE, Vector4(0.0f, 0.0f, 0.0f, 1.0f));
        pass.setDepthStencilActions(LoadAction::DONTCARE, StoreAction::DONTCARE);
        pass.setLabel("fluid clear");
        pass.startRenderPass(&target.framebuffer.getRender());
        pass.endRenderPass();
        stats.passesLastFrame++;
    }

    // --- targets ---

    // optional: a bloom or sunrays target (the fluid draws without it)
    TargetPtr makeTarget(int w, int h, ColorFormat format, bool linear, const char* label, bool optional = false){
        TargetPtr target = std::make_unique<FluidTarget>();
        const uint32_t off = disabledFeatures();
        const bool refused = (off & FLUID_TEST_FAIL_TARGETS) || (optional && (off & FLUID_TEST_FAIL_OPTIONAL_TARGETS));
        if (refused || !target->create(w, h, format, linear, label)){
            stats.targetFailures++;
            const std::string what = std::string("the fluid could not make its ") + label + " target (" + std::to_string(w) + " x " + std::to_string(h) + ")";
            if (optional)
                optionalFailure = what;
            else
                error = what;
            return nullptr;
        }
        clear(*target);
        outputDirty = true;
        return target;
    }

    // a target of that kind, cleared when it is made
    bool ensureTarget(TargetPtr& target, int w, int h, ColorFormat format, bool linear, const char* label, bool optional = false){
        if (target && target->matches(w, h, format, linear))
            return true;
        target.reset();
        target = makeTarget(w, h, format, linear, label, optional);
        return (bool)target;
    }

    // his resizeDoubleFBO: a new size keeps the field (resampled into the new read image)
    bool ensurePair(FluidPair& pair, int w, int h, ColorFormat format, bool linear, bool keep, const char* label){
        if (pair.exists() && pair.read().matches(w, h, format, linear) && pair.write().matches(w, h, format, linear))
            return true;
        TargetPtr read = makeTarget(w, h, format, linear, label);
        TargetPtr write = read ? makeTarget(w, h, format, linear, label) : nullptr;
        if (!read || !write)
            return false;
        if (keep && pair.exists()){
            FluidBlock block{};
            draw(solver, *read, PASS_COPY, block, nullptr, &pair.read().texture());
        }
        pair.reset();
        pair.images[0] = std::move(read);
        pair.images[1] = std::move(write);
        pair.current = 0;
        outputDirty = true;
        return true;
    }

    void destroyBloom(){
        bloom.reset();
        bloomDown.clear();
        bloomUp.clear();
    }

    void destroySunrays(){
        sunrays.reset();
        sunraysTemp.reset();
    }

    void destroyTargets(){
        velocity.reset();
        dye.reset();
        pressure.reset();
        divergence.reset();
        curl.reset();
        destroyBloom();
        destroySunrays();
        if (output->isCreated())
            output->destroy();
        outputWidth = 0;
        outputHeight = 0;
    }

    // the bloom chain (his initBloomFramebuffers) at that size; false: some level could not
    // be made (the chain is then left empty)
    bool makeBloom(const FluidSimulationComponent& c, Size res){
        if (!ensureTarget(bloom, res.width, res.height, ColorFormat::RGBA16F, true, "fluid bloom", true))
            return false;
        std::vector<Size> levels;
        for (unsigned int i = 0; i < std::min(c.bloomIterations, 16u); i++){
            Size level;
            level.width = res.width >> (i + 1);
            level.height = res.height >> (i + 1);
            if (level.width < 2 || level.height < 2)
                break;
            levels.push_back(level);
        }
        bool same = levels.size() == bloomDown.size() && bloomUp.size() + (levels.empty() ? 0 : 1) == levels.size();
        for (size_t i = 0; same && i < levels.size(); i++)
            same = bloomDown[i]->width == levels[i].width && bloomDown[i]->height == levels[i].height;
        if (same)
            return true;
        bloomDown.clear();
        bloomUp.clear();
        outputDirty = true;
        for (size_t i = 0; i < levels.size(); i++){
            TargetPtr down = makeTarget(levels[i].width, levels[i].height, ColorFormat::RGBA16F, true, "fluid bloom level", true);
            if (!down)
                return false;
            bloomDown.push_back(std::move(down));
            // the sum of the way up at this level (not the smallest)
            if (i + 1 < levels.size()){
                TargetPtr up = makeTarget(levels[i].width, levels[i].height, ColorFormat::RGBA16F, true, "fluid bloom up", true);
                if (!up)
                    return false;
                bloomUp.push_back(std::move(up));
            }
        }
        return true;
    }

    // the key of a set of target sizes and formats (what failed is not tried again for it)
    static uint64_t sizeKey(std::initializer_list<int> values){
        uint64_t h = 14695981039346656037ull;
        for (int value : values)
            h = fnvValue(h, value);
        return h == 0 ? 1 : h;
    }

    bool ensureTargets(const FluidSimulationComponent& c, const Caps& caps){
        manual = !caps.linear;
        const bool linear = !manual;
        // Pavel's getSupportedFormat chain: R16F, else RG16F, else RGBA16F (the velocity needs
        // its filter, the scalar fields are read at texel centres)
        const ColorFormat velocityFormat = (caps.renderRG && (manual || caps.filterRG)) ? ColorFormat::RG16F : ColorFormat::RGBA16F;
        const ColorFormat scalarFormat = caps.renderR ? ColorFormat::R16F : (caps.renderRG ? ColorFormat::RG16F : ColorFormat::RGBA16F);
        const ColorFormat sunraysFormat = caps.filterR ? ColorFormat::R16F : (caps.filterRG ? ColorFormat::RG16F : ColorFormat::RGBA16F);
        compact = velocityFormat == ColorFormat::RG16F && scalarFormat == ColorFormat::R16F;

        const int limit = maxSide();
        const Size simSize = resolutionOf(c.simResolution, aspect, limit);
        const Size dyeSize = resolutionOf(c.dyeResolution, aspect, limit);
        const bool keep = started;

        // the required targets: a size that failed waits before it is tried again
        const uint64_t requiredKey = sizeKey({simSize.width, simSize.height, dyeSize.width, dyeSize.height, (int)velocityFormat,
                                              (int)scalarFormat, linear ? 1 : 0});
        if (requiredKey == requiredFailedKey && requiredRetryIn > 0){
            requiredRetryIn--;
            error = requiredError;
            return false;
        }
        if (!ensureRequired(simSize, dyeSize, velocityFormat, scalarFormat, linear, keep)){
            requiredFailedKey = requiredKey;
            requiredRetryIn = REQUIRED_RETRY_FRAMES;
            requiredError = error;
            return false;
        }
        requiredFailedKey = 0;

        // the bloom chain, only while it is drawn; a size that failed stays off
        const Size bloomSize = resolutionOf(c.bloomResolution, aspect, limit);
        const uint64_t bloomKey = sizeKey({bloomSize.width, bloomSize.height, (int)std::min(c.bloomIterations, 16u)});
        if (c.bloom && !manual && bloomKey != bloomFailedKey){
            if (!makeBloom(c, bloomSize)){
                destroyBloom();
                bloomFailedKey = bloomKey;
                bloomFailure = "bloom is off: " + optionalFailure;
                outputDirty = true;
            }
        }else if (bloom || !bloomDown.empty()){
            destroyBloom();
            outputDirty = true;
        }
        if (c.bloom && !manual && bloomKey == bloomFailedKey)
            degraded = bloomFailure;

        // the sunrays (his initSunraysFramebuffers); the mask is drawn into the dye's write image
        const Size sunraysSize = resolutionOf(c.sunraysResolution, aspect, limit);
        const uint64_t sunraysKey = sizeKey({sunraysSize.width, sunraysSize.height, (int)sunraysFormat});
        if (c.sunrays && !manual && sunraysKey != sunraysFailedKey){
            if (!ensureTarget(sunrays, sunraysSize.width, sunraysSize.height, sunraysFormat, true, "fluid sunrays", true) ||
                !ensureTarget(sunraysTemp, sunraysSize.width, sunraysSize.height, sunraysFormat, true, "fluid sunrays blur", true)){
                destroySunrays();
                sunraysFailedKey = sunraysKey;
                sunraysFailure = "sunrays are off: " + optionalFailure;
                outputDirty = true;
            }
        }else if (sunrays || sunraysTemp){
            destroySunrays();
            outputDirty = true;
        }
        if (c.sunrays && !manual && sunraysKey == sunraysFailedKey)
            degraded += (degraded.empty() ? "" : "; ") + sunraysFailure;
        return true;
    }

    // the fields and the output; false (error set) when one could not be made
    bool ensureRequired(Size simSize, Size dyeSize, ColorFormat velocityFormat, ColorFormat scalarFormat, bool linear, bool keep){
        if (!ensurePair(dye, dyeSize.width, dyeSize.height, ColorFormat::RGBA16F, linear, keep, "fluid dye"))
            return false;
        if (!ensurePair(velocity, simSize.width, simSize.height, velocityFormat, linear, keep, "fluid velocity"))
            return false;
        // a new grid starts these over (his initFramebuffers)
        if (!(divergence && divergence->matches(simSize.width, simSize.height, scalarFormat, false)))
            pressure.reset();
        if (!ensureTarget(divergence, simSize.width, simSize.height, scalarFormat, false, "fluid divergence") ||
            !ensureTarget(curl, simSize.width, simSize.height, scalarFormat, false, "fluid curl") ||
            !ensurePair(pressure, simSize.width, simSize.height, scalarFormat, false, false, "fluid pressure"))
            return false;

        // the output: RGBA8 at the dye's size, the same Framebuffer object (a new version)
        if (!output->isCreated() || outputWidth != dyeSize.width || outputHeight != dyeSize.height){
            if (output->isCreated())
                output->destroy();
            FramebufferDesc desc;
            desc.width = dyeSize.width;
            desc.height = dyeSize.height;
            desc.color = ColorFormat::RGBA;
            desc.depth = DepthStencilFormat::NONE;
            desc.sampleCount = 1;
            desc.minFilter = desc.magFilter = TextureFilter::LINEAR;
            desc.wrapU = desc.wrapV = TextureWrap::CLAMP_TO_EDGE;
            desc.label = "fluid output";
            const bool refused = (disabledFeatures() & FLUID_TEST_FAIL_TARGETS) != 0;
            if (refused || !output->create(desc)){
                if (output->isCreated())
                    output->destroy();
                stats.targetFailures++;
                outputWidth = outputHeight = 0;
                error = "the fluid could not make its output (" + std::to_string(dyeSize.width) + " x " + std::to_string(dyeSize.height) + ")";
                return false;
            }
            outputWidth = dyeSize.width;
            outputHeight = dyeSize.height;
            outputDirty = true;
        }
        return true;
    }

    size_t gpuBytes() const{
        size_t bytes = 0;
        auto add = [&bytes](const TargetPtr& target){
            if (target)
                bytes += target->bytes;
        };
        for (const FluidPair* pair : {&velocity, &dye, &pressure}){
            add(pair->images[0]);
            add(pair->images[1]);
        }
        add(divergence);
        add(curl);
        add(bloom);
        for (const TargetPtr& target : bloomDown)
            add(target);
        for (const TargetPtr& target : bloomUp)
            add(target);
        add(sunrays);
        add(sunraysTemp);
        bytes += (size_t)outputWidth * (size_t)outputHeight * 4u;
        return bytes;
    }

    // --- the simulation ---

    Vector3 generateColor(){
        Vector3 c = hsvToRgb(random.uniform(), 1.0f, 1.0f);
        return c * 0.15f;
    }

    void resetState(const FluidSimulationComponent& c){
        seed = c.seed;
        if (seed == 0){
            std::random_device device;
            do {
                seed = device();
            } while (seed == 0);
        }
        random.seed(seed);
        const FluidStats previous = stats;
        stats = FluidStats();
        stats.seed = seed;
        stats.passesLastFrame = previous.passesLastFrame;
        stats.targetFailures = previous.targetFailures;
        accumulator = 0.0;
        colorTimer = 0.0;
        for (auto& entry : pointers)
            entry.second.applied = entry.second.position;
        needClear = true;
        startBurst = c.splatsOnStart ? (int)c.randomSplatCount : -1;
        started = true;
        outputDirty = true;
    }

    void clearFields(){
        for (FluidPair* pair : {&velocity, &dye, &pressure}){
            if (pair->exists()){
                clear(*pair->images[0]);
                clear(*pair->images[1]);
            }
        }
        if (divergence)
            clear(*divergence);
        if (curl)
            clear(*curl);
    }

    // his splat(): the force into the velocity, the colour into the dye
    void splat(Vector2 position, Vector2 force, Vector3 color, float splatRadius){
        FluidBlock block{};
        float radius = splatRadius / 100.0f;
        if (aspect > 1.0f)
            radius *= aspect;
        set4(block.params, 0.0f, 0.0f, aspect, std::max(radius, 1e-7f));
        set4(block.vector, position.x, position.y, 0.0f, 0.0f);
        set4(block.color, force.x, force.y, 0.0f, 0.0f);
        draw(solver, velocity.write(), PASS_SPLAT, block, nullptr, &velocity.read().texture());
        velocity.swap();
        set4(block.color, color.x, color.y, color.z, 0.0f);
        draw(solver, dye.write(), PASS_SPLAT, block, nullptr, &dye.read().texture());
        dye.swap();
        stats.splats++;
        outputDirty = true;
    }

    // his multipleSplats(): bright random splats with random velocities (0: 5 to 24 of them)
    void multipleSplats(const FluidSimulationComponent& c, unsigned int amount){
        if (amount == 0)
            amount = (unsigned int)(random.uniform() * 20.0f) + 5u;
        for (unsigned int i = 0; i < amount; i++){
            const Vector3 color = generateColor() * 10.0f;
            const float x = random.uniform();
            const float y = random.uniform();
            const float dx = 1000.0f * (random.uniform() - 0.5f);
            const float dy = 1000.0f * (random.uniform() - 0.5f);
            splat(Vector2(x, y), Vector2(dx, dy), color, c.splatRadius);
        }
    }

    // his correctDeltaX / correctDeltaY: a pointer delta in the fluid's square units
    Vector2 correctDelta(Vector2 delta) const{
        if (aspect < 1.0f)
            delta.x *= aspect;
        if (aspect > 1.0f)
            delta.y /= aspect;
        return delta;
    }

    // his updateColors()
    void updateColors(const FluidSimulationComponent& c, double dt){
        if (!c.colorful)
            return;
        colorTimer += dt * (double)c.colorUpdateSpeed;
        if (colorTimer >= 1.0){
            colorTimer = std::fmod(colorTimer, 1.0);
            for (auto& entry : pointers)
                entry.second.color = generateColor();
        }
    }

    // his step(dt)
    void stepOnce(const FluidSimulationComponent& c, double seconds){
        const float dt = (float)seconds;
        FluidTarget& vel = velocity.read();
        const float tx = 1.0f / (float)vel.width;
        const float ty = 1.0f / (float)vel.height;
        FluidBlock block{};
        set4(block.texel, tx, ty, tx, ty);

        draw(solver, *curl, PASS_CURL, block, &velocity.read().texture());

        set4(block.params, dt, c.curl, 0.0f, 0.0f);
        draw(solver, velocity.write(), PASS_VORTICITY, block, &velocity.read().texture(), &curl->texture());
        velocity.swap();

        draw(solver, *divergence, PASS_DIVERGENCE, block, &velocity.read().texture());

        set4(block.params, dt, c.pressure, 0.0f, 0.0f);
        draw(solver, pressure.write(), PASS_PRESSURE_CLEAR, block, nullptr, &pressure.read().texture());
        pressure.swap();

        const unsigned int iterations = std::min(c.pressureIterations, 1000u);
        for (unsigned int i = 0; i < iterations; i++){
            draw(solver, pressure.write(), PASS_JACOBI, block, &divergence->texture(), &pressure.read().texture());
            pressure.swap();
        }

        set4(block.params, dt, c.gradientScale, 0.0f, 0.0f);
        draw(solver, velocity.write(), PASS_GRADIENT, block, &velocity.read().texture(), &pressure.read().texture());
        velocity.swap();

        set4(block.params, dt, c.velocityDissipation, 0.0f, 0.0f);
        draw(solver, velocity.write(), PASS_ADVECT, block, &velocity.read().texture(), &velocity.read().texture());
        velocity.swap();

        FluidTarget& source = dye.read();
        set4(block.texel, tx, ty, 1.0f / (float)source.width, 1.0f / (float)source.height);
        set4(block.params, dt, c.densityDissipation, 0.0f, 0.0f);
        draw(solver, dye.write(), PASS_ADVECT, block, &velocity.read().texture(), &dye.read().texture());
        dye.swap();

        stats.steps++;
        stats.time += seconds;
        outputDirty = true;
    }

    // his applyBloom(): into `bloom`; false when the chain is too short to draw (his early out)
    bool applyBloom(const FluidSimulationComponent& c){
        if (!bloom || bloomDown.size() < 2)
            return false;
        FluidBlock block{};
        const float knee = c.bloomThreshold * c.bloomSoftKnee + 0.0001f;
        set4(block.vector, c.bloomThreshold - knee, knee * 2.0f, 0.25f / knee, c.bloomThreshold);
        draw(look, *bloom, LOOK_BLOOM_PREFILTER, block, &dye.read().texture());

        FluidTarget* last = bloom.get();
        for (TargetPtr& level : bloomDown){
            set4(block.texel, 1.0f / (float)last->width, 1.0f / (float)last->height, 0.0f, 0.0f);
            draw(look, *level, LOOK_BLOOM_BLUR, block, &last->texture());
            last = level.get();
        }
        for (int i = (int)bloomDown.size() - 2; i >= 0; i--){
            set4(block.texel, 1.0f / (float)last->width, 1.0f / (float)last->height, 0.0f, 0.0f);
            draw(look, *bloomUp[(size_t)i], LOOK_BLOOM_UP, block, &last->texture(), &bloomDown[(size_t)i]->texture());
            last = bloomUp[(size_t)i].get();
        }
        set4(block.texel, 1.0f / (float)last->width, 1.0f / (float)last->height, 0.0f, 0.0f);
        set4(block.params, 0.0f, c.bloomIntensity, 0.0f, 0.0f);
        draw(look, *bloom, LOOK_BLOOM_FINAL, block, &last->texture());
        return true;
    }

    // his applySunrays() and blur(sunrays, sunraysTemp, 1); the mask goes into the dye's write
    // image, which every pass writing the dye overwrites whole before it is read again
    bool applySunrays(const FluidSimulationComponent& c){
        if (!sunrays || !sunraysTemp)
            return false;
        FluidBlock block{};
        draw(look, dye.write(), LOOK_SUNRAYS_MASK, block, &dye.read().texture());
        set4(block.params, 0.0f, c.sunraysWeight, 0.0f, 0.0f);
        draw(look, *sunrays, LOOK_SUNRAYS, block, &dye.write().texture());
        set4(block.texel, 1.0f / (float)sunrays->width, 0.0f, 0.0f, 0.0f);
        draw(look, *sunraysTemp, LOOK_BLUR, block, &sunrays->texture());
        set4(block.texel, 0.0f, 1.0f / (float)sunrays->height, 0.0f, 0.0f);
        draw(look, *sunrays, LOOK_BLUR, block, &sunraysTemp->texture());
        return true;
    }

    // his render(): the bloom, the sunrays and the display into the output
    void render(const FluidSimulationComponent& c){
        const bool bloomOn = c.bloom && !manual && applyBloom(c);
        const bool sunraysOn = c.sunrays && !manual && applySunrays(c);
        const bool shadingOn = c.shading && !manual;
        FluidBlock block{};
        set4(block.texel, 1.0f / (float)outputWidth, 1.0f / (float)outputHeight, 0.0f, 0.0f);
        // (the output's alpha convention, FluidSimulator.h: premultiplied for the SCREEN composite,
        // straight for the alpha blending of the sprites, images and meshes showing it)
        const float alphaMode = !c.transparent ? 0.0f : (c.output == FluidOutput::SCREEN ? 1.0f : 2.0f);
        set4(block.color, c.backColor.x, c.backColor.y, c.backColor.z, alphaMode);
        set4(block.flags, shadingOn ? 1.0f : 0.0f, bloomOn ? 1.0f : 0.0f, sunraysOn ? 1.0f : 0.0f, 0.0f);
        if (manual){
            // (the manual filtering look reads the dye, a half float it cannot filter, as its
            // third image: fluid.frag; no bloom nor sunrays then)
            draw(look, *output, outputWidth, outputHeight, ColorFormat::RGBA, LOOK_DISPLAY, block, nullptr, nullptr, &dye.read().texture());
        }else{
            draw(look, *output, outputWidth, outputHeight, ColorFormat::RGBA, LOOK_DISPLAY, block, &dye.read().texture(),
                 bloomOn ? &bloom->texture() : nullptr, sunraysOn ? &sunrays->texture() : nullptr);
        }
        stats.shadingActive = shadingOn;
        stats.bloomActive = bloomOn;
        stats.sunraysActive = sunraysOn;
    }

    uint64_t lookKeyOf(const FluidSimulationComponent& c) const{
        uint64_t h = 14695981039346656037ull;
        h = fnvValue(h, c.shading);
        h = fnvValue(h, c.bloom);
        h = fnvValue(h, c.sunrays);
        h = fnvValue(h, c.transparent);
        h = fnvValue(h, c.output);
        h = fnvValue(h, c.backColor.x);
        h = fnvValue(h, c.backColor.y);
        h = fnvValue(h, c.backColor.z);
        h = fnvValue(h, c.bloomIntensity);
        h = fnvValue(h, c.bloomThreshold);
        h = fnvValue(h, c.bloomSoftKnee);
        h = fnvValue(h, c.sunraysWeight);
        // (the targets these size are remade when they change: drawn again, also paused)
        h = fnvValue(h, c.bloomIterations);
        h = fnvValue(h, c.bloomResolution);
        h = fnvValue(h, c.sunraysResolution);
        return h;
    }

    // the settings of a component (its queues left out), sanitized
    static FluidSimulationComponent settingsOf(FluidSimulationComponent& c){
        std::vector<FluidSplat> splats;
        std::vector<unsigned int> bursts;
        splats.swap(c.pendingSplats);
        bursts.swap(c.pendingBursts);
        FluidSimulationComponent settings = c;
        splats.swap(c.pendingSplats);
        bursts.swap(c.pendingBursts);
        FluidSimulator::sanitizeSettings(settings);
        return settings;
    }

    static bool usable(const FluidSplat& s){
        return std::isfinite(s.position.x) && std::isfinite(s.position.y) && std::isfinite(s.velocity.x) && std::isfinite(s.velocity.y) &&
               std::isfinite(s.color.x) && std::isfinite(s.color.y) && std::isfinite(s.color.z) && std::isfinite(s.radius) && s.radius >= 0.0f;
    }

    // one frame: (re)make, restart, splats, steps, display. False before anything changed
    bool frame(FluidSimulationComponent& component, float aspectRatio, double dt, int forcedSteps, bool clock){
        stats.passesLastFrame = 0;
        error.clear();
        degraded.clear();
        if (!std::isfinite(dt) || dt < 0.0)
            dt = 0.0;
        forcedSteps = std::max(forcedSteps, 0);
        // (what scripts queued while the fluid could not run: the latest ones)
        trimFluidQueue(component.pendingSplats, FLUID_MAX_PENDING_SPLATS);
        trimFluidQueue(component.pendingBursts, FLUID_MAX_PENDING_BURSTS);
        const FluidSimulationComponent c = settingsOf(component);

        const Caps caps = currentCaps();
        if (!caps.floatTargets){
            error = "this device cannot render to half-float (RGBA16F) targets, which the fluid simulation needs";
            return false;
        }
        if (!ensurePrograms(!caps.linear))
            return false;
        aspect = (std::isfinite(aspectRatio) && aspectRatio > 0.0f) ? std::clamp(aspectRatio, 1.0f / 64.0f, 64.0f) : 1.0f;
        if (!ensureTargets(c, caps))
            return false;

        if (component.needReset || !started){
            resetState(c);
            component.needReset = false;
        }
        if (needClear){
            clearFields();
            needClear = false;
        }

        const double h = 1.0 / (double)c.simulationRate;
        updateColors(c, clock ? dt : (double)forcedSteps * h);

        // the queued splats, also while paused (his applyInputs)
        if (startBurst >= 0){
            const unsigned int count = std::min((unsigned int)startBurst, FLUID_MAX_BURST_SPLATS);
            startBurst = -1;
            multipleSplats(c, count);
        }
        for (unsigned int count : component.pendingBursts)
            multipleSplats(c, std::min(count, FLUID_MAX_BURST_SPLATS));
        component.pendingBursts.clear();
        for (const FluidSplat& s : component.pendingSplats){
            // (a raw write may hold anything: a splat that is not finite is dropped)
            if (usable(s))
                splat(s.position, s.velocity, s.color, s.radius > 0.0f ? s.radius : c.splatRadius);
        }
        component.pendingSplats.clear();

        // the clock: whole fixed steps, the substep cap dropping the rest
        int clockSteps = 0;
        if (clock && !c.paused && dt > 0.0){
            accumulator += dt;
            const double due = std::floor(accumulator / h + 1e-9);
            const int cap = (int)c.maxSubsteps;
            if (!(due <= (double)cap)){
                clockSteps = cap;
                accumulator = 0.0;
            }else{
                clockSteps = (int)due;
                accumulator = std::max(0.0, accumulator - (double)clockSteps * h);
            }
        }
        const int steps = clockSteps + std::min(forcedSteps, 100000) + (int)std::min(component.pendingSteps, 100000u);
        component.pendingSteps = 0;

        // the pointers' strokes: spread over the frame's steps, or one splat now when nothing
        // steps (paused, his splats while paused; no time, a paused scene or the editor's
        // brush in edit mode); a running clock between steps keeps them
        struct Stroke {
            Vector2 from, delta;
            Vector3 color;
        };
        std::vector<Stroke> strokes;
        const bool immediate = steps == 0 && (c.paused || !clock || dt <= 0.0);
        if (steps > 0 || immediate){
            for (auto& entry : pointers){
                Pointer& p = entry.second;
                const Vector2 delta = p.position - p.applied;
                if (delta.x != 0.0f || delta.y != 0.0f)
                    strokes.push_back({p.applied, delta, p.color});
                p.applied = p.position;
            }
        }
        if (immediate){
            for (const Stroke& s : strokes)
                splat(s.from + s.delta, correctDelta(s.delta) * c.splatForce, s.color, c.splatRadius);
        }
        for (int k = 0; k < steps; k++){
            const float f = (float)(k + 1) / (float)steps;
            for (const Stroke& s : strokes)
                splat(s.from + s.delta * f, correctDelta(s.delta / (float)steps) * c.splatForce, s.color, c.splatRadius);
            stepOnce(c, h);
        }
        for (auto it = pointers.begin(); it != pointers.end();){
            if (!it->second.down && it->second.applied == it->second.position)
                it = pointers.erase(it);
            else
                ++it;
        }

        const uint64_t key = lookKeyOf(c);
        if (outputDirty || key != lookKey){
            render(c);
            outputDirty = false;
            lookKey = key;
        }

        stats.simWidth = velocity.read().width;
        stats.simHeight = velocity.read().height;
        stats.dyeWidth = dye.read().width;
        stats.dyeHeight = dye.read().height;
        stats.outputWidth = outputWidth;
        stats.outputHeight = outputHeight;
        stats.gpuBytes = gpuBytes();
        stats.seed = seed;
        stats.manualFiltering = manual;
        stats.compactFormats = compact;
        error = degraded;
        return true;
    }

    void releaseAll(){
        destroyTargets();
        // (another device, or the same one later: every size is tried again)
        stats.targetFailures = 0;
        bloomFailedKey = sunraysFailedKey = requiredFailedKey = 0;
        requiredRetryIn = 0;
        unload(solver);
        unload(look);
        if (placeholderCreated)
            placeholder.destroyTexture();
        placeholderCreated = false;
        started = false;
        needClear = false;
        startBurst = -1;
        outputDirty = true;
        stats.gpuBytes = 0;
    }
};

FluidSimulator::FluidSimulator(): impl(new Impl()) {}

FluidSimulator::~FluidSimulator(){
    impl->releaseAll();
    delete impl;
}

bool FluidSimulator::isSupported(std::string* reason){
    const Caps caps = currentCaps();
    if (!caps.floatTargets){
        if (reason)
            *reason = "this device cannot render to half-float (RGBA16F) targets, which the fluid simulation needs";
        return false;
    }
    if (reason)
        reason->clear();
    return true;
}

bool FluidSimulator::update(FluidSimulationComponent& component, float aspectRatio, double dt){
    impl->stats.drawn = impl->frame(component, aspectRatio, dt, 0, true);
    return impl->stats.drawn;
}

void FluidSimulator::reset(const FluidSimulationComponent& component){
    FluidSimulationComponent settings = component;
    sanitizeSettings(settings);
    impl->resetState(settings);
}

void FluidSimulator::sanitizeSettings(FluidSimulationComponent& c){
    const FluidSimulationComponent d;
    // non-finite: the default; then >= 0, or > 0 (else the default) where it divides
    auto nonNegative = [](float& value, float fallback){
        if (!std::isfinite(value))
            value = fallback;
        value = std::max(value, 0.0f);
    };
    auto positive = [](float& value, float fallback, float most){
        if (!std::isfinite(value) || value <= 0.0f)
            value = fallback;
        value = std::min(value, most);
    };
    auto side = [](unsigned int& value){
        value = std::clamp(value, 1u, (unsigned int)MAX_SIDE);
    };
    if (!std::isfinite(c.aspectRatio) || c.aspectRatio < 0.0f)
        c.aspectRatio = 0.0f;
    side(c.simResolution);
    side(c.dyeResolution);
    side(c.bloomResolution);
    side(c.sunraysResolution);
    nonNegative(c.densityDissipation, d.densityDissipation);
    nonNegative(c.velocityDissipation, d.velocityDissipation);
    nonNegative(c.pressure, d.pressure);
    c.pressureIterations = std::min(c.pressureIterations, 1000u);
    nonNegative(c.curl, d.curl);
    nonNegative(c.gradientScale, d.gradientScale);
    positive(c.simulationRate, d.simulationRate, 10000.0f);
    c.maxSubsteps = std::clamp(c.maxSubsteps, 1u, 1000u);
    nonNegative(c.timeScale, d.timeScale);
    positive(c.splatRadius, d.splatRadius, 1.0e6f);
    nonNegative(c.splatForce, d.splatForce);
    nonNegative(c.colorUpdateSpeed, d.colorUpdateSpeed);
    c.colorUpdateSpeed = std::min(c.colorUpdateSpeed, 1.0e6f);
    c.randomSplatCount = std::min(c.randomSplatCount, FLUID_MAX_BURST_SPLATS);
    nonNegative(c.backColor.x, d.backColor.x);
    nonNegative(c.backColor.y, d.backColor.y);
    nonNegative(c.backColor.z, d.backColor.z);
    c.bloomIterations = std::min(c.bloomIterations, 16u);
    nonNegative(c.bloomIntensity, d.bloomIntensity);
    nonNegative(c.bloomThreshold, d.bloomThreshold);
    nonNegative(c.bloomSoftKnee, d.bloomSoftKnee);
    nonNegative(c.sunraysWeight, d.sunraysWeight);
}

void FluidSimulator::step(FluidSimulationComponent& component, float aspectRatio, int steps){
    if (steps <= 0)
        return;
    // not ready yet (shaders building): they run at the next update
    impl->stats.drawn = impl->frame(component, aspectRatio, 0.0, steps, false);
    if (!impl->stats.drawn)
        component.pendingSteps += (unsigned int)steps;
}

void FluidSimulator::pointerDown(int id, Vector2 position){
    if (!std::isfinite(position.x) || !std::isfinite(position.y))
        return;
    Impl::Pointer& pointer = impl->pointers[id];
    pointer.down = true;
    pointer.position = position;
    pointer.applied = position;
    pointer.color = impl->generateColor();
}

void FluidSimulator::pointerMove(int id, Vector2 position){
    auto it = impl->pointers.find(id);
    if (it == impl->pointers.end() || !it->second.down || !std::isfinite(position.x) || !std::isfinite(position.y))
        return;
    it->second.position = position;
}

void FluidSimulator::pointerUp(int id){
    auto it = impl->pointers.find(id);
    if (it != impl->pointers.end())
        it->second.down = false;
}

void FluidSimulator::pointerCancelAll(){
    impl->pointers.clear();
}

Framebuffer* FluidSimulator::getOutputFramebuffer(){
    return impl->output.get();
}

Texture& FluidSimulator::getOutputTexture(){
    return impl->outputTexture;
}

Framebuffer* FluidSimulator::getDyeFramebuffer(){
    return impl->dye.exists() ? &impl->dye.read().framebuffer : nullptr;
}

Framebuffer* FluidSimulator::getVelocityFramebuffer(){
    return impl->velocity.exists() ? &impl->velocity.read().framebuffer : nullptr;
}

const FluidStats& FluidSimulator::getStats() const{
    return impl->stats;
}

const std::string& FluidSimulator::getError() const{
    return impl->error;
}

void FluidSimulator::release(){
    impl->releaseAll();
}

bool FluidSimulator::drawComposite(const PipelineState& state, bool flipY){
    Impl::Program& look = impl->look;
    if (!look.loaded || !impl->output->isCreated())
        return false;
    PipelineState composite = state;
    composite.blend = BlendMode::PREMULTIPLIED;
    composite.depthCompare = CompareFunc::ALWAYS;
    composite.depthWrite = false;
    composite.writeMask = ColorWriteMask::RGBA;
    if (!look.object.beginDraw(composite))
        return false;
    FluidBlock block{};
    set4(block.target, 1.0f, 1.0f, (float)LOOK_COMPOSITE, flipY ? 1.0f : 0.0f);
    impl->bind(look, 0, &impl->output->getRender().getColorTexture());
    impl->bind(look, 1, nullptr);
    impl->bind(look, 2, nullptr);
    look.object.applyUniformBlock(look.blockSlot, sizeof(FluidBlock), &block);
    look.object.draw(0, 3, 1);
    return true;
}

void FluidSimulator::setDisabledFeaturesForTesting(uint32_t features){
    disabledFeatures() = features;
}

void FluidSimulator::setMaxTextureSizeForTesting(int size){
    testMaxTextureSize() = std::max(size, 0);
}
