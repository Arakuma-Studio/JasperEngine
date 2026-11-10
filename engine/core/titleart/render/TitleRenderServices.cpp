// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleRenderServices.h"

#include "render/SystemRender.h"
#include "titleart/TitleArtHash.h"

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::titleart;

// ---- cache entries ----

TextureRender* TitleCacheEntry::texture() const {
    if (upload) return upload->created() ? &upload->texture() : nullptr;
    auto* t = target.as<SokolTitleTarget>();
    return t ? &t->texture() : nullptr;
}

size_t TitleCacheEntry::measure() const {
    if (upload) return upload->bytes();
    if (target) return targetBytes(target.key());
    return 0;
}

std::shared_ptr<TitleCacheEntry> TitleRenderCache::find(uint64_t key) {
    auto it = entries.find(key);
    if (it == entries.end()) {
        misses++;
        return nullptr;
    }
    // a target the pool dropped (device loss) is no entry
    std::shared_ptr<TitleCacheEntry> entry = *it->second;
    if (!entry->upload && !entry->target) {
        total -= entry->bytes;
        order.erase(it->second);
        entries.erase(it);
        misses++;
        return nullptr;
    }
    order.splice(order.begin(), order, it->second);
    hits++;
    return entry;
}

void TitleRenderCache::insert(const std::shared_ptr<TitleCacheEntry>& entry) {
    auto it = entries.find(entry->key);
    if (it != entries.end()) {
        total -= (*it->second)->bytes;
        order.erase(it->second);
        entries.erase(it);
    }
    entry->bytes = entry->measure();
    order.push_front(entry);
    entries[entry->key] = order.begin();
    total += entry->bytes;
    evict();
}

void TitleRenderCache::evict() {
    // the newest entry stays even when it alone passes the limit (its render needs it)
    while (total > limit && order.size() > 1) {
        std::shared_ptr<TitleCacheEntry> last = order.back();
        total -= last->bytes;
        entries.erase(last->key);
        order.pop_back();
    }
}

void TitleRenderCache::clear() {
    order.clear();
    entries.clear();
    total = 0;
}

// ---- services ----

TitleRenderServices::TitleRenderServices(const RendererLimits& rendererLimits)
    : pool(allocator, rendererLimits.poolBytes), cache(rendererLimits.nodeCacheBytes), limits(rendererLimits) {
    content = makeDefaultTitleContentProvider();
}

TitleRenderServices::~TitleRenderServices() {
    cache.clear();
    pool.clear();
    gpu.release();
}

TargetKey TitleRenderServices::keyFor(TitleBuffer buffer, int width, int height) const {
    const TitleBufferFormat& f = caps.format(buffer);
    TargetKey key;
    key.width = width;
    key.height = height;
    key.format = f.format;
    key.filter = f.filter;
    return key;
}

TargetRef TitleRenderServices::acquire(TitleBuffer buffer, int width, int height, std::string* error) {
    return pool.acquire(keyFor(buffer, width, height), error);
}

bool TitleRenderServices::checkDevice() {
    const uint64_t current = SystemRender::getContextId();
    if (contextId == 0) {
        contextId = current;
        return false;
    }
    if (current == contextId && !simulatedLoss) return false;
    simulatedLoss = false;
    contextId = current;
    reset();
    return true;
}

void TitleRenderServices::reset() {
    cache.clear();
    pool.clear();
    gpu.release();
    title3D.reset();
    generation++;
}

// ---- LUTs ----

std::shared_ptr<TitleCacheEntry> TitleRenderServices::gradientLut(const TitleGradient& gradient) {
    const uint64_t key = hashCombine(0x6772616469656e74ull, gradientKey(gradient));
    if (auto hit = cache.find(key)) return hit;
    auto entry = std::make_shared<TitleCacheEntry>();
    entry->key = key;
    uint8_t texels[256 * 4];
    bakeGradient(gradient, texels);
    entry->upload = std::make_unique<TitleUpload>();
    if (!entry->upload->create("title art gradient", 256, 1, ColorFormat::RGBA, texels, sizeof(texels), TextureFilter::LINEAR)) return nullptr;
    cache.insert(entry);
    return entry;
}

std::shared_ptr<TitleCacheEntry> TitleRenderServices::curveLut(const TitleCurve& curve) {
    const uint64_t key = hashCombine(0x6375727665ull, curveKey(curve));
    if (auto hit = cache.find(key)) return hit;
    auto entry = std::make_shared<TitleCacheEntry>();
    entry->key = key;
    uint8_t texels[256 * 4];
    bakeCurve16(curve, texels);
    entry->upload = std::make_unique<TitleUpload>();
    if (!entry->upload->create("title art curve", 256, 1, ColorFormat::RGBA, texels, sizeof(texels), TextureFilter::NEAREST)) return nullptr;
    cache.insert(entry);
    return entry;
}

std::shared_ptr<TitleCacheEntry> TitleRenderServices::elementLuts(const TitleGradient& fill, const TitleGradient& finish,
                                                                  const TitleCurve* gloss, const TitleCurve* satin) {
    uint64_t key = hashCombine(0x656c656d656e74ull, gradientKey(fill));
    key = hashCombine(key, gradientKey(finish));
    key = hashCombine(key, gloss ? curveKey(*gloss) : 1);
    key = hashCombine(key, satin ? curveKey(*satin) : 2);
    if (auto hit = cache.find(key)) return hit;
    auto entry = std::make_shared<TitleCacheEntry>();
    entry->key = key;
    std::vector<uint8_t> texels(256 * 4 * 4, 0);
    bakeGradient(fill, texels.data());
    bakeGradient(finish, texels.data() + 256 * 4);
    TitleCurve identity;
    identity.points = {{0.0, 0.0, false}, {1.0, 1.0, false}};
    bakeCurve16(gloss ? *gloss : identity, texels.data() + 2 * 256 * 4);
    bakeCurve16(satin ? *satin : identity, texels.data() + 3 * 256 * 4);
    entry->upload = std::make_unique<TitleUpload>();
    // linear: the gradient rows are sampled between texels; the curve rows are read with
    // texelFetch, and sampling at a row's centre never mixes rows
    if (!entry->upload->create("title art element LUTs", 256, 4, ColorFormat::RGBA, texels.data(), texels.size(), TextureFilter::LINEAR)) {
        return nullptr;
    }
    cache.insert(entry);
    return entry;
}
