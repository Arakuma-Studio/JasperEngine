// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S85's parts of the title_art tools (docs/specs/S85-title-animation.md R50-R55, §6.7): the
// animation outline and detail of get_title, the animation_changes of edit_title, render_title's
// options.animation (a frame or a filmstrip with each cell's time and motion figures),
// check_title's animation renders, export_title's flipbooks, and list_title_library's
// animation_presets. S85 adds no tool: these are parameters and options of S80's six.

#include "TitleArtToolsAnimation.h"

#include "Project.h"
#include "titleanim/TitleAnimDataIO.h"
#include "titleanim/TitleAnimSampling.h"
#include "mcp/McpContext.h"
#include "titleart/TitleArtBake.h"
#include "titleart/TitleArtSchema.h"
#include "titleart/TitleArtDocument.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtOps.h"
#include "titleart/anim/TitleAnimCost.h"
#include "titleart/anim/TitleAnimIO.h"
#include "titleart/anim/TitleAnimOps.h"
#include "titleart/anim/TitleAnimPlan.h"
#include "titleart/anim/TitleAnimPresets.h"
#include "titleart/anim/TitleAnimRender.h"
#include "titleart/anim/TitleFlipbookBaker.h"
#include "mcp/McpRefs.h"
#include "util/AssetDatabase.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::mcp;
using namespace jasperengine::editor::mcp::titletools;
namespace ta = jasperengine::titleanim;
namespace tt = jasperengine::titleart;
namespace fs = std::filesystem;

namespace {

    TitleArtBakeInfo stillOf(const fs::path& file){
        TitleArtBakeInfo info;
        std::string error;
        TitleArtBakeInfo::load(titleArtSidecarPath(file).string(), info, error);
        return info;
    }

    float outputScaleOf(const TitleArtBakeInfo& still){
        const TitleArtBakeOutput* main = still.output("main");
        if (!main) main = still.firstOutput("all");
        return main && still.frame.width > 0 ? (float)main->width / (float)still.frame.width : 1.0f;
    }

    Json classesOf(const ta::CompiledTitleAnimation& a){
        Json out = Json::object();
        for (const ta::CompiledTitleTrack& t : a.tracks){
            const std::string cls = ta::trackClassName(t.cls);
            out[cls] = out.value(cls, 0) + 1;
        }
        return out;
    }

    Json costJson(Project* project, const fs::path& file, const tt::TitleArtDoc& doc, const ta::CompiledTitleAnimation& a,
                  std::map<int, tt::TitleAnimPlan>& plans, const std::vector<ta::CompiledTitleAnimation>& all){
        TitleAnimCostInputs inputs;
        inputs.still = stillOf(file);
        inputs.outputScale = outputScaleOf(inputs.still);
        ta::TitleAnimData baked;
        std::string error;
        if (ta::loadAnimData(file.string() + ta::TITLE_ANIM_DATA_SUFFIX, baked, nullptr, &error)) inputs.baked = &baked;
        Json out = Json::object();
        for (TitleArtRenderMode mode : {TitleArtRenderMode::BAKED, TitleArtRenderMode::REALTIME, TitleArtRenderMode::GLYPH_ATLAS}){
            auto it = plans.find((int)mode);
            if (it == plans.end()) it = plans.emplace((int)mode, tt::buildAnimPlan(doc, all, mode)).first;
            const tt::AnimationPlan* ap = it->second.find(a.name);
            if (!ap) continue;
            if (mode == TitleArtRenderMode::REALTIME && ap->rerender && inputs.renderUs == 0.0)
                inputs.renderUs = titleRenderEstimateUs(TitleArtRenderHost::editor().resolver(project), doc, inputs.outputScale,
                                                        inputs.still.frame.empty() ? tt::TitleRectI() : tt::TitleRectI{inputs.still.frame.x, inputs.still.frame.y,
                                                                                                                       inputs.still.frame.width, inputs.still.frame.height});
            const TitleAnimCost cost = estimateAnimCost(doc, a, *ap, inputs);
            out[tt::renderModeName(mode)] = fromOrdered(cost.toJson());
            out[tt::renderModeName(mode)]["summary"] = cost.summary();
        }
        return out;
    }

