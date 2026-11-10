// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleAnimChecks.h"

#include "TitleAnimCost.h"
#include "TitleAnimOps.h"
#include "TitleFlipbookBaker.h"
#include "asset/AssetFileWrite.h"
#include "titleanim/TitleAnimDataIO.h"
#include "titleanim/TitleAnimSampling.h"
#include "titleart/TitleArtBake.h"
#include "titleart/TitleArtChecks.h"
#include "titleart/TitleArtIO.h"
#include "titleart/anim/TitleAnimIO.h"
#include "titleart/anim/TitleAnimPlan.h"
#include "titleart/anim/TitleAnimRealtime.h"
#include "titleart/render/TitleArtRenderer.h"
#include "titleart/text/TitleTextLayout.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace ta = jasperengine::titleanim;
namespace tt = jasperengine::titleart;
namespace fs = std::filesystem;

namespace {

    using Json = tt::Json;

    // the compiled animations a run judges (options.animation restricts them)
    struct Compiled {
        std::shared_ptr<ta::TitleAnimLibrary> library;
        ta::TitleAnimationSet set;
        std::vector<const ta::CompiledTitleAnimation*> judged;
    };

    Compiled compiledOf(TitleCheckContext& ctx){
        Compiled c;
        c.set = tt::readAnimationSet(ctx.doc);
        std::unique_ptr<tt::TitleTextLayout> layout;
        if (ctx.assets) layout = std::make_unique<tt::TitleTextLayout>(ctx.assets);
        c.library = tt::compileAnimations(ctx.doc, layout.get(), nullptr);
        for (const ta::CompiledTitleAnimation& a : c.library->animations)
            if (ctx.options.animation.empty() || a.name == ctx.options.animation) c.judged.push_back(&a);
        return c;
    }

    TitleCheckResult result(const char* id, TitleCheckKind kind, TitleCheckStatus status, const std::string& message){
        TitleCheckResult r;
        r.id = id;
        r.kind = kind;
        r.status = status;
        r.message = message;
        return r;
    }

    void skip(const char* id, TitleCheckKind kind, const std::string& why, std::vector<TitleCheckResult>& out){
        out.push_back(result(id, kind, TitleCheckStatus::Skip, why));
    }

    bool noAnimations(const char* id, TitleCheckKind kind, TitleCheckContext& ctx, const Compiled& c, std::vector<TitleCheckResult>& out){
        if (!c.judged.empty()) return false;
        skip(id, kind, ctx.options.animation.empty() ? "the title has no animations" : "no animation '" + ctx.options.animation + "'", out);
        return true;
    }

    struct Pixels {
        int width = 0, height = 0;
        std::vector<float> lum;         // relative luminance over black, per pixel
        std::vector<float> alpha;
        std::vector<float> redness;     // R / (R + G + B) of the linear colour
        std::vector<uint8_t> rgba;
    };

    float lin(uint8_t c){
        const float v = c / 255.0f;
        return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
    }

    Pixels pixelsOf(const TitleCheckImage& image){
        Pixels p;
        p.width = image.width;
        p.height = image.height;
        p.rgba = image.rgba;
        const size_t n = (size_t)image.width * (size_t)image.height;
        p.lum.resize(n);
        p.alpha.resize(n);
        p.redness.resize(n);
        for (size_t i = 0; i < n; i++){
            const float a = image.rgba[i * 4 + 3] / 255.0f;
            const float r = lin(image.rgba[i * 4]) * a, g = lin(image.rgba[i * 4 + 1]) * a, b = lin(image.rgba[i * 4 + 2]) * a;
            p.lum[i] = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            p.alpha[i] = a;
            p.redness[i] = r + g + b > 1e-4f ? r / (r + g + b) : 0.0f;
        }
        return p;
    }

    // the share of ink pixels (alpha >= 1/255 in either) that differ by more than `codes`, and the
    // largest difference
    void compare(const TitleCheckImage& a, const TitleCheckImage& b, int codes, double& changedPct, int& maxDiff){
        changedPct = 0.0;
        maxDiff = 0;
        if (a.width != b.width || a.height != b.height){
            changedPct = 100.0;
            maxDiff = 255;
            return;
        }
        size_t ink = 0, changed = 0;
        for (size_t i = 0; i < (size_t)a.width * (size_t)a.height; i++){
            if (a.rgba[i * 4 + 3] == 0 && b.rgba[i * 4 + 3] == 0) continue;
            ink++;
            int d = 0;
            for (int k = 0; k < 4; k++) d = std::max(d, std::abs((int)a.rgba[i * 4 + k] - (int)b.rgba[i * 4 + k]));
            maxDiff = std::max(maxDiff, d);
            if (d > codes) changed++;
        }
        changedPct = ink ? 100.0 * (double)changed / (double)ink : 0.0;
    }

    Json fixOps(Json ops){
        return Json{{"ops", std::move(ops)}};
    }

    // ---- anim_loop_seamless ----

