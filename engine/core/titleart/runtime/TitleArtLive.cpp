// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtLive.h"
#include "TitleArtScheduler.h"

#include "Log.h"
#include "render/SystemRender.h"
#include "texture/Texture.h"
#include "texture/Framebuffer.h"
#include "titleart/TitleArtAssets.h"
#include "titleart/render/TitleArtCaps.h"
#include "titleart/render/TitleArtRenderer.h"
#include "titleart/render/TitleGpu.h"
#include "titleart/runtime/TitleGlyphAtlas.h"
#include "titleart/runtime/TitleAnimCompose.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

using namespace jasperengine;
using namespace jasperengine::titleart;

// ==================================================================== the session's pixel path

namespace {

    bool forceRgba8Read = false;
    bool forceRgba8 = false;

    void readForceRgba8(){
        if (forceRgba8Read) return;
        forceRgba8Read = true;
        // R21: read once at start by players and the editor
        if (const char* env = std::getenv("JASPER_TITLEART_FORCE_RGBA8"); env && std::strcmp(env, "1") == 0) forceRgba8 = true;
#if defined(__ANDROID__)
        char value[PROP_VALUE_MAX] = {0};
        if (__system_property_get("debug.jasperengine.titleart_rgba8", value) > 0 && std::strcmp(value, "1") == 0) forceRgba8 = true;
#endif
        if (forceRgba8) Log::print("Title Art: the RGBA8 pixel path is forced (JASPER_TITLEART_FORCE_RGBA8 or debug.jasperengine.titleart_rgba8)");
    }

}

bool jasperengine::titleart::titleArtForceRgba8(){
    readForceRgba8();
    return forceRgba8;
}

void jasperengine::titleart::setTitleArtForceRgba8(bool force, const char* reason){
    readForceRgba8();
    if (force && !forceRgba8 && reason) Log::warn("Title Art: switching this session to the RGBA8 pixel path: %s", reason);
    forceRgba8 = force;
}

// ==================================================================== display images

TextureRender* TitleDisplayImage::texture() const{
    if (prebaked){
        TextureRender* render = prebaked->getRender();
        return render && render->isCreated() ? render : nullptr;
    }
    TitleTarget* t = target.get();
    return t ? t->colorTexture() : nullptr;
}

Framebuffer* TitleDisplayImage::framebuffer() const{
    SokolTitleTarget* t = target.as<SokolTitleTarget>();
    return target && t ? &t->framebufferObject() : nullptr;
}

Framebuffer* TitleDisplayImage::glowFramebuffer() const{
    SokolTitleTarget* t = glow.as<SokolTitleTarget>();
    return glow && t ? &t->framebufferObject() : nullptr;
}

// ==================================================================== the renderer's jobs

namespace {

    bool isAllocationError(const std::string& error){
        return error.rfind("out_of_target_memory", 0) == 0 || error.find("out_of_target_memory") != std::string::npos ||
               error.find("cannot create") != std::string::npos;
    }

    // S80's TitleRenderJob, and the half-resolution glow output after it when asked (R35)
    class RendererJob : public TitleJob {
    public:
        RendererJob(TitleArtRenderer& renderer, const AssetResolver* assets, const TitleJobSpec& spec): spec(spec){
            const bool slow = (spec.testFlags & TITLE_ART_TEST_SLOW_STEPS) != 0;
            scale = slow ? TITLE_ART_SLOW_STEP_FACTOR : 1;
            if (spec.testFlags & TITLE_ART_TEST_FAIL_TARGETS){
                // R21: the request's allocations fail (a device out of memory), as the pool would say
                failure = "out_of_target_memory: no title art target could be allocated (TITLE_ART_TEST_FAIL_TARGETS)";
                allocation = true;
                return;
            }
            RenderRequest request;
            request.doc = spec.doc;
            request.pxScale = spec.pxScale;
            request.region = spec.region;
            request.overrides = spec.overrides;
            request.testFlags = spec.testFlags & ~(TITLE_ART_TEST_FAIL_TARGETS | TITLE_ART_TEST_SLOW_STEPS);
            if (titleArtForceRgba8()) request.testFlags |= TITLE_ART_TEST_NO_FLOAT_TARGETS;
            request.output = TitleOutputFormat::Srgb8;
            request.tier = spec.tier;
            request.assets = assets;
            path = TitleArtCaps::select(SystemRender::getFeatures(), request.testFlags).pathName();
            main = renderer.begin(request);
            if (!main){
                failure = "the document could not be read";
                return;
            }
            if (spec.glow){
                RenderRequest glowRequest = request;
                glowRequest.content = OutputContent::Glow;
                glowRequest.pxScale = spec.pxScale * 0.5f;
                glow = renderer.begin(glowRequest);
            }
        }