    // keys in the compact grammar ([t, v], [t, v, ease])
    Json animationDetail(const tt::TitleArtDoc& doc, const ta::TitleAnimation& a){
        Json out = fromOrdered(ta::encodeAnimation(a));
        Json tracks = Json::array();
        for (size_t i = 0; i < a.tracks.size(); i++){
            Json t = out["tracks"][i];
            const tt::TrackFieldInfo info = tt::resolveTrackField(doc, a.tracks[i].layer, a.tracks[i].field);
            t["label"] = info.ok ? info.label : a.tracks[i].field;
            if (a.tracks[i].orphan) t["orphan"] = true;
            tracks.push_back(t);
        }
        out["tracks"] = tracks;
        return out;
    }

    bool timesOf(const Json& anim, const ta::CompiledTitleAnimation& a, std::vector<double>& times, std::string& error){
        if (anim.contains("times")){
            if (!anim["times"].is_array() || anim["times"].empty() || anim["times"].size() > 24){
                error = "options.animation.times is 1..24 times in seconds";
                return false;
            }
            for (const Json& t : anim["times"]){
                if (!t.is_number()){
                    error = "options.animation.times is 1..24 times in seconds";
                    return false;
                }
                times.push_back(std::max(0.0, t.get<double>()));
            }
            return true;
        }
        if (anim.contains("frames")){
            const int n = anim["frames"].is_number_integer() ? anim["frames"].get<int>() : 0;
            if (n < 2 || n > 24){
                error = "options.animation.frames is 2..24";
                return false;
            }
            // evenly over the duration (a loop: over its loop span, after the first pass)
            const bool loop = a.wrap != ta::AnimWrap::Once;
            const double from = loop ? a.duration : 0.0;
            const double span = loop ? a.duration - a.spanStart() : a.duration;
            for (int k = 0; k < n; k++) times.push_back(from + span * k / (loop ? n : n - 1));
            return true;
        }
        times.push_back(anim.contains("time") && anim["time"].is_number() ? std::max(0.0, anim["time"].get<double>()) : 0.0);
        return true;
    }

}

// ==================================================================== get_title (R51)

Json mcp::titletools::titleAnimationsOutline(Project* project, const fs::path& file, const tt::TitleArtDoc& doc){
    if (!tt::hasAnimations(doc)) return nullptr;
    const ta::TitleAnimationSet set = tt::readAnimationSet(doc);
    const auto lib = tt::compileAnimations(doc, nullptr, nullptr);
    std::map<int, tt::TitleAnimPlan> plans;
    Json out = Json::array();
    for (const ta::CompiledTitleAnimation& a : lib->animations){
        const ta::TitleAnimation* authored = set.find(a.name);
        int orphans = 0;
        if (authored) for (const ta::TitleTrack& t : authored->tracks) orphans += t.orphan ? 1 : 0;
        Json row = {{"name", a.name}, {"role", ta::animRoleName(a.role)}, {"duration", a.duration}, {"wrap", ta::animWrapName(a.wrap)},
                    {"tracks", a.tracks.size()}, {"markers", a.markers.size()}, {"classes", classesOf(a)},
                    {"cost", costJson(project, file, doc, a, plans, lib->animations)}};
        if (a.overlay) row["overlay"] = true;
        if (!a.next.empty()) row["next"] = a.next;
        if (orphans) row["orphans"] = orphans;
        if (set.defaultName == a.name) row["default"] = true;
        out.push_back(row);
    }
    return out;
}

bool mcp::titletools::titleAnimationGet(const tt::TitleArtDoc& doc, const std::string& name, Json& out, std::string& error){
    const ta::TitleAnimationSet set = tt::readAnimationSet(doc);
    if (name == "*"){
        out = Json::array();
        for (const ta::TitleAnimation& a : set.animations) out.push_back(animationDetail(doc, a));
        return true;
    }
    const int index = findTitleAnimation(set, name);
    if (index < 0){
        std::string list;
        for (const ta::TitleAnimation& a : set.animations) list += (list.empty() ? "" : ", ") + a.name;
        error = "animation: no animation '" + name + "' (animations: " + (list.empty() ? "none" : list) + ")";
        return false;
    }
    out = animationDetail(doc, set.animations[(size_t)index]);
    return true;
}