    void loopSeamless(TitleCheckContext& ctx, std::vector<TitleCheckResult>& out){
        const char* id = "anim_loop_seamless";
        const Compiled c = compiledOf(ctx);
        if (noAnimations(id, TitleCheckKind::DocumentPixels, ctx, c, out)) return;
        bool any = false;
        for (const ta::CompiledTitleAnimation* a : c.judged){
            if (a->wrap == ta::AnimWrap::Once) continue;
            any = true;
            const double start = a->loopStart, end = a->duration - 1e-4;
            ta::TitleAnimPose p0, p1, pBefore;
            p0.size(*a);
            p1.size(*a);
            pBefore.size(*a);
            ta::sampleAnimation(*a, start, p0);
            ta::sampleAnimation(*a, end, p1);
            ta::sampleAnimation(*a, end - 1.0 / 60.0, pBefore);
            Json seams = Json::array();
            Json ops = Json::array();
            Json slopes = Json::array();
            for (const ta::CompiledTitleTrack& t : a->tracks){
                if (t.mute || t.orphan) continue;
                const size_t width = std::max<size_t>(1, t.width) * t.units;
                const double range = std::max(1e-6, t.max - t.min < 1e17 ? t.max - t.min : 1.0);
                double worst = 0.0;
                for (size_t i = 0; i < width; i++) worst = std::max(worst, std::fabs(p0.values[t.poseOffset + i] - p1.values[t.poseOffset + i]));
                // a ping-pong turns at both ends: values always meet
                if (a->wrap == ta::AnimWrap::PingPong) worst = 0.0;
                if (worst > 1e-4 * std::max(1.0, range)){
                    seams.push_back({{"layer", t.layer}, {"field", t.field}, {"start", p0.values[t.poseOffset]}, {"end", p1.values[t.poseOffset]}});
                    if (t.source == ta::TrackSource::Drive && t.cycles <= 0.0f){
                        ops.push_back({{"op", "set_track"}, {"animation", a->name}, {"layer", titleLayerRef(t.layer)}, {"field", t.field},
                                       {"drive", {{"from", t.driveFrom.size() == 1 ? Json(t.driveFrom[0]) : Json(t.driveFrom)},
                                                  {"to", t.driveTo.size() == 1 ? Json(t.driveTo[0]) : Json(t.driveTo)}, {"cycles", 1}, {"wrap", t.driveWrap}}}});
                    }else if (t.source == ta::TrackSource::Keys && t.unitDelays.empty()){
                        const TitleJson startValue = sampleTitleTrackValue(ctx.doc, *c.set.find(a->name), t.layer, t.field, start);
                        ops.push_back({{"op", "set_key"}, {"animation", a->name}, {"layer", titleLayerRef(t.layer)}, {"field", t.field}, {"time", a->duration},
                                       {"value", startValue}});
                    }
                }
                // a slope jump of over half at the seam warns
                if (a->wrap == ta::AnimWrap::Loop && t.source != ta::TrackSource::Reseed && t.value != ta::TrackValue::Stepped){
                    ta::TitleAnimPose pAfter;
                    pAfter.size(*a);
                    ta::sampleAnimation(*a, start + 1.0 / 60.0, pAfter);
                    const double before = p1.values[t.poseOffset] - pBefore.values[t.poseOffset];
                    const double after = pAfter.values[t.poseOffset] - p0.values[t.poseOffset];
                    const double scale = std::max(std::fabs(before), std::fabs(after));
                    if (scale > 1e-3 * std::max(1.0, range) && std::fabs(before - after) > 0.5 * scale)
                        slopes.push_back({{"layer", t.layer}, {"field", t.field}});
                }
            }
            // the pixels at both times decide whether a value seam shows
            bool pixelsMatch = false;
            Json pixelsJson = nullptr;
            if (!seams.empty() && ctx.renderAnimation){
                TitleCheckImage a0, a1;
                std::string error;
                if (ctx.renderAnimation(a->name, end, "exact", true, a1, error) && ctx.renderAnimation(a->name, start, "exact", true, a0, error)){
                    double changed = 0.0;
                    int maxDiff = 0;
                    compare(a0, a1, 2, changed, maxDiff);
                    pixelsMatch = maxDiff <= 2 || changed <= 0.05;
                    pixelsJson = {{"max_diff", maxDiff}, {"changed_pct", changed}};
                }
            }
            TitleCheckResult r = result(id, TitleCheckKind::DocumentPixels, TitleCheckStatus::Pass, "'" + a->name + "' loops without a seam");
            r.measured = {{"animation", a->name}, {"value_seams", seams}, {"slope_jumps", slopes}, {"pixels", pixelsJson}};
            r.threshold = {{"values", "within 1e-4 of the range"}, {"pixels", "max 2/255 or <= 0.05 % changed"}, {"slope_jump", 0.5}};
            r.method = "values at D - 1e-4 against loop_start (ping_pong turns at both ends); the renders at both times; the slope across the seam";
            if (!seams.empty() && !pixelsMatch){
                r.status = TitleCheckStatus::Fail;
                r.message = "'" + a->name + "' jumps where it loops: " + std::to_string(seams.size()) + " track(s) end away from their loop_start value";
                r.fix = fixOps(ops);
            }else if (!seams.empty()){
                r.status = TitleCheckStatus::Pass;
                r.message = "'" + a->name + "': values differ at the seam but the pixels match (the band is off the frame at both)";
            }else if (!slopes.empty()){
                r.status = TitleCheckStatus::Warn;
                r.message = "'" + a->name + "' changes speed abruptly where it loops (slope jump over 50 %)";
            }
            out.push_back(std::move(r));
        }
        if (!any) skip(id, TitleCheckKind::DocumentPixels, "no looping animation", out);
    }

    // ---- anim_duration ----

    void durationCheck(TitleCheckContext& ctx, std::vector<TitleCheckResult>& out){
        const char* id = "anim_duration";
        const Compiled c = compiledOf(ctx);
        if (noAnimations(id, TitleCheckKind::Document, ctx, c, out)) return;
        for (const ta::CompiledTitleAnimation* a : c.judged){
            double lo = 0.05, hi = 60.0;
            switch (a->role){
                case ta::AnimRole::Intro: lo = 0.2; hi = 3.0; break;
                case ta::AnimRole::Outro: lo = 0.15; hi = 2.0; break;
                case ta::AnimRole::Emphasis: lo = 0.1; hi = 1.5; break;
                case ta::AnimRole::Idle: lo = 0.5; hi = 12.0; break;
                case ta::AnimRole::Win: lo = 0.3; hi = 6.0; break;
                default: break;
            }
            const double measured = a->role == ta::AnimRole::Idle && a->wrap != ta::AnimWrap::Once ? a->duration - a->spanStart() : a->duration;
            TitleCheckResult r = result(id, TitleCheckKind::Document, TitleCheckStatus::Pass, "'" + a->name + "' lasts " + std::to_string(measured) + " s");
            r.measured = {{"animation", a->name}, {"role", ta::animRoleName(a->role)}, {"seconds", measured}};
            r.threshold = {{"min", lo}, {"max", hi}};
            r.method = "the duration (the loop span of an idle loop) against the role's range";
            if (measured <= 0.0 || measured > 60.0) r.status = TitleCheckStatus::Fail;
            else if (measured < lo || measured > hi){
                r.status = TitleCheckStatus::Warn;
                const double target = std::min(hi, std::max(lo, measured));
                r.fix = fixOps(Json::array({Json{{"op", "retime"}, {"animation", a->name}, {"scale", target / std::max(1e-3, measured)}}}));
                r.message = "'" + a->name + "' (" + ta::animRoleName(a->role) + ") lasts " + std::to_string(measured) + " s, outside " + std::to_string(lo) +
                            "-" + std::to_string(hi) + " s";
            }
            out.push_back(std::move(r));
        }
    }

