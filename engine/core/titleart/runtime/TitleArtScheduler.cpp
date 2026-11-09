// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtScheduler.h"

#include "Engine.h"
#include "Log.h"
#include "device/QualityResolver.h"
#include "io/Data.h"
#include "io/FileData.h"
#include "render/SystemRender.h"
#include "texture/TextureData.h"
#include "titleart/TitleArtAssets.h"
#include "titleart/render/TitleArtRenderer.h"
#include "util/FontFace.h"
#include "util/FontRegistry.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    bool exists(const std::string& path){
        FileData* file = FileData::newFile(path.c_str());
        const bool found = file != nullptr && file->length() > 0;
        delete file;
        return found;
    }

    // A player's assets: the exported files at their project-relative paths (the build keeps
    // them, R52/R54), found by the reference's path hint under the assets directory
    class PlayerAssetResolver : public AssetResolver {
    public:
        explicit PlayerAssetResolver(std::string assetsPrefix): prefix(std::move(assetsPrefix)){}

        ResolvedTitleAsset resolve(const TitleAssetRef& ref, TitleAssetKind) const override{
            ResolvedTitleAsset out;
            if (ref.path.empty()) return out;
            for (const std::string& candidate : {prefix + ref.path, ref.path}){
                if (!exists(candidate)) continue;
                out.found = true;
                out.path = candidate;
                out.assetPath = ref.path;
                out.guid = ref.guid;
                return out;
            }
            return out;
        }

        std::shared_ptr<FontFace> loadFont(const ResolvedTitleAsset& asset, int faceIndex, std::string* error) const override{
            if (!asset.found){
                if (error) *error = "the font is missing";
                return nullptr;
            }
            std::lock_guard<std::mutex> guard(lock);
            const auto key = std::make_pair(asset.path, faceIndex);
            auto it = fonts.find(key);
            if (it != fonts.end()) return it->second;
            std::shared_ptr<FontFace> face = FontFace::loadFile(asset.path, faceIndex, error, FontFaceRasterizer::Optional);
            if (face) fonts[key] = face;
            return face;
        }

        bool readBytes(const ResolvedTitleAsset& asset, std::vector<uint8_t>& out, std::string* error) const override{
            Data data;
            if (!asset.found || data.open(asset.path.c_str()) != FILEDATA_OK){
                if (error) *error = "cannot read " + asset.assetPath;
                return false;
            }
            out.assign(data.getMemPtr(), data.getMemPtr() + data.length());
            return true;
        }

        bool loadImage(const ResolvedTitleAsset& asset, TitleImageData& out, std::string* error) const override{
            TextureData data;
            if (!asset.found || !data.loadTextureFromFile(asset.path.c_str())){
                if (error) *error = "cannot read the image " + asset.assetPath;
                return false;
            }
            const int width = data.getWidth();
            const int height = data.getHeight();
            const int channels = data.getChannels();
            const uint8_t* pixels = static_cast<const uint8_t*>(data.getData());
            if (!pixels || width <= 0 || height <= 0 || channels < 1 || channels > 4){
                if (error) *error = "the image " + asset.assetPath + " has no 8-bit pixels";
                return false;
            }
            out.width = width;
            out.height = height;
            out.rgba.assign((size_t)width * height * 4, 255);
            for (size_t i = 0; i < (size_t)width * height; i++){
                const uint8_t* src = pixels + i * channels;
                uint8_t* dst = out.rgba.data() + i * 4;
                if (channels <= 2){
                    dst[0] = dst[1] = dst[2] = src[0];
                    dst[3] = channels == 2 ? src[1] : 255;
                }else{
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                    dst[3] = channels == 4 ? src[3] : 255;
                }
            }
            return true;
        }

        std::shared_ptr<FontFace> builtinFont() const override{
            return FontRegistry::getBuiltInLatin();
        }

    private:
        std::string prefix;
        mutable std::mutex lock;
        mutable std::map<std::pair<std::string, int>, std::shared_ptr<FontFace>> fonts;
    };

}

double jasperengine::titleart::titlePlatformFactor(DevicePlatformGroup group){
    switch (group){
        case DevicePlatformGroup::Mobile: return 6.0;
        case DevicePlatformGroup::Web: return 4.0;
        case DevicePlatformGroup::Desktop: return 1.0;
    }
    return 1.0;
}