// ==================================================================== edit_title (R50)

Json mcp::titletools::titleAnimationChanges(Project* project, const fs::path& file, const tt::TitleArtDoc& before, const tt::TitleArtDoc& after){
    const ta::TitleAnimationSet a = tt::readAnimationSet(before), b = tt::readAnimationSet(after);
    Json changes = Json::array();
    std::vector<std::string> touched;
    for (const ta::TitleAnimation& anim : b.animations){
        const ta::TitleAnimation* old = a.find(anim.name);
        for (const ta::TitleTrack& t : anim.tracks){
            bool same = false;
            if (old){
                for (const ta::TitleTrack& o : old->tracks)
                    if (o.layer == t.layer && o.field == t.field && ta::encodeTrack(o) == ta::encodeTrack(t)) same = true;
            }
            if (same) continue;
            const tt::TrackFieldInfo info = tt::resolveTrackField(after, t.layer, t.field);
            ta::TitleAnimation one;
            one.name = anim.name;
            one.tracks = {t};
            const ta::CompiledTitleAnimation c = tt::compileAnimation(after, one, nullptr, nullptr);
            changes.push_back({{"animation", anim.name}, {"track", (t.layer.empty() ? std::string("title") : t.layer) + "/" + t.field},
                               {"keys", t.keys.size()}, {"class", c.tracks.empty() ? "none" : ta::trackClassName(c.tracks[0].cls)},
                               {"label", info.ok ? info.label : t.field}});
            if (std::find(touched.begin(), touched.end(), anim.name) == touched.end()) touched.push_back(anim.name);
        }
        if (!old || ta::encodeAnimation(*old) != ta::encodeAnimation(anim))
            if (std::find(touched.begin(), touched.end(), anim.name) == touched.end()) touched.push_back(anim.name);
    }
    for (const ta::TitleAnimation& anim : a.animations)
        if (!b.find(anim.name)) changes.push_back({{"animation", anim.name}, {"removed", true}});
    if (changes.empty() && touched.empty()) return nullptr;
    Json out = {{"animation_changes", changes}};
    // R20's cost of each animation the batch touched
    const auto lib = tt::compileAnimations(after, nullptr, nullptr);
    std::map<int, tt::TitleAnimPlan> plans;
    Json costs = Json::object();
    for (const std::string& name : touched)
        if (const ta::CompiledTitleAnimation* c = lib->find(name)) costs[name] = costJson(project, file, after, *c, plans, lib->animations);
    out["animation_cost"] = costs;
    return out;
}

// ==================================================================== render_title options.animation (R53)

bool mcp::titletools::checkAnimationOption(const Json& value, std::string& error){
    if (!value.is_object() || !value.contains("name") || !value["name"].is_string()){
        error = "bad_option: `options.animation` is {name, time | times (<= 24) | frames (2..24), as}.";
        return false;
    }
    for (auto it = value.begin(); it != value.end(); ++it){
        const std::string& k = it.key();
        if (k != "name" && k != "time" && k != "times" && k != "frames" && k != "as"){
            error = "bad_option: options.animation has no '" + k + "' (name, time, times, frames, as).";
            return false;
        }
    }
    TitleAnimRenderAs as;
    if (value.contains("as") && (!value["as"].is_string() || !titleAnimRenderAsFromName(value["as"].get<std::string>(), as))){
        error = "bad_option: options.animation.as is exact, baked, realtime or glyph_atlas.";
        return false;
    }
    return true;
}