    // ---- anim_end_state ----

    void endState(TitleCheckContext& ctx, std::vector<TitleCheckResult>& out){
        const char* id = "anim_end_state";
        const Compiled c = compiledOf(ctx);
        if (noAnimations(id, TitleCheckKind::Pixels, ctx, c, out)) return;
        if (!ctx.renderAnimation || !ctx.render){
            skip(id, TitleCheckKind::Pixels, "no renderer (check --titles runs without a GPU)", out);
            return;
        }
        bool any = false;
        TitleCheckImage still;
        std::string error;
        bool stillOk = false;
        for (const ta::CompiledTitleAnimation* a : c.judged){
            if (a->wrap != ta::AnimWrap::Once || a->overlay) continue;
            const bool outro = a->role == ta::AnimRole::Outro;
            const bool endsOnStill = a->role == ta::AnimRole::Intro || a->role == ta::AnimRole::Emphasis || a->role == ta::AnimRole::Win;
            if (!outro && !endsOnStill) continue;
            any = true;
            TitleCheckImage end;
            if (!ctx.renderAnimation(a->name, a->duration, "exact", true, end, error)){
                skip(id, TitleCheckKind::Pixels, "'" + a->name + "' did not render: " + error, out);
                continue;
            }
            TitleCheckResult r = result(id, TitleCheckKind::Pixels, TitleCheckStatus::Pass, "");
            r.method = "the render at D against the still (intro, emphasis, win) or its coverage (outro)";
            if (outro){
                size_t covered = 0;
                for (size_t i = 0; i < (size_t)end.width * (size_t)end.height; i++) covered += end.rgba[i * 4 + 3] > 0 ? 1 : 0;
                const double pct = end.width > 0 && end.height > 0 ? 100.0 * (double)covered / ((double)end.width * end.height) : 0.0;
                r.measured = {{"animation", a->name}, {"coverage_pct", pct}};
                r.threshold = {{"coverage_pct", 0.1}};
                if (pct > 0.1){
                    r.status = TitleCheckStatus::Warn;
                    r.message = "'" + a->name + "' (outro) ends visible (" + std::to_string(pct) + " % covered)";
                    r.fix = fixOps(Json::array({Json{{"op", "set_key"}, {"animation", a->name}, {"field", "opacity"}, {"time", a->duration}, {"value", 0}}}));
                }else{
                    r.message = "'" + a->name + "' ends hidden";
                }
            }else{
                if (!stillOk){
                    stillOk = ctx.renderAnimation(a->name, -1.0, "still", false, still, error);
                    if (!stillOk){
                        skip(id, TitleCheckKind::Pixels, "the still did not render: " + error, out);
                        return;
                    }
                }
                double changed = 0.0;
                int maxDiff = 0;
                compare(still, end, 8, changed, maxDiff);
                r.measured = {{"animation", a->name}, {"changed_pct", changed}, {"max_diff", maxDiff}};
                r.threshold = {{"changed_pct", 0.5}, {"max_diff", 8}};
                if (changed > 0.5){
                    r.status = TitleCheckStatus::Warn;
                    r.message = "'" + a->name + "' does not end on the still (" + std::to_string(changed) + " % of the ink differs)";
                    Json ops = Json::array();
                    const ta::TitleAnimation* authored = c.set.find(a->name);
                    for (const ta::CompiledTitleTrack& t : a->tracks){
                        if (t.mute || t.orphan || t.source != ta::TrackSource::Keys || !authored) continue;
                        const tt::TrackFieldInfo info = tt::resolveTrackField(ctx.doc, t.layer, t.field);
                        if (!info.ok) continue;
                        // the rest: identity for relative and virtual tracks, the document's value else
                        Json rest = info.rest;
                        if (t.combine != ta::TrackCombine::Replace)
                            rest = t.combine == ta::TrackCombine::Multiply ? Json(info.width == 2 ? Json::array({1, 1}) : Json(1)) : Json(info.width == 2 ? Json::array({0, 0}) : Json(0));
                        ops.push_back({{"op", "set_key"}, {"animation", a->name}, {"layer", titleLayerRef(t.layer)}, {"field", t.field}, {"time", a->duration}, {"value", rest}});
                    }
                    r.fix = fixOps(ops);
                }else{
                    r.message = "'" + a->name + "' ends on the still";
                }
            }
            out.push_back(std::move(r));
        }
        if (!any) skip(id, TitleCheckKind::Pixels, "no once intro, emphasis, win or outro", out);
    }

    // a frame series of an animation: exact renders at `fps` (capped), or one render and the
    // companion's values at `fps` when every track is title live (cheap and exact for those)
    bool titleLiveOnly(const ta::CompiledTitleAnimation& a){
        for (const ta::CompiledTitleTrack& t : a.tracks) if (!t.mute && !t.orphan && t.cls != ta::TrackClass::TitleLive) return false;
        return true;
    }

    // ---- anim_bounds ----