        bool step(const StepBudget& budget, StepReport* report) override{
            if (!failure.empty()) return false;
            StepBudget scaled = budget;
            if (scaled.budgetUs > 0) scaled.budgetUs = std::max<uint32_t>(1, scaled.budgetUs / scale);
            TitleRenderJob* job = current();
            if (!job) return true;
            StepReport local;
            job->step(scaled, &local);
            local.estimatedUs *= scale;
            if (report) *report = local;
            if (job->failed()){
                failure = job->error();
                allocation = isAllocationError(failure);
                floatFailure = allocation && std::strcmp(path, "rgba16f") == 0;
                return false;
            }
            return done();
        }

        bool done() const override{
            return failure.empty() && main && main->done() && (!glow || glow->done());
        }
        bool failed() const override{ return !failure.empty(); }
        bool waiting() const override{
            const TitleRenderJob* job = const_cast<RendererJob*>(this)->current();
            return job && job->waiting();
        }
        float progress() const override{
            if (!main) return failure.empty() ? 1.0f : 0.0f;
            if (!glow) return main->progress();
            // the glow is a quarter of the pixels: weigh it so
            return (main->progress() * 4.0f + glow->progress()) / 5.0f;
        }
        uint32_t nextStepCostUs() const override{
            const TitleRenderJob* job = const_cast<RendererJob*>(this)->current();
            return job ? job->nextStepCostUs() * scale : 0;
        }
        void splitOversizedSteps(uint32_t maxUs) override{
            const uint32_t limit = std::max<uint32_t>(1, maxUs / scale);
            if (main) main->splitOversizedSteps(limit);
            if (glow) glow->splitOversizedSteps(limit);
        }
        uint32_t passesDone() const override{ return (main ? main->passesDone() : 0) + (glow ? glow->passesDone() : 0); }
        uint32_t passesTotal() const override{ return (main ? main->passesTotal() : 0) + (glow ? glow->passesTotal() : 0); }
        const std::string& error() const override{ return failure; }
        bool allocationFailure() const override{ return allocation; }
        bool floatTargetFailure() const override{ return floatFailure; }
        const char* pathName() const override{ return path; }

        TitleDisplayImage takeImage() override{
            TitleDisplayImage out;
            if (!main || !main->done()) return out;
            TitleImage& image = main->resultImage();
            out.width = image.width;
            out.height = image.height;
            out.frame = image.frame;
            out.bytes = targetBytes(image.target.key());
            out.target = std::move(image.target);
            if (glow && glow->done()){
                TitleImage& g = glow->resultImage();
                out.glowWidth = g.width;
                out.glowHeight = g.height;
                out.bytes += targetBytes(g.target.key());
                out.glow = std::move(g.target);
            }
            return out;
        }

        std::vector<ShaderKey> shaderKeys() const override{
            std::vector<ShaderKey> keys;
            if (main) keys = main->shaderKeys();
            if (glow) for (const ShaderKey& k : glow->shaderKeys()) if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
            return keys;
        }

    private:
        TitleJobSpec spec;
        std::unique_ptr<TitleRenderJob> main;
        std::unique_ptr<TitleRenderJob> glow;
        std::string failure;
        bool allocation = false;
        bool floatFailure = false;
        const char* path = "rgba8";
        uint32_t scale = 1;

        TitleRenderJob* current(){
            if (main && !main->done()) return main.get();
            if (glow && !glow->done()) return glow.get();
            return nullptr;
        }
    };

}

TitleJobFactory jasperengine::titleart::makeRendererJobFactory(TitleArtRenderer& renderer, const AssetResolver* assets){
    return [&renderer, assets](const TitleJobSpec& spec) -> std::unique_ptr<TitleJob> {
        if (spec.compose) return makeTitleAnimComposeJob(renderer, assets, spec);
        if (spec.atlas) return makeGlyphAtlasJob(renderer, assets, spec);
        return std::make_unique<RendererJob>(renderer, assets, spec);
    };
}

// ==================================================================== TitleArtLive