const char* jasperengine::titleart::titleQualityTierFor(DeviceTier tier){
    switch (tier){
        case DeviceTier::Low: return "low";
        case DeviceTier::Medium: return "medium";
        case DeviceTier::High: case DeviceTier::Ultra: return "high";
    }
    return "high";
}

// ==================================================================== the scheduler

TitleArtScheduler::TitleArtScheduler() = default;

TitleArtScheduler& TitleArtScheduler::get(){
    static TitleArtScheduler* scheduler = new TitleArtScheduler();
    return *scheduler;
}

void TitleArtScheduler::add(TitleArtLive* live){
    if (std::find(lives.begin(), lives.end(), live) == lives.end()) lives.push_back(live);
}

void TitleArtScheduler::remove(TitleArtLive* live){
    lives.erase(std::remove(lives.begin(), lives.end(), live), lives.end());
}

DevicePlatformGroup TitleArtScheduler::platformGroup() const{
    return QualityResolver::getPlatformGroup();
}

double TitleArtScheduler::frameBudgetMs(bool editorEditMode) const{
    if (budgetOverrideMs > 0.0) return budgetOverrideMs;
    if (editorEditMode) return config.editorFrameBudgetMs;
    return config.frameBudgetMs[(size_t)platformGroup()];
}

void TitleArtScheduler::setFrameBudgetOverride(double ms){
    budgetOverrideMs = ms <= 0.0 ? 0.0 : std::min(16.0, std::max(0.25, ms));
}

bool jasperengine::titleart::parseTitleArtRuntimeSettings(const std::string& text, TitleArtRuntimeSettings& settings){
    const Json root = Json::parse(text, nullptr, false);
    if (!root.is_object() || root.value("version", 0) != 1) return false;
    auto group = [&root](const char* key, double* values, double lo, double hi){
        auto it = root.find(key);
        if (it == root.end() || !it->is_object()) return;
        static const char* const names[DEVICE_PLATFORM_GROUP_COUNT] = {"desktop", "mobile", "web"};
        for (size_t g = 0; g < DEVICE_PLATFORM_GROUP_COUNT; g++){
            auto value = it->find(names[g]);
            if (value != it->end() && value->is_number()) values[g] = std::min(hi, std::max(lo, value->get<double>()));
        }
    };
    group("frame_budget_ms", settings.frameBudgetMs, 0.25, 16.0);
    group("release_delay_s", settings.releaseDelaySeconds, 0.0, 600.0);
    group("cache_budget_mib", settings.cacheBudgetMiB, 1.0, 4096.0);
    auto height = root.find("reference_height");
    if (height != root.end() && height->is_number_integer()) settings.referenceHeight = std::max(0, height->get<int>());
    return true;
}

void TitleArtScheduler::configure(const TitleArtRuntimeSettings& settings){
    config = settings;
    if (playerResolver) resolver.reset();
}

TitleArtRenderer& TitleArtScheduler::renderer(){
    if (!rendererInstance){
        // R20: the cache budget bounds the renderer's per-step caches; the pool holds the
        // intermediates of the largest active render plus the display images
        const double cacheMiB = config.cacheBudgetMiB[(size_t)platformGroup()];
        RendererLimits rendererLimits;
        rendererLimits.nodeCacheBytes = (size_t)(cacheMiB * 1024.0 * 1024.0);
        rendererLimits.poolBytes = (size_t)(cacheMiB * 4.0 * 1024.0 * 1024.0);
        rendererInstance = std::make_unique<TitleArtRenderer>(rendererLimits);
    }
    return *rendererInstance;
}

void TitleArtScheduler::setAssetResolver(std::shared_ptr<const AssetResolver> assets){
    resolver = std::move(assets);
    playerResolver = false;
}

const AssetResolver* TitleArtScheduler::assetResolver(){
    if (!resolver){
        resolver = std::make_shared<PlayerAssetResolver>(config.assetsPrefix);
        playerResolver = true;
    }
    return resolver.get();
}

void TitleArtScheduler::setJobFactoryForTesting(TitleJobFactory factory){
    testFactory = std::move(factory);
}