    void boundsCheck(TitleCheckContext& ctx, std::vector<TitleCheckResult>& out){
        const char* id = "anim_bounds";
        const Compiled c = compiledOf(ctx);
        if (noAnimations(id, TitleCheckKind::Pixels, ctx, c, out)) return;
        if (!ctx.renderAnimation){
            skip(id, TitleCheckKind::Pixels, "no renderer (check --titles runs without a GPU)", out);
            return;
        }
        const int w = ctx.doc.canvasWidth(), h = ctx.doc.canvasHeight();
        for (const ta::CompiledTitleAnimation* a : c.judged){
            // 60 samples a second of title-live motion; renders at 15 a second for the rest (capped)
            const double fps = titleLiveOnly(*a) ? 60.0 : 15.0;
            const int samples = std::min(240, std::max(2, (int)std::ceil(a->duration * fps) + 1));
            Json hits = Json::array();
            std::vector<tt::TitleRectI> at;
            for (int k = 0; k < samples; k++){
                const double t = std::min(a->duration, k / fps);
                TitleCheckImage image;
                std::string error;
                if (!ctx.renderAnimation(a->name, t, "exact", true, image, error)) break;
                // the image covers the canvas at its scale: ink at its edges went outside
                const double sx = (double)image.width / std::max(1, w), sy = (double)image.height / std::max(1, h);
                int minX = image.width, minY = image.height, maxX = -1, maxY = -1;
                for (int y = 0; y < image.height; y++)
                    for (int x = 0; x < image.width; x++)
                        if (image.rgba[((size_t)y * image.width + x) * 4 + 3] > 0){
                            minX = std::min(minX, x); maxX = std::max(maxX, x);
                            minY = std::min(minY, y); maxY = std::max(maxY, y);
                        }
                if (maxX < 0) continue;
                Json sides = Json::array();
                if (minX <= 0) sides.push_back("left");
                if (minY <= 0) sides.push_back("top");
                if (maxX >= image.width - 1) sides.push_back("right");
                if (maxY >= image.height - 1) sides.push_back("bottom");
                if (!sides.empty()){
                    hits.push_back({{"time", t}, {"sides", sides}});
                    at.push_back({(int)(minX / sx), (int)(minY / sy), (int)((maxX - minX + 1) / sx), (int)((maxY - minY + 1) / sy)});
                    if (hits.size() >= 8) break;
                }
            }
            TitleCheckResult r = result(id, TitleCheckKind::Pixels, hits.empty() ? TitleCheckStatus::Pass : TitleCheckStatus::Warn,
                                        hits.empty() ? "'" + a->name + "' stays inside the canvas" : "'" + a->name + "' leaves the canvas");
            r.measured = {{"animation", a->name}, {"outside", hits}};
            r.threshold = "no ink at the canvas edges";
            r.method = std::string("exact renders at ") + (fps == 60.0 ? "60" : "15") + " samples a second";
            r.at = at;
            out.push_back(std::move(r));
        }
    }

    // ---- anim_static ----

    void staticCheck(TitleCheckContext& ctx, std::vector<TitleCheckResult>& out){
        const char* id = "anim_static";
        const Compiled c = compiledOf(ctx);
        if (noAnimations(id, TitleCheckKind::Pixels, ctx, c, out)) return;
        if (!ctx.renderAnimation){
            skip(id, TitleCheckKind::Pixels, "no renderer (check --titles runs without a GPU)", out);
            return;
        }
        for (const ta::CompiledTitleAnimation* a : c.judged){
            TitleCheckImage first;
            std::string error;
            double most = 0.0;
            bool ok = true;
            for (int k = 0; k < 12 && ok; k++){
                const double t = a->duration * k / 11.0;
                TitleCheckImage image;
                if (!ctx.renderAnimation(a->name, t, "exact", true, image, error)){
                    ok = false;
                    break;
                }
                if (k == 0){
                    first = std::move(image);
                    continue;
                }
                double changed = 0.0;
                int maxDiff = 0;
                compare(first, image, 0, changed, maxDiff);
                most = std::max(most, changed);
            }
            if (!ok){
                skip(id, TitleCheckKind::Pixels, "'" + a->name + "' did not render: " + error, out);
                continue;
            }
            TitleCheckResult r = result(id, TitleCheckKind::Pixels, most > 0.01 ? TitleCheckStatus::Pass : TitleCheckStatus::Fail,
                                        most > 0.01 ? "'" + a->name + "' moves" : "'" + a->name + "' does nothing: 12 renders over its duration are the same");
            r.measured = {{"animation", a->name}, {"changed_pct_max", most}};
            r.threshold = {{"changed_pct", 0.01}};
            r.method = "12 exact renders over the duration against the first";
            out.push_back(std::move(r));
        }
    }

    // ---- anim_flash (WCAG 2.3.1) ----