namespace {

    // R13's age order is across titles: one request clock for all of them
    uint64_t nextRequestSerial(){
        static uint64_t serial = 0;
        return ++serial;
    }

}

TitleArtLive::TitleArtLive(std::string titlePath, TitleArtRenderMode mode, std::shared_ptr<const TitleArtDoc> document, uint64_t documentHash)
    : path(std::move(titlePath)), renderMode(mode), doc(std::move(document)), docHash(documentHash){
}

TitleArtLive::~TitleArtLive(){
    TitleArtScheduler::get().remove(this);
}

void TitleArtLive::setState(const TitleArtLiveState& state, uint64_t stateHash, std::vector<FieldOverride> overrides){
    const bool changed = !hasWanted || stateHash != wantedHash;
    wanted = state;
    if (!changed){
        // priority, visibility and the like may change without a new request
        return;
    }
    hasWanted = true;
    wantedHash = stateHash;
    wantedOverrides = std::move(overrides);
    // a new state ends a failure's retry wait (R19: "stop until the state changes")
    retryWaiting = false;
    retries = 0;
    retryAt = -1.0;
    queueWanted();
}

void TitleArtLive::queueWanted(){
    cacheSwapPending = false;
    // R14: the image already shown
    if (frontImage.valid() && frontImage.hash == wantedHash && !running.job){
        queued = false;
        return;
    }
    // R14: one held in the cache swaps in at the next frame
    for (const Cached& c : cache){
        if (c.image.hash == wantedHash){
            cacheSwapPending = true;
            queued = false;
            return;
        }
    }
    if (running.job && running.hash == wantedHash){
        queued = false;
        return;
    }
    if (running.job && running.started && running.job->progress() < 0.5f){
        // R12: under half done: start again with the new state
        running = Running();
        restartCount++;
    }
    queued = true;
    requestSerial = nextRequestSerial();
}

void TitleArtLive::invalidate(){
    // the same state rendered again from the document (refresh, device loss)
    if (frontImage.valid()) frontImage.hash = 0;
    cache.clear();
    retryWaiting = false;
    retries = 0;
    if (hasWanted){
        if (running.job) running = Running();
        queued = true;
        requestSerial = nextRequestSerial();
    }
}

void TitleArtLive::setPrebaked(TitleDisplayImage image){
    if (!image.valid()) return;
    if (image.hash == wantedHash || !frontImage.valid()){
        frontImage = std::move(image);
        if (frontImage.hash == wantedHash){
            queued = false;
            running = Running();
        }
    }else{
        pushToCache(std::move(image));
    }
}

bool TitleArtLive::hasWork() const{
    return !retryWaiting && (queued || running.job != nullptr);
}

bool TitleArtLive::runnable() const{
    // R13: a hidden title gets no steps unless renderWhenHidden
    return hasWork() && (wanted.visible || wanted.renderWhenHidden);
}

uint32_t TitleArtLive::nextCostUs(double platformFactor) const{
    if (running.job) return (uint32_t)std::ceil(running.job->nextStepCostUs() * platformFactor);
    return queued ? 1u : 0u;
}

void TitleArtLive::startJob(const TitleJobFactory& factory, uint32_t budgetUs, double platformFactor){
    running = Running();
    running.hash = wantedHash;
    running.state = wanted;
    running.overrides = wantedOverrides;
    TitleJobSpec spec;
    spec.doc = doc;
    spec.overrides = wantedOverrides;
    spec.pxScale = wanted.resolution;
    spec.region = {wanted.region.x, wanted.region.y, wanted.region.width, wanted.region.height};
    spec.testFlags = wanted.testFlags;
    spec.glow = wanted.glowOutput;
    spec.label = wanted.label;
    if (renderMode == TitleArtRenderMode::REALTIME && composePose && !composePose->empty()){
        if (!atlas) atlas = std::make_shared<TitleGlyphAtlas>();
        atlas->setLimits(wanted.atlasPageSize, wanted.atlasMaxPages);
        spec.atlas = atlas;
        spec.compose = composePose;
        spec.docHash = docHash;
    }else if (renderMode == TitleArtRenderMode::GLYPH_ATLAS && glyphPlan.fallback.empty() && !glyphPlan.textLayer.empty()){
        // R25-R29: per glyph; a title whose plan falls back renders as Realtime
        if (!atlas) atlas = std::make_shared<TitleGlyphAtlas>();
        atlas->setLimits(wanted.atlasPageSize, wanted.atlasMaxPages);
        spec.atlas = atlas;
        spec.glyphPlan = glyphPlan;
        spec.docHash = docHash;
        spec.titlePath = path;
        spec.glyphPoses = glyphPoses;
    }
    running.job = factory(spec);
    queued = false;
    if (running.job){
        // R15: GPU steps over the budget become scissor tiles (>= 256 x 256)
        const uint32_t maxUs = (uint32_t)std::max(1.0, budgetUs / std::max(platformFactor, 1e-3));
        running.job->splitOversizedSteps(maxUs);
        running.estimatedUs = 0;
    }
}

