// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleJobRunner.h"

#include "TitleGpu.h"
#include "TitleRenderServices.h"

#include "titleart/TitleArtHash.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::titleart;

struct TitleJobRunner::Lut : TitleLut {
    std::shared_ptr<TitleCacheEntry> entry;
};

TitleJobRunner::TitleJobRunner(TitleRenderServices& renderServices) : services(renderServices) {
}

TitleJobRunner::~TitleJobRunner() = default;

const TitleArtCaps& TitleJobRunner::caps() const {
    return services.caps;
}

TargetRef* TitleJobRunner::acquire(const TargetKey& key) {
    std::string why;
    TargetRef ref = services.pool.acquire(key, &why);
    if (!ref) {
        if (failure.empty()) failure = why;
        return nullptr;
    }
    lent.push_back(std::make_unique<TargetRef>(std::move(ref)));
    return lent.back().get();
}

void TitleJobRunner::release(TargetRef* target) {
    for (auto it = lent.begin(); it != lent.end(); ++it) {
        if (it->get() == target) {
            lent.erase(it);
            return;
        }
    }
}

const TargetRef* TitleJobRunner::recall(uint64_t key) {
    std::shared_ptr<TitleCacheEntry> entry = services.cache.find(key);
    if (!entry || !entry->target) return nullptr;
    recalled.push_back(entry);
    return &entry->target;
}

bool TitleJobRunner::keep(uint64_t key, TargetRef* target) {
    // a result drawn while a program was missing (the step runs again) or after a failure is
    // not the result
    if (!pending.empty() || !failure.empty()) return false;
    for (auto it = lent.begin(); it != lent.end(); ++it) {
        if (it->get() != target) continue;
        auto entry = std::make_shared<TitleCacheEntry>();
        entry->key = key;
        entry->target = std::move(**it);
        services.cache.insert(entry);
        recalled.push_back(entry);
        lent.erase(it);
        return true;
    }
    return false;
}

const TitleLut* TitleJobRunner::lut(const TitleLutData& data) {
    uint64_t key = hashCombine(fnv1a64("effect lut"), (uint64_t)data.format);
    key = hashCombine(key, fnv1a64(std::string_view(reinterpret_cast<const char*>(data.bytes.data()), data.bytes.size())));
    std::shared_ptr<TitleCacheEntry> entry = services.cache.find(key);
    if (!entry) {
        const size_t texel = colorFormatBytes(data.format);
        if (texel == 0 || data.bytes.size() != (size_t)TITLE_LUT_SIZE * texel) {
            if (failure.empty()) failure = "a lookup of the wrong size";
            return nullptr;
        }
        entry = std::make_shared<TitleCacheEntry>();
        entry->key = key;
        entry->upload = std::make_unique<TitleUpload>();
        if (!entry->upload->create("title art effect LUT", TITLE_LUT_SIZE, 1, data.format, data.bytes.data(), data.bytes.size(),
                                   TextureFilter::LINEAR)) {
            if (failure.empty()) failure = "the lookup could not be uploaded";
            return nullptr;
        }
        services.cache.insert(entry);
    }
    auto lutHandle = std::make_unique<Lut>();
    lutHandle->entry = entry;
    luts.push_back(std::move(lutHandle));
    return luts.back().get();
}

TextureRender* TitleJobRunner::textureOf(const TitleSource& source) {
    if (source.target) return source.target->get() ? source.target->get()->colorTexture() : nullptr;
    if (source.lut) {
        const auto* lut = static_cast<const Lut*>(source.lut);
        return lut->entry ? lut->entry->texture() : nullptr;
    }
    return const_cast<TextureRender*>(source.texture);
}

bool TitleJobRunner::run(const TitlePass& pass) {
    if (!pass.output || !pass.output->get()) {
        if (failure.empty()) failure = std::string("a pass of ") + (pass.label ? pass.label : "an effect") + " has no output";
        return false;
    }
    std::string why;
    const uint32_t number = (uint32_t)pass.program | (uint32_t)pass.variant;
    if (!services.gpu.ensureProgram(number, &why)) {
        if (!why.empty()) {
            if (failure.empty()) failure = why;
            return false;
        }
        // drawn when the step runs again (the contract above)
        if (std::find(pending.begin(), pending.end(), number) == pending.end()) pending.push_back(number);
        return true;
    }
    TitleGpuPass g;
    g.program = (TitleRenderProgram)(uint32_t)pass.program;
    g.variant = (uint32_t)pass.variant;
    g.target = pass.output->get();
    g.scissor = pass.scissor;
    g.blend = pass.blend;
    g.keepParam0 = true;
    for (int i = 0; i < TITLE_PASS_VEC4S; i++) g.params.set(i, pass.params[i][0], pass.params[i][1], pass.params[i][2], pass.params[i][3]);
    // the runner's own members of the block (TitlePasses.h): the target and the whole render
    const TargetKey& key = pass.output->key();
    g.params.set(0, (float)key.width, (float)key.height, pass.pxScale, 0.0f);
    g.params.set(1, pass.frameUv[0], pass.frameUv[1], pass.frameUv[2], pass.frameUv[3]);
    for (int i = 0; i < TITLE_PASS_TEXTURES; i++) {
        if (!pass.textures[i].empty()) g.tex[i] = textureOf(pass.textures[i]);
    }
    for (int i = 0; i < TITLE_PASS_RAW_TEXTURES; i++) {
        if (!pass.raw[i].empty()) g.raw[i] = textureOf(pass.raw[i]);
    }
    if (!services.gpu.run(g, why)) {
        if (failure.empty()) failure = why;
        return false;
    }
    return true;
}

bool TitleJobRunner::clear(TargetRef& target) {
    std::string why;
    const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (!services.gpu.clear(target.get(), zero, why)) {
        if (failure.empty()) failure = why;
        return false;
    }
    return true;
}

const std::string& TitleJobRunner::error() const {
    return failure;
}

void TitleJobRunner::clearError() {
    failure.clear();
}