void mcp::titletools::renderTitleAnimation(const CallPtr& call, RenderSpec spec){
    Project* project = call->ctx().project();
    const Json& args = spec.args;
    const Json anim = args["options"]["animation"];
    if (args.contains("compare")){
        call->fail("compare: kept revision renders are stills; compare the times of one revision with `try` (each variant at the same times).");
        return;
    }
    struct State {
        std::vector<std::pair<std::string, std::shared_ptr<tt::TitleArtDoc>>> variants;
        std::vector<std::unique_ptr<TitleAnimRenderer>> renderers;
        std::vector<double> times;
        TitleAnimRenderAs as = TitleAnimRenderAs::Exact;
        std::string name;
        std::vector<TitleAnimFrame> frames;     // variant-major
        std::vector<SheetBackground> backgrounds;
        int maxWidth = 768, columns = 0;
        bool images = true;
        tt::TitleRectI region;
        size_t next = 0;
        std::string relative;
        std::string savePath;
        Project* project = nullptr;
    };
    auto s = std::make_shared<State>();
    std::string error;
    s->name = anim["name"].get<std::string>();
    if (anim.contains("as")) titleAnimRenderAsFromName(anim["as"].get<std::string>(), s->as);
    s->relative = spec.relative;
    s->savePath = args.value("save_path", std::string());
    s->project = call->ctx().project();
    if (!parseBackgrounds(project, args.contains("backgrounds") ? args["backgrounds"] : Json(), s->backgrounds, error)){
        call->fail(error);
        return;
    }
    s->maxWidth = std::clamp(args.value("max_width", 768), 64, 4096);
    s->columns = std::clamp(args.value("columns", 0), 0, 24);
    s->images = args.value("image", std::string("always")) != "none";
    if (args.contains("crop")){
        const Json& crop = args["crop"];
        if (!crop.is_array() || crop.size() != 4){
            call->fail("`crop` is [x, y, w, h] in canvas pixels (image space, top-left origin).");
            return;
        }
        s->region = {crop[0].get<int>(), crop[1].get<int>(), crop[2].get<int>(), crop[3].get<int>()};
    }
    // the variants: the document (with `ops`), then each `try`
    auto whatIf = [&](const tt::TitleArtDoc& base, const Json& ops, std::shared_ptr<tt::TitleArtDoc>& out) -> bool{
        if (ops.is_null() || (ops.is_array() && ops.empty())){
            out = std::make_shared<tt::TitleArtDoc>(base);
            return true;
        }
        const TitleOpsResult r = TitleArtOps::apply(base, toOrdered(ops));
        if (!r.ok){
            error = r.error();
            return false;
        }
        out = std::make_shared<tt::TitleArtDoc>(r.doc);
        return true;
    };
    std::shared_ptr<tt::TitleArtDoc> base;
    if (!whatIf(*spec.doc, args.contains("ops") ? args["ops"] : Json(), base)){
        call->fail("ops: " + error);
        return;
    }
    s->variants.push_back({"current", base});
    if (args.contains("try")){
        if (!args["try"].is_array() || args["try"].size() > 9){
            call->fail("`try` is a list of at most 9 {label, ops}.");
            return;
        }
        for (size_t i = 0; i < args["try"].size(); i++){
            std::shared_ptr<tt::TitleArtDoc> v;
            if (!whatIf(*base, args["try"][i].value("ops", Json::array()), v)){
                call->fail("try[" + std::to_string(i) + "]: " + error + " Nothing was rendered.");
                return;
            }
            s->variants.push_back({args["try"][i].value("label", "try " + std::to_string(i + 1)), v});
        }
    }
    const TitleArtBakeInfo still = stillOf(spec.file);
    for (const auto& v : s->variants){
        auto r = std::make_unique<TitleAnimRenderer>(project, v.second);
        r->setStillFrame(still);
        if (!r->library().find(s->name)){
            std::string list;
            for (const std::string& n : r->library().names()) list += (list.empty() ? "" : ", ") + n;
            call->fail("options.animation.name: no animation '" + s->name + "' (animations: " + (list.empty() ? "none" : list) + ").",
                       Json{{"code", "unknown_animation"}, {"valid", r->library().names()}});
            return;
        }
        if (s->renderers.empty() && !timesOf(anim, *r->library().find(s->name), s->times, error)){
            call->fail(error);
            return;
        }
        s->renderers.push_back(std::move(r));
    }
    const int cw = base->canvasWidth();
    const int regionW = s->region.empty() ? cw : s->region.width;
    const int perRow = s->columns > 0 ? s->columns : (int)std::min<size_t>(s->times.size(), 6);
    const float scale = std::min(1.0f, (float)s->maxWidth / (float)std::max(1, regionW * std::max(1, perRow)));
    auto task = std::make_shared<FrameTask>();
    std::weak_ptr<FrameTask> weakTask = task;
    task->tick = [s, scale, weakTask](McpJob& job){
        auto t = weakTask.lock();
        if (!t) return true;
        const size_t total = s->variants.size() * s->times.size();
        if (s->next < total){
            const size_t v = s->next / s->times.size(), k = s->next % s->times.size();
            TitleAnimFrameRequest request;
            request.animation = s->name;
            request.time = s->times[k];
            request.as = s->as;
            request.pxScale = scale;
            request.region = s->region;
            TitleAnimFrame frame;
            std::string error;
            if (!s->renderers[v]->render(request, frame, error)){
                job.fail("The render at " + std::to_string(request.time) + " s failed: " + error);
                return true;
            }
            s->frames.push_back(std::move(frame));
            s->next++;
            job.setProgress((double)s->next / (double)total, "frame " + std::to_string(s->next) + "/" + std::to_string(total));
            return false;
        }
        // the cells: time, alpha bounds, coverage and the change from the cell before (R53)
        Json cells = Json::array();
        std::vector<texsynth::Image> images;
        std::vector<SheetCell> boxes;
        for (size_t i = 0; i < s->frames.size(); i++){
            const TitleAnimFrame& f = s->frames[i];
            const size_t v = i / s->times.size(), k = i % s->times.size();
            double changed = 0.0;
            if (k > 0){
                const TitleAnimFrame& p = s->frames[i - 1];
                size_t ink = 0, diff = 0;
                for (size_t q = 0; q < f.image.pixelCount() && q < p.image.pixelCount(); q++){
                    if (f.image.data[q * 4 + 3] == 0 && p.image.data[q * 4 + 3] == 0) continue;
                    ink++;
                    int d = 0;
                    for (int c = 0; c < 4; c++) d = std::max(d, std::abs((int)f.image.data[q * 4 + c] - (int)p.image.data[q * 4 + c]));
                    if (d > 2) diff++;
                }
                changed = ink ? 100.0 * (double)diff / (double)ink : 0.0;
            }
            const tt::TitleRectI& b = f.alphaBounds;
            Json cell = {{"variant", s->variants[v].first}, {"time", s->times[k]}, {"local_time", f.localTime},
                         {"alpha_bounds", b.empty() ? Json(nullptr) : Json::array({b.x, b.y, b.width, b.height})},
                         {"coverage_pct", std::round(f.coveragePct * 100.0) / 100.0}, {"changed_pct", std::round(changed * 100.0) / 100.0}};
            if (!f.notPlayed.empty()) cell["not_played"] = f.notPlayed;
            if (f.cells) cell["glyph_cells"] = true;
            cells.push_back(cell);
            if (s->images){
                for (const SheetBackground& bg : s->backgrounds){
                    images.push_back(composite(f.image, bg));
                    SheetCell box;
                    box.n = (int)boxes.size();
                    char label[64];
                    std::snprintf(label, sizeof(label), "%.3g s", s->times[k]);
                    box.label = (s->variants.size() > 1 ? s->variants[v].first + " " : std::string()) + label;
                    box.background = bg.name;
                    boxes.push_back(box);
                }
            }
        }
        Json result = {{"path", s->relative}, {"animation", s->name}, {"as", titleAnimRenderAsName(s->as)}, {"cells", cells}};
        if (s->images && !images.empty()){
            const int columns = s->columns > 0 ? s->columns : (int)std::min<size_t>(s->times.size(), 6) * (int)s->backgrounds.size();
            const texsynth::Image full = sheet(images, columns, boxes);
            const texsynth::Image shown = downscaleTo(full, s->maxWidth);
            Json boxJson = Json::array();
            const double factor = full.width > 0 ? (double)shown.width / full.width : 1.0;
            for (const SheetCell& c : boxes)
                boxJson.push_back({{"n", c.n}, {"label", c.label}, {"background", c.background},
                                   {"box", Json::array({(int)std::lround(c.x * factor), (int)std::lround(c.y * factor), (int)std::lround(c.width * factor),
                                                        (int)std::lround(c.height * factor)})}});
            result["sheet"] = boxJson;
            t->images.push_back(toolImage(shown));
        }
        // the frame itself, with its real alpha (render_title's save_path; a filmstrip has several)
        if (!s->savePath.empty()){
            if (s->frames.size() != 1){
                job.fail("save_path: options.animation renders " + std::to_string(s->frames.size()) + " frames: give one time to save it");
                return true;
            }
            fs::path file;
            std::string error;
            std::vector<uint8_t> png;
            if (!McpRefs::resolveProjectWritePath(s->project, s->savePath, file, error)){
                job.fail("save_path: " + error);
                return true;
            }
            if (!texsynth::encodePng(s->frames[0].image, png) || !FileUtils::writeFileReplacing(file, std::string(png.begin(), png.end()))){
                job.fail("save_path: could not write " + s->savePath);
                return true;
            }
            result["saved_to"] = s->project->normalizeToProjectRelative(file).generic_string();
        }
        t->text = "Rendered " + std::to_string(s->frames.size()) + " frame(s) of '" + s->name + "' (" + titleAnimRenderAsName(s->as) + ") of " + s->relative + ".";
        job.succeed(t->text, result);
        return true;
    };
    runFrameTask(call, "render_title", "Render " + spec.relative + " " + s->name, task);
}