bool TitleArtLive::runStep(const TitleJobFactory& factory, double platformFactor, uint32_t budgetUs, double& spentUs){
    if (!running.job){
        if (!queued) return false;
        startJob(factory, budgetUs, platformFactor);
        if (!running.job){
            failJob("the renderer refused the document", false, false);
            return false;
        }
    }
    if (running.job->done()) return false;
    running.started = true;
    StepReport report;
    // one step at a time: the scheduler packs steps by their scaled estimates
    running.job->step(StepBudget{1, 0}, &report);
    shadersWaiting = running.job->waiting() && report.steps == 0;
    const double estimate = report.estimatedUs * platformFactor;
    spentUs += std::max(estimate, (double)report.measuredUs);
    running.estimatedUs += (uint32_t)std::min(4.0e9, estimate);
    running.cpuMs += report.measuredUs / 1000.0;
    if (estimate > budgetUs) running.overBudget++;
    totalSteps += report.steps;
    path8 = running.job->pathName();
    if (running.job->failed()){
        failJob(running.job->error(), running.job->allocationFailure(), running.job->floatTargetFailure());
        return false;
    }
    return report.steps > 0;
}

void TitleArtLive::failJob(std::string error, bool allocation, bool floatTarget){
    running = Running();
    if (floatTarget && !titleArtForceRgba8()){
        // R19: the caps said a float target would work and it did not: RGBA8 for the session,
        // and the render starts again at once
        setTitleArtForceRgba8(true, ("a float target failed to allocate (" + error + ")").c_str());
        queued = true;
        return;
    }
    failedState = true;
    failure = std::move(error);
    failedHash = wantedHash;
    if (allocation && retries < 3){
        // R19: retry after 2, 4 and 8 s (FluidSimulator's policy)
        retryWaiting = true;
        retryAt = -1.0;     // endFrame stamps it with the clock
        queued = true;
    }else{
        retryWaiting = true;
        queued = false;
    }
}

void TitleArtLive::pushToCache(TitleDisplayImage image){
    if (!image.valid() || image.hash == 0) return;
    for (auto it = cache.begin(); it != cache.end(); ++it){
        if (it->image.hash == image.hash){
            cache.erase(it);
            break;
        }
    }
    cache.push_front(Cached{std::move(image), ++useClock});
}

bool TitleArtLive::takeFromCache(uint64_t hash, TitleDisplayImage& out){
    for (auto it = cache.begin(); it != cache.end(); ++it){
        if (it->image.hash == hash){
            out = std::move(it->image);
            cache.erase(it);
            return true;
        }
    }
    return false;
}

void TitleArtLive::finishJob(TitleLiveLimits& limits, size_t& cacheBytesInUse){
    (void)limits;
    (void)cacheBytesInUse;
    TitleDisplayImage image = running.job->takeImage();
    image.hash = running.hash;
    lastFrames = running.frames;
    lastGpuMs = running.estimatedUs / 1000.0;
    lastCpuMs = running.cpuMs;
    lastOverBudget = running.overBudget;
    running = Running();
    failedState = false;
    failure.clear();
    retries = 0;
    retryWaiting = false;
    if (!image.valid()) return;
    // R17: the old front becomes the previous image (a crossfade's start) and a cache entry
    if (frontImage.valid()){
        if (wanted.transition == TitleArtTransition::CROSSFADE && wanted.transitionDuration > 0.0f){
            if (previousImage.valid()) pushToCache(std::move(previousImage));
            previousImage = std::move(frontImage);
            fade = 0.0f;
        }else{
            pushToCache(std::move(frontImage));
        }
    }
    frontImage = std::move(image);
    // the latest state was asked while finishing (R12): it renders next
    if (hasWanted && frontImage.hash != wantedHash && !queued && !cacheSwapPending) queueWanted();
}

