// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleFlipbookBaker.h"

#include "EditorVersion.h"
#include "Project.h"
#include "TitleAnimRender.h"
#include "asset/AssetFileWrite.h"
#include "titleart/TitleArtRenderHost.h"
#include "util/FileUtils.h"
#include "asset/AssetMeta.h"
#include "asset/Guid.h"
#include "render/SystemRender.h"
#include "subsystem/TitleArtSystem.h"
#include "texsynth/SynthesisRecipe.h"
#include "texture/TextureCookQueue.h"
#include "texture/TextureImportSettings.h"
#include "titleanim/TitleAnimSampling.h"
#include "titleart/TitleArtBake.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleEffectRegistry.h"
#include "titleart/anim/TitleAnimIO.h"
#include "titleart/anim/TitleAnimPlan.h"
#include "util/AssetDatabase.h"
#include "util/Fnv.h"
#include "util/ProjectFileWrites.h"
#include "util/Util.h"
#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace ta = jasperengine::titleanim;
namespace tt = jasperengine::titleart;
namespace fs = std::filesystem;

namespace {

    constexpr int ALIGN = 4;
    constexpr int GUTTER = 4;
    constexpr const char* RECIPE_ANIMATION = "animation_hash";

    int alignUp(int v){
        return (v + ALIGN - 1) / ALIGN * ALIGN;
    }

    std::string hex64(uint64_t v){
        char buf[17];
        std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)v);
        return buf;
    }

    std::string pageMeta(){
        AssetMeta meta;
        meta.guid = Guid::parse(AssetDatabase::generateGuid());
        TextureImportSettings settings;
        // R16: ui, no mips, normal quality, alpha is transparency, at most 4096
        settings.type = TextureImportType::UI;
        settings.quality = TextureCookQuality::Normal;
        settings.mipmaps = false;
        settings.alphaIsTransparency = true;
        settings.maxSize = 4096;
        YAML::Node import(YAML::NodeType::Map);
        import["textureImporter"] = settings.serialize();
        YAML::Emitter out;
        out << YAML::Flow << import;
        meta.setSection("import", out.c_str());
        return meta.serialize();
    }

    std::string guidOfMeta(const fs::path& metaPath, const std::string& staged){
        std::string text = staged;
        if (text.empty() && !assetio::readFile(metaPath, text)) return {};
        AssetMeta meta;
        if (!AssetMeta::parse(text, meta)) return {};
        return meta.guid.str();
    }

    // the animation hash of a page's recipe ("" none)
    std::string pageAnimationHash(const fs::path& png){
        std::string error;
        const std::optional<TitleBakeRecipe> recipe = readTitleBakeRecipe(png, &error);
        if (!recipe) return {};
        return recipe->extra.value(RECIPE_ANIMATION, std::string());
    }

    texsynth::Image crop(const texsynth::Image& src, int x, int y, int w, int h){
        texsynth::Image out = texsynth::Image::make(w, h, 4, 8);
        for (int row = 0; row < h; row++){
            const int sy = y + row;
            if (sy < 0 || sy >= src.height) continue;
            for (int col = 0; col < w; col++){
                const int sx = x + col;
                if (sx < 0 || sx >= src.width) continue;
                std::memcpy(out.pixel(col, row), src.pixel(sx, sy), 4);
            }
        }
        return out;
    }

}

// ==================================================================== figures

void TitleFlipbookBytes::add(int w, int h){
    const uint64_t b4 = (uint64_t)((w + 3) / 4) * (uint64_t)((h + 3) / 4) * 16;
    const uint64_t b6 = (uint64_t)((w + 5) / 6) * (uint64_t)((h + 5) / 6) * 16;
    desktop += b4;
    mobile += b6;
    androidEtc2 += b4;
    web += b4;
}