// ==================================================================== check_title (R54)

void mcp::titletools::setupAnimationChecks(TitleCheckContext& context, Project* project, const std::shared_ptr<const tt::TitleArtDoc>& doc, const Json& args,
                                           std::shared_ptr<void>& keep){
    if (args.contains("animation") && args["animation"].is_string()) context.options.animation = args["animation"].get<std::string>();
    context.assetExists = [project](const std::string& path, const std::string& guid){
        if (!project) return false;
        AssetDatabase& database = project->getAssetDatabase();
        if (!guid.empty() && !database.getPath(guid).empty()) return true;
        std::error_code ec;
        return !path.empty() && fs::exists(project->getProjectPath() / project->normalizeToProjectRelative(project->getAssetsPath() / path), ec);
    };
    if (!tt::hasAnimations(*doc)) return;
    auto renderer = std::make_shared<TitleAnimRenderer>(project, doc);
    renderer->setStillFrame(stillOf(context.file));
    keep = renderer;
    const std::weak_ptr<TitleAnimRenderer> weak = renderer;
    context.renderAnimation = [weak, project, doc](const std::string& animation, double time, const std::string& as, bool companion, TitleCheckImage& out,
                                                   std::string& error){
        std::shared_ptr<TitleAnimRenderer> r = weak.lock();
        if (!r) return false;
        const float scale = std::min(1.0f, 512.0f / (float)std::max(1, doc->canvasWidth()));
        if (as == "still"){
            tt::RenderRequest request;
            request.doc = doc;
            request.pxScale = scale;
            TitleRenderedImage image;
            if (!TitleArtRenderHost::editor().render(project, request, image, error)) return false;
            out.width = image.image.width;
            out.height = image.image.height;
            out.rgba = image.image.data;
            out.frame = image.frame;
            return true;
        }
        TitleAnimFrameRequest request;
        request.animation = animation;
        request.time = std::max(0.0, time);
        request.pxScale = scale;
        request.companion = companion;
        if (as == "cells_at_rest"){
            // the cells with no glyph moved: an Exact frame of a time before anything starts is
            // not that (keys hold their first value), so the rest is forced
            request.forceCells = true;
            request.companion = false;
        }else{
            titleAnimRenderAsFromName(as, request.as);
        }
        TitleAnimFrame frame;
        if (as == "cells_at_rest"){
            // a pose with every glyph at rest: the document's still values under the compose
            tt::TitleArtDoc rest = *doc;
            ta::TitleAnimationSet set = tt::readAnimationSet(rest);
            ta::TitleAnimation* a = set.find(animation);
            if (!a) return false;
            for (ta::TitleTrack& t : a->tracks) t.mute = true;
            tt::writeAnimationSet(rest, set);
            TitleAnimRenderer still(project, std::make_shared<tt::TitleArtDoc>(rest));
            if (!still.render(request, frame, error)) return false;
        }else if (!r->render(request, frame, error)){
            return false;
        }
        out.width = frame.image.width;
        out.height = frame.image.height;
        out.rgba = frame.image.data;
        out.frame = frame.frame;
        return true;
    };
}