void TitleArtLive::endFrame(double now, double dt, TitleLiveLimits& limits, size_t& cacheBytesInUse){
    clock = now;
    if (running.job){
        running.frames++;
        if (running.job->done()) finishJob(limits, cacheBytesInUse);
    }
    if (cacheSwapPending){
        TitleDisplayImage image;
        if (takeFromCache(wantedHash, image)){
            if (frontImage.valid()) pushToCache(std::move(frontImage));
            frontImage = std::move(image);
            failedState = false;
            failure.clear();
        }else{
            queued = true;
            requestSerial = nextRequestSerial();
        }
        cacheSwapPending = false;
    }
    if (previousImage.valid()){
        const double duration = std::max(0.001, (double)wanted.transitionDuration);
        fade = (float)std::min(1.0, fade + dt / duration);
        if (fade >= 1.0f) pushToCache(std::move(previousImage));
    }
    if (retryWaiting && failedState && queued){
        if (retryAt < 0.0){
            retryAt = now + limits.retryDelays[std::min(retries, 2)];
        }else if (now >= retryAt){
            retries++;
            retryAt = -1.0;
            retryWaiting = false;
            requestSerial = nextRequestSerial();
        }
    }
}

size_t TitleArtLive::cacheBytes() const{
    size_t bytes = 0;
    for (const Cached& c : cache) bytes += c.image.bytes;
    return bytes;
}

bool TitleArtLive::evictOldest(){
    if (cache.empty()) return false;
    cache.pop_back();
    return true;
}

void TitleArtLive::dropGpu(){
    frontImage = TitleDisplayImage();
    previousImage = TitleDisplayImage();
    cache.clear();
    running = Running();
    if (atlas) atlas->clear();
    if (hasWanted){
        queued = true;
        requestSerial = nextRequestSerial();
    }
}

const TitleDisplayImage* TitleArtLive::previous(float* blend) const{
    if (blend) *blend = fade;
    return previousImage.valid() ? &previousImage : nullptr;
}

TitleArtLiveStatus TitleArtLive::status() const{
    TitleArtLiveStatus s;
    s.stateHash = wantedHash;
    s.displayedHash = frontImage.valid() ? frontImage.hash : 0;
    s.hasImage = frontImage.valid() && (frontImage.target.get() != nullptr || frontImage.prebaked != nullptr);
    s.ready = s.hasImage && frontImage.hash == wantedHash && !cacheSwapPending;
    s.failed = failedState;
    s.error = failure;
    s.pixelPath = path8.empty() ? (titleArtForceRgba8() ? "rgba8" : "") : (path8 == "rgba16f" ? "float" : "rgba8");
    if (s.ready){
        s.pending.clear();
    }else if (failedState && !hasWork()){
        s.pending = "failed";
    }else if (running.job){
        if (shadersWaiting) s.pending = "shaders";
        else s.pending = "rendering " + std::to_string(running.job->passesDone()) + "/" + std::to_string(running.job->passesTotal());
    }else if (queued && !wanted.visible && !wanted.renderWhenHidden){
        s.pending = "hidden";
    }else if (failedState){
        s.pending = "failed";
    }else{
        s.pending = "queued";
    }
    s.displayWidth = frontImage.width;
    s.displayHeight = frontImage.height;
    s.displayBytes = frontImage.bytes + (previousImage.valid() ? previousImage.bytes : 0);
    s.lastFrames = lastFrames;
    s.lastGpuMsEstimate = lastGpuMs;
    s.lastCpuMs = lastCpuMs;
    s.overBudgetSteps = lastOverBudget;
    s.steps = totalSteps;
    if (frontImage.valid()){
        s.frame.frame = {frontImage.frame.x, frontImage.frame.y, frontImage.frame.width, frontImage.frame.height};
        s.frame.alphaBounds = s.frame.frame;
    }
    s.missingCharacters = missingCharacters;
    if (renderMode == TitleArtRenderMode::GLYPH_ATLAS){
        s.glyphUnsafe = glyphPlan.unsafeLabels();
        s.glyphApproximate = glyphPlan.approximateLabels();
        s.glyphFallback = glyphPlan.fallback;
        s.atlasPages = atlas ? (uint32_t)atlas->pageCount() : 0;
    }
    return s;
}