uint64_t TitleFlipbookBytes::of(const std::string& family) const{
    if (family == "mobile") return mobile;
    if (family == "android_etc2") return androidEtc2;
    if (family == "web") return web;
    return desktop;
}

double TitleFlipbookBudget::of(const std::string& family) const{
    if (family == "mobile") return mobile;
    if (family == "android_etc2") return androidEtc2;
    if (family == "web") return web;
    return desktop;
}

double editor::titleFlipbookFps(const ta::CompiledTitleAnimation& animation){
    return std::min((double)ta::TITLE_ANIM_MAX_FLIPBOOK_FPS, std::max(1.0, (double)animation.flipbookSettings.fps));
}

std::vector<double> editor::titleFlipbookFrameTimes(const ta::CompiledTitleAnimation& animation){
    const double fps = titleFlipbookFps(animation);
    const double d = animation.duration;
    std::vector<double> times;
    if (animation.wrap == ta::AnimWrap::Once){
        const int last = (int)std::ceil(d * fps - 1e-9);
        for (int k = 0; k <= last && times.size() < ta::TITLE_ANIM_MAX_FLIPBOOK_FRAMES; k++) times.push_back(std::min(d, k / fps));
    }else{
        // [0, D): the loop section wraps with no duplicate frame
        const int count = std::max(1, (int)std::ceil(d * fps - 1e-9));
        for (int k = 0; k < count && times.size() < ta::TITLE_ANIM_MAX_FLIPBOOK_FRAMES; k++) times.push_back(k / fps);
    }
    return times;
}

tt::Json TitleFlipbookReport::toJson() const{
    tt::Json bytesJson = tt::Json::object();
    for (const char* f : TITLE_FLIPBOOK_FAMILIES) bytesJson[f] = bytes.of(f);
    return tt::Json{{"animation", animation}, {"needed", needed}, {"frames", frames}, {"unique", unique}, {"pages", pages},
                    {"frame_size", tt::Json::array({frameWidth, frameHeight})},
                    {"frame", tt::Json::array({frame.x, frame.y, frame.width, frame.height})}, {"bytes", bytesJson}, {"files", files}};
}

tt::Json TitleFlipbookResult::toJson() const{
    tt::Json list = tt::Json::array();
    for (const TitleFlipbookReport& r : animations) list.push_back(r.toJson());
    return tt::Json{{"ok", ok}, {"code", code}, {"error", error}, {"animations", list}, {"warnings", warnings}};
}

// ==================================================================== <file>.anim.json without frames

ta::TitleAnimData editor::titleAnimDataOf(const tt::TitleArtDoc& doc, const fs::path& document, const std::string& sourceHash, uint32_t bakeVersion,
                                          std::vector<std::string>* warnings){
    ta::TitleAnimData data;
    data.sourceHash = sourceHash;
    data.bakeVersion = bakeVersion;
    const ta::TitleAnimationSet set = tt::readAnimationSet(doc);
    data.defaultName = set.defaultName;
    // the file there now: its flipbooks are kept where the pages still hold this animation
    ta::TitleAnimData previous;
    std::string error;
    std::string text;
    const fs::path animPath(document.string() + ta::TITLE_ANIM_DATA_SUFFIX);
    if (assetio::readFile(animPath, text)) ta::readAnimData(text, previous, nullptr, &error);
    for (const ta::TitleAnimation& a : set.animations){
        ta::TitleAnimDataEntry e;
        e.authored = a;
        e.authored.tracks.clear();
        for (const ta::TitleTrack& t : a.tracks) if (t.layer.empty()) e.authored.tracks.push_back(t);
        e.authored.unknown.clear();
        const ta::TitleAnimDataEntry* old = previous.find(a.name);
        if (old && !old->compiled.flipbook.empty()){
            bool fresh = true;
            const std::string hash = hex64(tt::animationHash(doc, a));
            for (const ta::FlipbookPage& page : old->compiled.flipbook.pages){
                if (pageAnimationHash(document.parent_path() / page.file) != hash){
                    fresh = false;
                    break;
                }
            }
            if (fresh){
                e.compiled.flipbook = old->compiled.flipbook;
                e.compiled.bakedCost = old->compiled.bakedCost;
            }else if (warnings){
                warnings->push_back("flipbook_stale: animation '" + a.name + "' changed since its flipbook was baked: export_title {animations} bakes it again");
            }
        }
        ta::compileTitleOnly(e.authored, e.compiled, nullptr);
        data.animations.push_back(std::move(e));
    }
    return data;
}