void TitleArtScheduler::setClockForTesting(double seconds){
    testClock = true;
    clock = seconds;
}

size_t TitleArtScheduler::poolBytes() const{
    if (!rendererInstance) return 0;
    const TitleTargetPoolStats& stats = rendererInstance->poolStats();
    return stats.bytesLive + stats.bytesIdle;
}

void TitleArtScheduler::releaseAll(){
    for (TitleArtLive* live : lives) live->dropGpu();
    if (rendererInstance) rendererInstance->trim();
}

TitleFrameReport TitleArtScheduler::schedule(const std::vector<TitleArtLive*>& entries, const TitleJobFactory& factory, uint32_t budgetUs,
                                             double platformFactor){
    TitleFrameReport report;
    std::vector<TitleArtLive*> order;
    for (TitleArtLive* live : entries) if (live->runnable()) order.push_back(live);
    // R13: priority, then visible before not visible, then the oldest request
    std::stable_sort(order.begin(), order.end(), [](const TitleArtLive* a, const TitleArtLive* b){
        if (a->priority() != b->priority()) return a->priority() > b->priority();
        if (a->visible() != b->visible()) return a->visible();
        return a->requestOrder() < b->requestOrder();
    });
    double spent = 0.0;
    for (TitleArtLive* live : order){
        bool ran = false;
        while (live->hasWork() && !live->jobDone()){
            const uint32_t next = live->nextCostUs(platformFactor);
            // R15: whole steps while they fit; the first step of the frame always runs
            if (report.steps > 0 && spent + next > budgetUs) break;
            const double before = spent;
            if (!live->runStep(factory, platformFactor, budgetUs, spent)) break;
            report.steps++;
            if (spent - before > budgetUs) report.overBudgetSteps++;
            ran = true;
            if (spent >= budgetUs) break;
        }
        if (ran) report.order.push_back(live);
        if (report.steps > 0 && spent >= budgetUs) break;
    }
    report.spentUs = spent;
    return report;
}

void TitleArtScheduler::runFrame(double dt, bool editorEditMode){
    if (!testClock){
        clock += std::isfinite(dt) && dt > 0.0 ? dt : 0.0;
    }
    // R22: a lost device took every target: the live titles render again from their documents
    const uint64_t context = SystemRender::getContextId();
    if (contextId != 0 && context != contextId) releaseAll();
    contextId = context;
    if (lives.empty()) return;

    const DevicePlatformGroup group = platformGroup();
    const double factor = titlePlatformFactor(group);
    const uint32_t budgetUs = (uint32_t)std::max(1.0, frameBudgetMs(editorEditMode) * 1000.0);
    bool anyWork = false;
    for (TitleArtLive* live : lives) anyWork = anyWork || live->hasWork();
    if (anyWork){
        const TitleJobFactory factory = testFactory ? testFactory : makeRendererJobFactory(renderer(), assetResolver());
        schedule(lives, factory, budgetUs, factor);
    }

    limits.cacheBytes = (size_t)(config.cacheBudgetMiB[(size_t)group] * 1024.0 * 1024.0) / 2;
    size_t cacheInUse = 0;
    for (TitleArtLive* live : lives) live->endFrame(clock, std::isfinite(dt) ? std::max(0.0, dt) : 0.0, limits, cacheInUse);
    // R20: shown-before images within the budget, the largest holder's oldest first
    size_t total = 0;
    for (TitleArtLive* live : lives) total += live->cacheBytes();
    while (total > limits.cacheBytes){
        TitleArtLive* largest = nullptr;
        for (TitleArtLive* live : lives) if (!largest || live->cacheBytes() > largest->cacheBytes()) largest = live;
        const size_t before = largest ? largest->cacheBytes() : 0;
        if (!largest || !largest->evictOldest()) break;
        total -= before - largest->cacheBytes();
    }
    // R20: the pool goes after the Release Delay without a render
    if (anyWork){
        idleSeconds = 0.0;
    }else if (rendererInstance){
        idleSeconds += std::isfinite(dt) ? std::max(0.0, dt) : 0.0;
        if (idleSeconds >= config.releaseDelaySeconds[(size_t)group]){
            rendererInstance->trim();
            idleSeconds = -1e30;    // trimmed: nothing more until the next render
        }
    }
}