    void flashCheck(TitleCheckContext& ctx, std::vector<TitleCheckResult>& out){
        const char* id = "anim_flash";
        const Compiled c = compiledOf(ctx);
        if (noAnimations(id, TitleCheckKind::Pixels, ctx, c, out)) return;
        if (!ctx.renderAnimation){
            skip(id, TitleCheckKind::Pixels, "no renderer (check --titles runs without a GPU)", out);
            return;
        }
        for (const ta::CompiledTitleAnimation* a : c.judged){
            const double fps = 60.0;
            // a looping animation is judged over two passes (its seam flashes too)
            const double length = a->wrap == ta::AnimWrap::Once ? a->duration : a->duration + (a->duration - a->spanStart());
            const int samples = std::min(600, std::max(2, (int)std::ceil(length * fps) + 1));
            const bool analytic = titleLiveOnly(*a);
            std::vector<Pixels> frames;
            std::string error;
            bool ok = true;
            if (analytic){
                // one render without the companion's values, then each sample's opacity and flash
                TitleCheckImage base;
                if (!ctx.renderAnimation(a->name, 0.0, "exact", false, base, error)) ok = false;
                ta::TitleAnimPlayer player;
                player.setLibrary(c.library);
                for (int k = 0; ok && k < samples; k++){
                    player.sample(a->name, k / fps);
                    ta::TitlePoseSummary s;
                    player.summary(nullptr, s);
                    TitleCheckImage image = base;
                    for (size_t i = 0; i < (size_t)image.width * (size_t)image.height; i++){
                        uint8_t* p = image.rgba.data() + i * 4;
                        for (int ch = 0; ch < 3; ch++){
                            const float l = std::max(0.0f, lin(p[ch]) + (float)(s.flashAmount * std::pow(s.flashColor[ch], 2.2)));
                            const float srgb = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
                            p[ch] = (uint8_t)std::lround(std::min(1.0f, srgb) * 255.0f);
                        }
                        p[3] = (uint8_t)std::lround(p[3] * std::min(1.0, std::max(0.0, s.opacity)));
                    }
                    frames.push_back(pixelsOf(image));
                }
            }else{
                // renders at 30 a second (each held for two samples)
                for (int k = 0; ok && k < samples; k += 2){
                    TitleCheckImage image;
                    if (!ctx.renderAnimation(a->name, k / fps, "exact", true, image, error)){
                        ok = false;
                        break;
                    }
                    Pixels p = pixelsOf(image);
                    frames.push_back(p);
                    frames.push_back(std::move(p));
                }
            }
            if (!ok || frames.empty()){
                skip(id, TitleCheckKind::Pixels, "'" + a->name + "' did not render: " + error, out);
                continue;
            }
            // the ink: pixels covered in any frame
            const size_t n = (size_t)frames[0].width * (size_t)frames[0].height;
            std::vector<size_t> ink;
            for (size_t i = 0; i < n; i++){
                bool covered = false;
                for (const Pixels& f : frames) if (i < f.alpha.size() && f.alpha[i] > 0.0f){ covered = true; break; }
                if (covered) ink.push_back(i);
            }
            const double area = std::min(0.25 * (double)ink.size(), 21824.0);
            // the mean luminance of the ink per sample, its extremes, and the transitions between them
            std::vector<double> mean(frames.size(), 0.0), red(frames.size(), 0.0);
            for (size_t k = 0; k < frames.size(); k++){
                double sum = 0.0, rsum = 0.0;
                for (size_t i : ink){
                    sum += frames[k].lum[i];
                    rsum += frames[k].redness[i];
                }
                mean[k] = ink.empty() ? 0.0 : sum / (double)ink.size();
                red[k] = ink.empty() ? 0.0 : rsum / (double)ink.size();
            }
            struct Transition { double time; bool red; };
            std::vector<Transition> transitions;
            size_t anchor = 0;
            for (size_t k = 1; k < mean.size(); k++){
                const double d = mean[k] - mean[anchor];
                const double darker = std::min(mean[k], mean[anchor]);
                if (std::fabs(d) >= 0.1 && darker < 0.8){
                    // the changed area: ink pixels whose luminance moved by 0.1 or more
                    size_t moved = 0;
                    for (size_t i : ink) if (std::fabs(frames[k].lum[i] - frames[anchor].lum[i]) >= 0.1) moved++;
                    if ((double)moved >= area){
                        const bool isRed = std::max(red[k], red[anchor]) >= 0.8;
                        if (transitions.empty() || transitions.back().time != k / fps) transitions.push_back({k / fps, isRed});
                    }
                    anchor = k;
                }
            }
            // flashes (pairs of opposite transitions) in any one-second window
            int worst = 0, worstRed = 0;
            double worstAt = 0.0;
            for (size_t i = 0; i < transitions.size(); i++){
                int count = 0, reds = 0;
                for (size_t j = i; j < transitions.size() && transitions[j].time - transitions[i].time < 1.0; j++){
                    count++;
                    reds += transitions[j].red ? 1 : 0;
                }
                if (count / 2 > worst){
                    worst = count / 2;
                    worstAt = transitions[i].time;
                }
                worstRed = std::max(worstRed, reds / 2);
            }
            const bool fail = worst > 3 || worstRed > 3;
            TitleCheckResult r = result(id, TitleCheckKind::Pixels, fail ? TitleCheckStatus::Fail : TitleCheckStatus::Pass,
                                        fail ? "'" + a->name + "' flashes " + std::to_string(std::max(worst, worstRed)) +
                                                   " times in one second (more than 3). This is a measurement, not a certification."
                                             : "'" + a->name + "' flashes at most 3 times a second. This is a measurement, not a certification.");
            r.measured = {{"animation", a->name}, {"flashes_per_second", worst}, {"red_flashes_per_second", worstRed}, {"at", worstAt}, {"transitions", transitions.size()}};
            r.threshold = {{"flashes_per_second", 3}, {"change", 0.1}, {"darker_below", 0.8}, {"area_px", area}};
            r.method = std::string("WCAG 2.3.1 general and red flash on the ink's mean relative luminance at 60 samples a second") +
                       (analytic ? " (one render, the companion's opacity and flash per sample)" : " (renders at 30 a second)");
            if (fail){
                const double scale = std::max(worst, worstRed) / 3.0 + 0.05;
                r.fix = fixOps(Json::array({Json{{"op", "retime"}, {"animation", a->name}, {"scale", scale}}}));
            }
            out.push_back(std::move(r));
        }
    }

    // ---- estimates ----

    TitleAnimCostInputs costInputs(TitleCheckContext& ctx, ta::TitleAnimData& baked, bool& hasBaked){
        TitleAnimCostInputs inputs;
        hasBaked = false;
        if (!ctx.file.empty()){
            std::string text, error;
            TitleBakeInfo info;
            if (assetio::readFile(titleArtSidecarPath(ctx.file), text) && TitleBakeInfo::parse(text, info, error)){
                inputs.still.frame = {info.frame.x, info.frame.y, info.frame.width, info.frame.height};
                for (const TitleBakeInfoOutput& o : info.outputs)
                    if (o.id == "main" && info.frame.width > 0) inputs.outputScale = (float)o.width / (float)info.frame.width;
            }
            std::vector<std::string> warnings;
            if (ta::loadAnimData(ctx.file.string() + ta::TITLE_ANIM_DATA_SUFFIX, baked, &warnings, &error)) hasBaked = true;
        }
        if (hasBaked) inputs.baked = &baked;
        return inputs;
    }

    std::vector<TitleArtRenderMode> modesOf(TitleCheckContext& ctx){
        if (ctx.renderMode) return {(TitleArtRenderMode)*ctx.renderMode};
        return {TitleArtRenderMode::BAKED, TitleArtRenderMode::REALTIME, TitleArtRenderMode::GLYPH_ATLAS};
    }