// ==================================================================== the job

struct TitleFlipbookBakeJob::Work {
    const ta::CompiledTitleAnimation* animation = nullptr;
    const ta::TitleAnimation* authored = nullptr;
    std::vector<double> times;
    std::vector<texsynth::Image> frames;        // the canvas-sized frames, rendered
    TitleFlipbookReport report;
};

TitleFlipbookBakeJob::TitleFlipbookBakeJob(Project* project, fs::path document, tt::TitleArtDoc doc, TitleFlipbookOptions options)
    : project(project), documentPath(std::move(document)), doc(std::make_shared<tt::TitleArtDoc>(std::move(doc))), options(std::move(options)){
}

TitleFlipbookBakeJob::~TitleFlipbookBakeJob() = default;

void TitleFlipbookBakeJob::fail(const std::string& code, const std::string& error){
    outcome.ok = false;
    outcome.code = code;
    outcome.error = error;
    finished = true;
}

float TitleFlipbookBakeJob::progress() const{
    size_t total = 0, done = 0;
    for (size_t i = 0; i < work.size(); i++){
        total += work[i]->times.size();
        done += i < current ? work[i]->times.size() : (i == current ? frameIndex : 0);
    }
    return finished ? 1.0f : (total ? (float)done / (float)total : 0.0f);
}

std::string TitleFlipbookBakeJob::progressText() const{
    if (current < work.size())
        return "flipbook of '" + work[current]->report.animation + "': frame " + std::to_string(frameIndex) + "/" + std::to_string(work[current]->times.size());
    return finished ? "done" : "writing";
}

bool TitleFlipbookBakeJob::begin(){
    started = true;
    renderer = std::make_unique<TitleAnimRenderer>(project, doc);
    authoredSet = std::make_unique<ta::TitleAnimationSet>(tt::readAnimationSet(*doc));
    const tt::TitleAnimPlan& plan = renderer->plan(TitleArtRenderMode::BAKED);
    for (const ta::CompiledTitleAnimation& a : renderer->library().animations){
        if (!options.animations.empty() && std::find(options.animations.begin(), options.animations.end(), a.name) == options.animations.end()) continue;
        auto w = std::make_unique<Work>();
        w->animation = &a;
        w->authored = authoredSet->find(a.name);
        w->report.animation = a.name;
        w->report.id = a.id;
        const tt::AnimationPlan* ap = plan.find(a.name);
        w->report.needed = ap && ap->flipbook;
        if (w->report.needed) w->times = titleFlipbookFrameTimes(a);
        w->report.frames = (int)w->times.size();
        work.push_back(std::move(w));
    }
    for (const std::string& name : options.animations){
        if (!renderer->library().find(name)){
            fail("unknown_animation", "no animation '" + name + "'");
            return false;
        }
    }
    return true;
}

bool TitleFlipbookBakeJob::renderOne(){
    while (current < work.size() && frameIndex >= work[current]->times.size()){
        current++;
        frameIndex = 0;
    }
    if (current >= work.size()) return true;
    Work& w = *work[current];
    TitleAnimFrameRequest request;
    request.animation = w.animation->name;
    request.time = w.times[frameIndex];
    request.as = TitleAnimRenderAs::Baked;
    request.pxScale = std::max(0.05f, options.outputScale);
    request.companion = false;      // title-live tracks play on the companion over the frames
    TitleAnimFrame frame;
    std::string error;
    if (!renderer->render(request, frame, error)){
        fail("render_failed", "frame " + std::to_string(frameIndex) + " of '" + w.animation->name + "': " + error);
        return false;
    }
    if (w.frames.empty()) w.report.frame = frame.frame;
    w.frames.push_back(std::move(frame.image));
    frameIndex++;
    return false;
}