// ==================================================================== export_title (R55)

std::shared_ptr<TitleFlipbookBakeJob> mcp::titletools::titleFlipbookJob(Project* project, const fs::path& file, const tt::TitleArtDoc& doc, const Json& args,
                                                                         std::string& error){
    if (!tt::hasAnimations(doc)) return nullptr;
    TitleFlipbookOptions options;
    if (args.contains("animations")){
        if (!args["animations"].is_array()){
            error = "`animations` is a list of animation names";
            return nullptr;
        }
        for (const Json& n : args["animations"]) if (n.is_string()) options.animations.push_back(n.get<std::string>());
        const ta::TitleAnimationSet set = tt::readAnimationSet(doc);
        for (const std::string& n : options.animations){
            if (!set.find(n)){
                std::string list;
                for (const ta::TitleAnimation& a : set.animations) list += (list.empty() ? "" : ", ") + a.name;
                error = "animations: no animation '" + n + "' (animations: " + list + ")";
                return nullptr;
            }
        }
    }
    options.dryRun = args.value("dry_run", false);
    options.outputScale = outputScaleOf(stillOf(file));
    return std::make_shared<TitleFlipbookBakeJob>(project, file, doc, options);
}

Json mcp::titletools::flipbookReportJson(const TitleFlipbookResult& result, Project* project){
    Json out = fromOrdered(result.toJson());
    // R38: against the budgets (Project Settings > Title Art > Animation)
    TitleFlipbookBudget budget;
    if (project){
        const TitleArtProjectSettings& settings = project->getTitleArtSettings();
        budget.desktop = settings.flipbookBudgetMiB.desktop;
        budget.mobile = settings.flipbookBudgetMiB.mobile;
        budget.androidEtc2 = settings.flipbookBudgetAndroidEtc2MiB;
        budget.web = settings.flipbookBudgetMiB.web;
    }
    TitleFlipbookBytes total;
    for (const TitleFlipbookReport& r : result.animations){
        total.desktop += r.bytes.desktop;
        total.mobile += r.bytes.mobile;
        total.androidEtc2 += r.bytes.androidEtc2;
        total.web += r.bytes.web;
    }
    Json families = Json::object();
    for (const char* f : TITLE_FLIPBOOK_FAMILIES){
        const double mib = (double)total.of(f) / (1024.0 * 1024.0);
        families[f] = {{"mib", std::round(mib * 100.0) / 100.0}, {"budget_mib", budget.of(f)}, {"over", mib > budget.of(f)}};
    }
    out["budget"] = families;
    return out;
}