    void frameBudget(TitleCheckContext& ctx, std::vector<TitleCheckResult>& out){
        const char* id = "anim_frame_budget";
        const Compiled c = compiledOf(ctx);
        if (noAnimations(id, TitleCheckKind::Estimate, ctx, c, out)) return;
        ta::TitleAnimData baked;
        bool hasBaked = false;
        TitleAnimCostInputs inputs = costInputs(ctx, baked, hasBaked);
        const tt::TitleRectI region = inputs.still.frame.empty() ? tt::TitleRectI() : tt::TitleRectI{inputs.still.frame.x, inputs.still.frame.y, inputs.still.frame.width, inputs.still.frame.height};
        inputs.renderUs = ctx.render ? titleRenderEstimateUs(ctx.assets, ctx.doc, inputs.outputScale, region) : 0.0;
        const tt::TitleAnimPlan realtime = tt::buildAnimPlan(ctx.doc, c.library->animations, TitleArtRenderMode::REALTIME);
        const tt::TitleAnimPlan atlas = tt::buildAnimPlan(ctx.doc, c.library->animations, TitleArtRenderMode::GLYPH_ATLAS);
        bool judgedAny = false;
        for (const ta::CompiledTitleAnimation* a : c.judged){
            for (TitleArtRenderMode mode : modesOf(ctx)){
                if (mode == TitleArtRenderMode::BAKED) continue;
                const tt::AnimationPlan* ap = (mode == TitleArtRenderMode::REALTIME ? realtime : atlas).find(a->name);
                if (!ap) continue;
                judgedAny = true;
                const TitleAnimCost cost = estimateAnimCost(ctx.doc, *a, *ap, inputs);
                TitleCheckResult r = result(id, TitleCheckKind::Estimate, TitleCheckStatus::Pass, "'" + a->name + "' " + cost.summary());
                r.measured = cost.toJson();
                r.threshold = {{"warn_below_fps", a->rerenderFps}, {"fail_below_fps", 10}};
                r.method = "S81 §6.3's model: the planned render's estimate x the tier's factor, against R18's share of the tier's frame budget";
                if (mode == TitleArtRenderMode::REALTIME && (cost.rerender || cost.prerender) && inputs.renderUs > 0.0){
                    double lowest = 1e9;
                    for (double f : cost.reachedFps) lowest = std::min(lowest, f);
                    if (lowest < 10.0) r.status = TitleCheckStatus::Fail;
                    else if (lowest < a->rerenderFps - 1e-6) r.status = TitleCheckStatus::Warn;
                    if (r.status != TitleCheckStatus::Pass){
                        const double fps = std::max(1.0, std::floor(lowest));
                        r.fix = fixOps(Json::array({Json{{"op", "set_animation"}, {"animation", a->name}, {"values", {{"rerender", "prerender"}}}},
                                                    Json{{"op", "set_animation"}, {"animation", a->name}, {"values", {{"rerender_fps", fps}}}}}));
                        r.message = "'" + a->name + "' re-renders at " + std::to_string((int)lowest) + " fps on the slowest tier (asked " +
                                    std::to_string((int)a->rerenderFps) + "): prerender it, lower rerender_fps (fix ops: either one), or animate a live field instead";
                    }
                }
                out.push_back(std::move(r));
            }
        }
        if (!judgedAny) skip(id, TitleCheckKind::Estimate, "only Baked is judged (the component's Render Mode)", out);
    }

    void flipbookMemory(TitleCheckContext& ctx, std::vector<TitleCheckResult>& out){
        const char* id = "anim_flipbook_memory";
        const Compiled c = compiledOf(ctx);
        if (noAnimations(id, TitleCheckKind::Estimate, ctx, c, out)) return;
        const std::vector<TitleArtRenderMode> modes = modesOf(ctx);
        if (std::find(modes.begin(), modes.end(), TitleArtRenderMode::BAKED) == modes.end()){
            skip(id, TitleCheckKind::Estimate, "the title is not Baked", out);
            return;
        }
        ta::TitleAnimData baked;
        bool hasBaked = false;
        const TitleAnimCostInputs inputs = costInputs(ctx, baked, hasBaked);
        const tt::TitleAnimPlan plan = tt::buildAnimPlan(ctx.doc, c.library->animations, TitleArtRenderMode::BAKED);
        // the document's pages together, per family
        TitleFlipbookBytes total;
        Json perAnimation = Json::array();
        std::vector<std::pair<const ta::CompiledTitleAnimation*, TitleAnimCost>> costs;
        for (const ta::CompiledTitleAnimation& a : c.library->animations){
            const tt::AnimationPlan* ap = plan.find(a.name);
            if (!ap || !ap->flipbook) continue;
            const TitleAnimCost cost = estimateAnimCost(ctx.doc, a, *ap, inputs);
            total.desktop += cost.bytes.desktop;
            total.mobile += cost.bytes.mobile;
            total.androidEtc2 += cost.bytes.androidEtc2;
            total.web += cost.bytes.web;
            perAnimation.push_back(cost.toJson());
            costs.push_back({&a, cost});
        }
        TitleCheckResult r = result(id, TitleCheckKind::Estimate, TitleCheckStatus::Pass, "");
        Json measured = Json::object();
        Json threshold = Json::object();
        std::string over;
        double worstRatio = 0.0;
        for (const char* f : TITLE_FLIPBOOK_FAMILIES){
            const double mibs = (double)total.of(f) / (1024.0 * 1024.0);
            measured[f] = std::round(mibs * 100.0) / 100.0;
            threshold[f] = inputs.budget.of(f);
            if (mibs > inputs.budget.of(f)){
                over += (over.empty() ? "" : ", ") + std::string(f);
                worstRatio = std::max(worstRatio, mibs / inputs.budget.of(f));
            }
        }
        r.measured = {{"mib", measured}, {"animations", perAnimation}, {"baked", hasBaked}};
        r.threshold = {{"mib", threshold}};
        r.method = "R38: every flipbook page's cooked bytes (BC7 1 B/px, ASTC 6x6 0.445, ETC2 1, web 1; no mips), the baked ones when fresh, else "
                   "the upper bound (every frame unique)";
        if (over.empty()){
            r.message = costs.empty() ? "no flipbooks: every Baked animation is title live" : "flipbooks fit the budgets";
        }else{
            r.status = TitleCheckStatus::Fail;
            // a scale and an fps that bring the worst family under its budget (bytes go with scale^2 x fps)
            const double cut = 1.0 / worstRatio;
            Json ops = Json::array();
            for (const auto& [a, cost] : costs){
                const double scale = std::max(0.25, std::min(1.0, (double)a->flipbookSettings.scale * std::sqrt(cut) * 0.97));
                const double fps = std::max(1.0, std::floor(titleFlipbookFps(*a) * std::min(1.0, cut / ((scale / a->flipbookSettings.scale) *
                                                                                                         (scale / a->flipbookSettings.scale))) * 0.97));
                ops.push_back({{"op", "set_animation"}, {"animation", a->name}, {"values", {{"flipbook.scale", std::round(scale * 100.0) / 100.0}, {"flipbook.fps", fps}}}});
            }
            r.fix = fixOps(ops);
            r.message = "the flipbooks are over budget on " + over + ": lower flipbook.fps and flipbook.scale (fix ops), or use Render Mode Realtime or "
                        "Glyph Atlas (anim_frame_budget shows their cost)";
        }
        out.push_back(std::move(r));
    }