bool TitleFlipbookBakeJob::step(double budgetMs){
    if (finished) return true;
    if (cancelled){
        fail("cancelled", "the flipbook bake was cancelled; nothing was written");
        return true;
    }
    if (!started && !begin()) return true;
    const auto start = std::chrono::steady_clock::now();
    while (!finished){
        if (renderOne()){
            write();
            return true;
        }
        if (finished) return true;
        const double spent = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (spent >= budgetMs) break;
    }
    return finished;
}

const TitleFlipbookResult& TitleFlipbookBakeJob::run(){
    while (!step(1e9)){}
    return outcome;
}

bool TitleFlipbookBakeJob::write(){
    const std::string docName = documentPath.filename().string();
    const float pxScale = std::max(0.05f, options.outputScale);
    std::vector<texsynth::StagedFile> staged;
    const std::string sourceHash = titleArtSourceHash(*doc);
    const uint32_t bakeVersion = tt::titleArtBakeVersion(*doc);
    std::vector<std::string> warnings;
    ta::TitleAnimData data = titleAnimDataOf(*doc, documentPath, sourceHash, bakeVersion, &warnings);
    for (const std::unique_ptr<Work>& wp : work){
        Work& w = *wp;
        ta::TitleAnimDataEntry* entry = nullptr;
        for (ta::TitleAnimDataEntry& e : data.animations) if (e.authored.name == w.animation->name) entry = &e;
        if (!w.report.needed){
            if (entry){
                entry->compiled.flipbook = ta::CompiledFlipbook();
                entry->compiled.bakedCost = ta::BakedCost();
            }
            outcome.animations.push_back(w.report);
            continue;
        }
        const float scale = pxScale * std::max(0.25f, std::min(1.0f, w.animation->flipbookSettings.scale));
        // R16: one frame size, the union of the frames' alpha bounds with 1 px
        tt::TitleRectI bounds;
        for (const texsynth::Image& f : w.frames){
            const tt::TitleRectI b = TitleArtRenderHost::alphaBoundsOf(f);
            if (b.empty()) continue;
            if (bounds.empty()) bounds = b;
            else{
                const int x0 = std::min(bounds.x, b.x), y0 = std::min(bounds.y, b.y);
                const int x1 = std::max(bounds.x + bounds.width, b.x + b.width), y1 = std::max(bounds.y + bounds.height, b.y + b.height);
                bounds = {x0, y0, x1 - x0, y1 - y0};
            }
        }
        if (bounds.empty()) bounds = {0, 0, 1, 1};
        bounds = {bounds.x - 1, bounds.y - 1, bounds.width + 2, bounds.height + 2};
        const int fw = bounds.width, fh = bounds.height;
        const int cellW = alignUp(fw) + GUTTER, cellH = alignUp(fh) + GUTTER;
        const int maxPage = w.animation->flipbookSettings.maxPage;
        if (cellW - GUTTER > maxPage || cellH - GUTTER > maxPage){
            fail("frame_too_large", "a frame of '" + w.animation->name + "' is " + std::to_string(fw) + " x " + std::to_string(fh) + " px, over max_page " +
                                        std::to_string(maxPage) + ": lower flipbook.scale or raise flipbook.max_page");
            return false;
        }
        // dedupe: a hash, then the bytes
        std::vector<texsynth::Image> unique;
        std::vector<uint64_t> hashes;
        std::vector<int> frameOf;
        for (const texsynth::Image& f : w.frames){
            texsynth::Image cut = crop(f, bounds.x, bounds.y, fw, fh);
            const uint64_t h = fnv1a64(cut.data.data(), cut.data.size());
            int found = -1;
            for (size_t u = 0; u < unique.size(); u++)
                if (hashes[u] == h && unique[u].data == cut.data){ found = (int)u; break; }
            if (found < 0){
                found = (int)unique.size();
                hashes.push_back(h);
                unique.push_back(std::move(cut));
            }
            frameOf.push_back(found);
        }
        // pages: a grid of cells, as few pages as fit, each trimmed to the cells it holds
        const int cols = std::max(1, (maxPage + GUTTER) / cellW), rows = std::max(1, (maxPage + GUTTER) / cellH);
        const int perPage = cols * rows;
        const int pageCount = ((int)unique.size() + perPage - 1) / perPage;
        ta::CompiledFlipbook fb;
        fb.fps = (float)titleFlipbookFps(*w.animation);
        fb.blend = w.animation->flipbookSettings.frameBlend;
        // the frame in canvas pixels (image space): the render's frame and the cut, at the scale
        const tt::TitleRectI renderFrame = w.report.frame;
        fb.frameX = (int)std::floor(renderFrame.x + bounds.x / scale);
        fb.frameY = (int)std::floor(renderFrame.y + bounds.y / scale);
        fb.frameWidth = std::max(1, (int)std::lround(fw / scale));
        fb.frameHeight = std::max(1, (int)std::lround(fh / scale));
        fb.pixelWidth = fw;
        fb.pixelHeight = fh;
        const std::string animationHash = hex64(tt::animationHash(*doc, *w.authored));
        std::vector<ta::FlipbookFrame> cellOf(unique.size());
        TitleFlipbookBytes bytes;
        for (int p = 0; p < pageCount; p++){
            const int first = p * perPage, count = std::min(perPage, (int)unique.size() - first);
            const int usedCols = std::min(cols, count), usedRows = (count + cols - 1) / cols;
            const int pw = alignUp(usedCols * cellW - GUTTER), ph = alignUp(usedRows * cellH - GUTTER);
            texsynth::Image page = texsynth::Image::make(pw, ph, 4, 8);
            for (int i = 0; i < count; i++){
                const int x = (i % cols) * cellW, y = (i / cols) * cellH;
                const texsynth::Image& cell = unique[(size_t)(first + i)];
                for (int row = 0; row < fh; row++) std::memcpy(page.pixel(x, y + row), cell.pixel(0, row), (size_t)fw * 4);
                cellOf[(size_t)(first + i)] = {p, x, y};
            }
            bleedAlpha(page, 2);
            std::vector<uint8_t> encoded;
            if (!texsynth::encodePng(page, encoded)){
                fail("encode_failed", "encoding a flipbook page failed");
                return false;
            }
            TitleBakeRecipe recipe;
            recipe.sourceGuid = doc->guid();
            recipe.sourcePath = project ? project->normalizeToAssetsRelative(documentPath).generic_string() : docName;
            recipe.sourceHash = sourceHash;
            recipe.bakeVersion = bakeVersion;
            recipe.output = "flipbook:" + w.animation->id;
            recipe.frame = {fb.frameX, fb.frameY, fb.frameWidth, fb.frameHeight};
            recipe.scale = scale;
            recipe.bitDepth = 8;
            recipe.backend = SystemRender::getBackendName();
            recipe.gpu = SystemRender::getDeviceName();
            recipe.editorVersion = JASPERENGINE_EDITOR_VERSION;
            recipe.extra[RECIPE_ANIMATION] = animationHash;
            std::vector<uint8_t> withRecipe;
            std::string error;
            if (!insertTitleBakeRecipe(encoded, recipe, withRecipe, error)){
                fail("encode_failed", "the recipe of a flipbook page: " + error);
                return false;
            }
            const std::string file = ta::titleFlipbookPageFile(docName, w.animation->id, p);
            const fs::path png = documentPath.parent_path() / file;
            const fs::path meta(png.string() + ".meta");
            std::error_code ec;
            std::string metaText;
            if (!fs::exists(meta, ec)){
                metaText = pageMeta();
                staged.push_back({meta, std::vector<uint8_t>(metaText.begin(), metaText.end())});
            }
            staged.push_back({png, std::move(withRecipe)});
            fb.pages.push_back({file, guidOfMeta(meta, metaText), pw, ph});
            bytes.add(pw, ph);
            w.report.files.push_back(project ? project->normalizeToProjectRelative(png).generic_string() : png.generic_string());
        }
        for (int f : frameOf) fb.frames.push_back(cellOf[(size_t)f]);
        w.report.unique = (int)unique.size();
        w.report.pages = pageCount;
        w.report.frameWidth = fw;
        w.report.frameHeight = fh;
        w.report.frame = {fb.frameX, fb.frameY, fb.frameWidth, fb.frameHeight};
        w.report.bytes = bytes;
        if (entry){
            entry->compiled.flipbook = fb;
            entry->compiled.bakedCost.frames = w.report.frames;
            entry->compiled.bakedCost.unique = w.report.unique;
            entry->compiled.bakedCost.pages = pageCount;
            entry->compiled.bakedCost.bytesDesktop = bytes.desktop;
            entry->compiled.bakedCost.bytesMobile = bytes.mobile;
            entry->compiled.bakedCost.bytesAndroidEtc2 = bytes.androidEtc2;
            entry->compiled.bakedCost.bytesWeb = bytes.web;
        }
        outcome.animations.push_back(w.report);
    }
    const std::string animText = ta::writeAnimData(data);
    staged.push_back({fs::path(documentPath.string() + ta::TITLE_ANIM_DATA_SUFFIX), std::vector<uint8_t>(animText.begin(), animText.end())});
    outcome.data = std::move(data);
    outcome.warnings.insert(outcome.warnings.end(), warnings.begin(), warnings.end());
    if (options.dryRun){
        outcome.ok = true;
        finished = true;
        return true;
    }
    {
        std::vector<std::unique_ptr<ScopedExpectedWrite>> expected;
        for (const texsynth::StagedFile& f : staged) expected.push_back(std::make_unique<ScopedExpectedWrite>(f.path));
        std::string failedPath, error;
        if (!texsynth::writeFilesAllOrNothing(staged, failedPath, error)){
            fail("write_failed", "writing " + failedPath + " failed: " + error + " (nothing was written)");
            return false;
        }
        for (size_t i = 0; i < staged.size(); i++) expected[i]->reported(staged[i].path);
    }
    for (const texsynth::StagedFile& f : staged) outcome.written.push_back(f.path);
    if (project){
        ProjectFileWrites::filesWritten(project, outcome.written);
        if (TextureCookQueue::get().getProject() == project){
            for (const fs::path& path : outcome.written)
                if (Util::isImageFile(path.string())) TextureCookQueue::get().sourceChanged(project->normalizeToAssetsRelative(path).generic_string());
        }
        // the pages' GUIDs once the asset database has their metas
        for (ta::TitleAnimDataEntry& e : outcome.data.animations){
            for (ta::FlipbookPage& page : e.compiled.flipbook.pages){
                if (!page.guid.empty()) continue;
                AssetDatabase::AssetInfo info;
                if (project->getAssetDatabase().getInfo(project->normalizeToProjectRelative(documentPath.parent_path() / page.file), info))
                    page.guid = info.guid.str();
            }
        }
        std::error_code relativeError;
        const fs::path relative = fs::relative(documentPath, project->getProjectPath(), relativeError);
        if (!relativeError) TitleArtSystem::notifyTitleChanged(relative.generic_string());
    }
    outcome.ok = true;
    finished = true;
    return true;
}