// ==================================================================== list_title_library animation_presets (R52)

void mcp::titletools::listAnimationPresets(const CallPtr& call){
    Project* project = call->ctx().project();
    if (project) TitleAnimPresets::setProjectDir(project->getProjectPath().string());
    const Json& args = call->args();
    const std::string id = args.value("id", std::string());
    const std::string query = args.value("query", std::string());
    const std::string category = args.value("category", std::string());
    const int limit = std::clamp(args.value("limit", 20), 1, 200), offset = std::max(0, args.value("offset", 0));
    // each preset's classes, from applying it to a reference title (a text layer, the main text)
    auto classesFor = [](const TitleAnimPreset& p) -> Json{
        tt::TitleArtDoc doc = tt::makeNewTitleArt();
        const TitleOpsResult added = TitleArtOps::apply(doc, toOrdered(Json::array({Json{{"op", "add_animation"}, {"name", "Preview"}, {"preset", p.id}}})));
        if (!added.ok) return Json{{"error", added.error()}};
        const auto lib = tt::compileAnimations(added.doc, nullptr, nullptr);
        const ta::CompiledTitleAnimation* a = lib->find("Preview");
        Json out = Json::object();
        if (!a) return out;
        for (TitleArtRenderMode mode : {TitleArtRenderMode::BAKED, TitleArtRenderMode::REALTIME, TitleArtRenderMode::GLYPH_ATLAS}){
            const tt::TitleAnimPlan plan = tt::buildAnimPlan(added.doc, lib->animations, mode);
            const tt::AnimationPlan* ap = plan.find("Preview");
            Json hows = Json::object();
            if (ap) for (const tt::TrackPlan& t : ap->tracks) hows[t.how] = hows.value(t.how, 0) + 1;
            out[tt::renderModeName(mode)] = hows;
        }
        bool titleLive = true;
        for (const ta::CompiledTitleTrack& t : a->tracks) titleLive = titleLive && t.cls == ta::TrackClass::TitleLive;
        out["title_live"] = titleLive;
        return out;
    };
    if (!id.empty()){
        const TitleAnimPreset* p = TitleAnimPresets::find(id);
        if (!p){
            const std::vector<std::string> ids = TitleAnimPresets::ids();
            call->fail("No Animation Preset '" + id + "'" + (tt::nearestName(id, ids).empty() ? "" : " (did you mean '" + tt::nearestName(id, ids) + "'?)") + ".",
                       Json{{"valid", ids}});
            return;
        }
        Json params = Json::array();
        std::string lines;
        for (const TitleAnimPresetParam& q : p->params){
            Json row = {{"name", q.name}, {"label", q.label}, {"type", q.type}, {"default", fromOrdered(q.defaultValue)}};
            if (q.max > q.min) row["range"] = Json::array({q.min, q.max});
            if (!q.options.empty()) row["options"] = q.options;
            params.push_back(row);
            lines += "\n" + q.name + " (" + q.type + ") = " + fromOrdered(q.defaultValue).dump();
        }
        Json op = {{"op", "apply_animation_preset"}, {"animation", "<animation>"}, {"preset", p->id}, {"params", Json::object()}};
        call->ok(p->name + " (" + p->id + "): " + p->description + lines,
                 Json{{"id", p->id}, {"name", p->name}, {"group", p->group}, {"role", p->role}, {"duration", fromOrdered(p->data["duration"])},
                      {"wrap", p->wrap}, {"overlay", p->overlay}, {"description", p->description}, {"params", params}, {"classes", classesFor(*p)},
                      {"op", op}, {"scope", p->scope}});
        return;
    }
    std::vector<const TitleAnimPreset*> rows;
    for (const TitleAnimPreset& p : TitleAnimPresets::all()){
        if (!category.empty() && p.group != category) continue;
        if (!query.empty()){
            std::string hay = p.id + " " + p.name + " " + p.description + " " + p.group;
            std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char c){ return (char)std::tolower(c); });
            std::string q = query;
            std::transform(q.begin(), q.end(), q.begin(), [](unsigned char c){ return (char)std::tolower(c); });
            if (hay.find(q) == std::string::npos) continue;
        }
        rows.push_back(&p);
    }
    Json list = Json::array();
    for (size_t i = (size_t)offset; i < rows.size() && list.size() < (size_t)limit; i++){
        const TitleAnimPreset& p = *rows[i];
        const Json classes = classesFor(p);
        list.push_back({{"id", p.id}, {"name", p.name}, {"group", p.group}, {"role", p.role}, {"duration", fromOrdered(p.data["duration"])},
                        {"classes", classes.value("baked", Json::object())}, {"title_live", classes.value("title_live", false)},
                        {"use", "edit_title {ops: [{op: \"add_animation\", name, preset: \"" + p.id + "\"}]}"}});
    }
    call->ok(std::to_string(rows.size()) + " Animation Presets" + (query.empty() ? "" : " matching '" + query + "'") + ".",
             Json{{"kind", "animation_presets"}, {"rows", list}, {"total", rows.size()}, {"offset", offset}});
}