    // ---- anim_mode_support ----

    void modeSupport(TitleCheckContext& ctx, std::vector<TitleCheckResult>& out){
        const char* id = "anim_mode_support";
        const Compiled c = compiledOf(ctx);
        if (noAnimations(id, TitleCheckKind::Document, ctx, c, out)) return;
        for (TitleArtRenderMode mode : modesOf(ctx)){
            const tt::TitleAnimPlan plan = tt::buildAnimPlan(ctx.doc, c.library->animations, mode);
            for (const ta::CompiledTitleAnimation* a : c.judged){
                const tt::AnimationPlan* ap = plan.find(a->name);
                if (!ap) continue;
                Json notPlayed = Json::array(), fallbacks = Json::array();
                Json ops = Json::array();
                for (const tt::TrackPlan& t : ap->tracks){
                    if (t.code == "muted" || t.code == "anim_orphan") continue;
                    const Json row = {{"layer", t.layer}, {"field", t.field}, {"how", t.how}, {"code", t.code}, {"reason", t.reason}};
                    if (t.play == tt::TrackPlay::NotPlayed){
                        notPlayed.push_back(row);
                        ops.push_back({{"op", "remove_track"}, {"animation", a->name}, {"layer", titleLayerRef(t.layer)}, {"field", t.field}});
                    }else if (t.play == tt::TrackPlay::FallsBack){
                        fallbacks.push_back(row);
                    }
                }
                TitleCheckResult r = result(id, TitleCheckKind::Document, TitleCheckStatus::Pass,
                                            "'" + a->name + "' plays every track in " + tt::renderModeName(mode));
                r.measured = {{"animation", a->name}, {"mode", tt::renderModeName(mode)}, {"not_played", notPlayed}, {"fallbacks", fallbacks}, {"notes", ap->notes}};
                r.threshold = "every track plays";
                r.method = "the plan of the mode (R12): what plays, falls back (with why) or is not played";
                if (!notPlayed.empty()){
                    r.status = TitleCheckStatus::Fail;
                    r.message = "'" + a->name + "': " + std::to_string(notPlayed.size()) + " track(s) are not played in " + tt::renderModeName(mode) +
                                ": remove them (fix ops) or switch the Render Mode (Realtime plays every class)";
                    r.fix = fixOps(ops);
                }else if (!fallbacks.empty()){
                    r.status = TitleCheckStatus::Warn;
                    r.message = "'" + a->name + "': " + std::to_string(fallbacks.size()) + " track(s) fall back in " + tt::renderModeName(mode);
                }
                out.push_back(std::move(r));
            }
        }
    }

    // ---- anim_markers ----

    void markersCheck(TitleCheckContext& ctx, std::vector<TitleCheckResult>& out){
        const char* id = "anim_markers";
        const Compiled c = compiledOf(ctx);
        if (noAnimations(id, TitleCheckKind::Document, ctx, c, out)) return;
        for (const ta::CompiledTitleAnimation* a : c.judged){
            Json problems = Json::array();
            Json ops = Json::array();
            for (size_t i = 0; i < a->markers.size(); i++){
                const ta::CompiledMarker& m = a->markers[i];
                if (!a->autoDuration && m.time > a->duration + 1e-6){
                    problems.push_back({{"marker", m.name}, {"problem", "marker_after_end"}, {"time", m.time}});
                    ops.push_back({{"op", "set_marker"}, {"animation", a->name}, {"name", m.name}, {"time", a->duration}});
                }
                if (i > 0 && a->markers[i - 1].name == m.name && std::fabs(a->markers[i - 1].time - m.time) < 1e-4)
                    problems.push_back({{"marker", m.name}, {"problem", "duplicate_at_time"}, {"time", m.time}});
                if (!m.sound.empty()){
                    const bool found = ctx.assetExists && ctx.assetExists(m.sound.path, m.sound.guid.str());
                    const std::string& p = m.sound.path;
                    const std::string ext = p.find_last_of('.') == std::string::npos ? std::string() : p.substr(p.find_last_of('.'));
                    const bool audio = ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac" || ext == ".soundevent";
                    if (!audio) problems.push_back({{"marker", m.name}, {"problem", "sound_not_audio"}, {"sound", p}});
                    else if (ctx.assetExists && !found) problems.push_back({{"marker", m.name}, {"problem", "sound_missing"}, {"sound", p}});
                }
            }
            TitleCheckResult r = result(id, TitleCheckKind::Document, problems.empty() ? TitleCheckStatus::Pass : TitleCheckStatus::Warn,
                                        problems.empty() ? "'" + a->name + "': markers fire as written" : "'" + a->name + "': " + std::to_string(problems.size()) +
                                                                                                         " marker problem(s)");
            r.measured = {{"animation", a->name}, {"markers", a->markers.size()}, {"problems", problems}};
            r.threshold = "Lua identifiers, times within the duration, sounds that resolve, no duplicates at one time";
            r.method = "the animation's markers";
            if (!ops.empty()) r.fix = fixOps(ops);
            out.push_back(std::move(r));
        }
    }

    // ---- anim_glyph_merge ----

    void glyphMerge(TitleCheckContext& ctx, std::vector<TitleCheckResult>& out){
        const char* id = "anim_glyph_merge";
        const Compiled c = compiledOf(ctx);
        if (noAnimations(id, TitleCheckKind::Pixels, ctx, c, out)) return;
        bool any = false;
        for (const ta::CompiledTitleAnimation* a : c.judged){
            const std::vector<std::string> layers = tt::cellLayersOf(ctx.doc, *a);
            if (layers.empty()) continue;
            any = true;
            if (!ctx.renderAnimation){
                skip(id, TitleCheckKind::Pixels, "no renderer (check --titles runs without a GPU)", out);
                return;
            }
            // the cell compose at rest ("cells") against the merged still
            TitleCheckImage cells, merged;
            std::string error;
            if (!ctx.renderAnimation(a->name, -1.0, "cells_at_rest", false, cells, error) || !ctx.renderAnimation(a->name, -1.0, "still", false, merged, error)){
                skip(id, TitleCheckKind::Pixels, "'" + a->name + "' did not render: " + error, out);
                continue;
            }
            double changed = 0.0;
            int maxDiff = 0;
            compare(merged, cells, 8, changed, maxDiff);
            TitleCheckResult r = result(id, TitleCheckKind::Pixels, changed <= 1.0 ? TitleCheckStatus::Pass : TitleCheckStatus::Warn,
                                        changed <= 1.0 ? "'" + a->name + "': the glyph cells match the merged look"
                                                       : "'" + a->name + "': the glyph cells differ from the merged look on " + std::to_string(changed) + " % of the ink");
            r.measured = {{"animation", a->name}, {"layers", layers}, {"changed_pct", changed}, {"max_diff", maxDiff}};
            r.threshold = {{"changed_pct", 1.0}};
            r.method = "the cell compose at rest against the merged still (8/255)";
            if (changed > 1.0){
                Json ops = Json::array();
                for (const std::string& l : layers) ops.push_back({{"op", "set"}, {"layer", "id:" + l}, {"values", {{"glyph_mode", "merged"}}}});
                r.fix = fixOps(ops);
            }
            out.push_back(std::move(r));
        }
        if (!any) skip(id, TitleCheckKind::Pixels, "no glyph tracks on a cells text layer", out);
    }

    // ---- anim_orphans ----

    void orphans(TitleCheckContext& ctx, std::vector<TitleCheckResult>& out){
        const char* id = "anim_orphans";
        const Compiled c = compiledOf(ctx);
        if (noAnimations(id, TitleCheckKind::Document, ctx, c, out)) return;
        Json found = Json::array();
        Json ops = Json::array();
        for (const ta::TitleAnimation& a : c.set.animations){
            if (!ctx.options.animation.empty() && a.name != ctx.options.animation) continue;
            for (const ta::TitleTrack& t : a.tracks){
                if (!t.orphan) continue;
                found.push_back({{"animation", a.name}, {"layer", t.layer}, {"field", t.field}});
                ops.push_back({{"op", "remove_track"}, {"animation", a.name}, {"layer", titleLayerRef(t.layer)}, {"field", t.field}});
            }
        }
        TitleCheckResult r = result(id, TitleCheckKind::Document, found.empty() ? TitleCheckStatus::Pass : TitleCheckStatus::Warn,
                                    found.empty() ? "every track has its layer and field" : std::to_string(found.size()) + " track(s) lost their layer or field");
        r.measured = {{"orphans", found}};
        r.threshold = "none";
        r.method = "each track's layer and field against the document";
        if (!ops.empty()) r.fix = fixOps(ops);
        out.push_back(std::move(r));
    }

}

void editor::registerTitleAnimChecks(){
    static bool done = false;
    if (done) return;
    done = true;
    struct Row {
        const char* id;
        TitleCheckKind kind;
        const char* description;
        void (*run)(TitleCheckContext&, std::vector<TitleCheckResult>&);
    };
    const Row rows[] = {
        {"anim_loop_seamless", TitleCheckKind::DocumentPixels, "loops whose end differs from their loop start (values and pixels)", loopSeamless},
        {"anim_duration", TitleCheckKind::Document, "durations outside their role's range", durationCheck},
        {"anim_end_state", TitleCheckKind::Pixels, "intros, emphases and wins that do not end on the still; outros that end visible", endState},
        {"anim_bounds", TitleCheckKind::Pixels, "ink that leaves the canvas while it plays", boundsCheck},
        {"anim_static", TitleCheckKind::Pixels, "animations that change nothing", staticCheck},
        {"anim_flash", TitleCheckKind::Pixels, "more than 3 general or red flashes in a second (WCAG 2.3.1; not a certification)", flashCheck},
        {"anim_frame_budget", TitleCheckKind::Estimate, "Realtime re-render rates and Glyph Atlas compose against the frame budget", frameBudget},
        {"anim_flipbook_memory", TitleCheckKind::Estimate, "Baked flipbook bytes per cook family against the budgets", flipbookMemory},
        {"anim_mode_support", TitleCheckKind::Document, "tracks the Render Mode does not play, and fallbacks", modeSupport},
        {"anim_markers", TitleCheckKind::Document, "marker names, times and sounds", markersCheck},
        {"anim_glyph_merge", TitleCheckKind::Pixels, "glyph cells at rest against the merged look", glyphMerge},
        {"anim_orphans", TitleCheckKind::Document, "tracks whose layer or field is gone", orphans},
    };
    for (const Row& r : rows){
        CheckDef def;
        def.id = r.id;
        def.kind = r.kind;
        def.owner = "S85";
        def.group = "anim";
        def.description = r.description;
        def.run = r.run;
        TitleArtChecks::registerCheck(std::move(def));
    }
}
