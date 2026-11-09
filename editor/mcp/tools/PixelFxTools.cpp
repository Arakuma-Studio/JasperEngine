// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The pixel_art group's capture_contact_sheet (docs/specs/S48-pixel-fx.md R27, R28, R44): a target
// drawn at fixed times into one labelled PNG on named backdrops, with the effect lints as the
// verdict tools' check rows (util/QaChecks.h, the rows preview_particle_effect returns). A preview
// until output: true writes the PNG at path (one undo step).

#include "McpTools.h"
#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/tools/PlayViewCommon.h"
#include "mcp/tools/W4Shared.h"
#include "EditorHost.h"
#include "Project.h"
#include "command/CommandHistory.h"
#include "command/UndoOrigin.h"
#include "pixelart/PixelProblem.h"
#include "pixelart/fx/ContactSheet.h"
#include "pixelart/fx/EffectCapture.h"
#include "pixelart/lint/EffectLints.h"
#include "util/Base64.h"
#include "util/FileUtils.h"
#include "util/QaChecks.h"
#include "util/Util.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor::mcp {

namespace pixelfxtools {
namespace {

namespace fs = std::filesystem;
namespace epx = jasperengine::editor::pixelart;
namespace efx = jasperengine::editor::pixelart::fx;
namespace lint = jasperengine::editor::pixelart::lint;

constexpr int kMaxFrames = 64;
constexpr double kMaxDuration = 60.0;
constexpr double kJobWaitSeconds = 120.0;
// S35's fix rounds (ParticleQaSettings::fixRounds' default): after this many FAILs in a row, stop
constexpr int kFixRounds = 2;
constexpr const char* kClosing = "Look at the sheet as well as the checks: they measure mistakes, not taste.";

using Problem = epx::PixelProblem;

Problem problem(const std::string& kind, const std::string& field, const std::string& message, const std::string& fix) {
    Problem p;
    p.kind = kind;
    p.field = field;
    p.message = message;
    p.fix = fix;
    return p;
}

void failCall(const CallPtr& call, const std::string& headline, const std::vector<Problem>& problems) {
    std::string text = headline;
    Json list = Json::array();
    for (const Problem& p : problems) {
        text += "\n- " + p.kind + ": " + p.message + (p.fix.empty() ? "" : " Fix: " + p.fix);
        Json j = {{"kind", p.kind}, {"message", p.message}, {"fix", p.fix}};
        if (!p.field.empty()) j["field"] = p.field;
        list.push_back(std::move(j));
    }
    call->fail(text, Json{{"errors", list}});
}

// the consecutive FAILs of each check by target (S35's anti-gaming rounds)
qa::FailRounds& failRounds() {
    static qa::FailRounds rounds;
    return rounds;
}

// the captures in flight: kept alive until they end (never destroyed inside their own callback)
std::vector<std::shared_ptr<efx::EffectCapture>>& activeCaptures() {
    static std::vector<std::shared_ptr<efx::EffectCapture>> runs;
    return runs;
}

struct Args {
    std::unique_ptr<efx::CaptureTarget> target;
    std::string scope;
    efx::CaptureSettings capture;
    efx::SheetSettings sheet;
    bool write = false;
    std::string path;
    fs::path absolute;
};

bool readInt(const Json& args, const char* key, int lo, int hi, int& out, std::vector<Problem>& problems) {
    if (!args.contains(key) || args[key].is_null()) return true;
    if (!args[key].is_number_integer() || args[key].get<long long>() < lo || args[key].get<long long>() > hi) {
        problems.push_back(problem("bad_value", key, std::string(key) + " must be a whole number from " + std::to_string(lo) + " to " + std::to_string(hi),
                                   "give " + std::string(key) + " in range or leave it out"));
        return false;
    }
    out = args[key].get<int>();
    return true;
}

bool parseArgs(const CallPtr& call, Args& a, std::vector<Problem>& problems) {
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    const Json& args = call->args();
    if (!project) {
        problems.push_back(problem("no_project", "", "no project is open", "open a project"));
        return false;
    }
    // ---- target
    const Json target = args.value("target", Json());
    if (!target.is_object()) {
        problems.push_back(problem("missing_target", "target", "target is required",
                                   "give target {entity, scene?, animation?} for a GameObject or target {particles: \"<.particles path>\"}"));
        return false;
    }
    for (auto it = target.begin(); it != target.end(); ++it) {
        if (it.key() != "entity" && it.key() != "scene" && it.key() != "animation" && it.key() != "particles") {
            problems.push_back(problem("unknown_key", "target." + it.key(), "target has no key " + it.key(),
                                       "use entity (with scene and animation) or particles"));
            return false;
        }
    }
    const bool hasEntity = target.contains("entity"), hasParticles = target.contains("particles");
    if (hasEntity == hasParticles) {
        problems.push_back(problem("bad_target", "target", "target needs exactly one of entity or particles",
                                   "give target {entity: ...} for a GameObject, or target {particles: \"assets/fx/x.particles\"}"));
        return false;
    }
    std::string error;
    if (hasParticles) {
        if (target.contains("animation") || target.contains("scene")) {
            problems.push_back(problem("bad_target", "target.particles", "a .particles target takes no scene or animation",
                                       "capture a GameObject (target.entity) to use its scene or a clip"));
            return false;
        }
        const std::string value = target["particles"].is_string() ? target["particles"].get<std::string>() : std::string();
        fs::path absolute;
        if (!Util::isParticleEffectFile(value)) {
            problems.push_back(problem("bad_path", "target.particles", "\"" + value + "\" is not a .particles file",
                                       "give a project-relative .particles path, e.g. assets/fx/fire.particles"));
            return false;
        }
        if (!McpRefs::resolveProjectPath(project, value, absolute, error, false)) {
            problems.push_back(problem("bad_path", "target.particles", error, "give a project-relative .particles path"));
            return false;
        }
        if (!project->isInsideAssetsPath(absolute.string())) absolute = project->resolveAssetPath(value);
        if (!project->isInsideAssetsPath(absolute.string()) || !fs::exists(absolute)) {
            problems.push_back(problem("not_found", "target.particles", "no effect at \"" + value + "\" in the assets folder",
                                       "create it with create_particle_effect, or fix the path"));
            return false;
        }
        const std::string asset = project->normalizeToAssetsRelative(absolute).generic_string();
        a.target = efx::makeParticlesTarget(project, asset, error);
        a.scope = asset;
    } else {
        SceneProject* scene = McpRefs::resolveScene(project, target.contains("scene") ? target["scene"] : Json(), error);
        if (!scene) {
            problems.push_back(problem("bad_scene", "target.scene", error, "give the scene's id, name or path (list_scenes)"));
            return false;
        }
        if (!scene->scene) {
            problems.push_back(problem("scene_closed", "target.scene", McpRefs::notOpenError(project, *scene), "open the scene, then call again"));
            return false;
        }
        const Entity entity = McpRefs::resolveEntity(scene, target["entity"], error);
        if (entity == NULL_ENTITY) {
            problems.push_back(problem("bad_entity", "target.entity", error, "give the GameObject's id, name or path (find_entities)"));
            return false;
        }
        std::string clip;
        if (target.contains("animation")) {
            const std::string value = target["animation"].is_string() ? target["animation"].get<std::string>() : std::string();
            fs::path absolute;
            if (fs::path(value).extension() != ".anim" || !McpRefs::resolveProjectPath(project, value, absolute, error, true)) {
                problems.push_back(problem("bad_path", "target.animation", "\"" + value + "\" is not an .anim clip of the project" +
                                           (error.empty() ? std::string() : ": " + error), "give a project-relative .anim path"));
                return false;
            }
            clip = fs::relative(absolute, project->getProjectPath()).generic_string();
        }
        a.target = efx::makeEntityTarget(project, scene, entity, clip, error);
        a.scope = scene->name + "/" + McpRefs::entityPath(scene, entity) + (clip.empty() ? "" : "#" + clip);
    }
    if (!a.target) {
        problems.push_back(problem("bad_target", "target", error, "fix the asset, then call again"));
        return false;
    }

    // ---- mode
    const std::string mode = args.value("mode", std::string("isolated"));
    if (mode != "isolated" && mode != "scene") {
        problems.push_back(problem("bad_value", "mode", "mode must be isolated or scene", "leave it out (isolated) or give scene"));
        return false;
    }
    a.capture.mode = mode == "scene" ? efx::CaptureMode::Scene : efx::CaptureMode::Isolated;
    if (a.capture.mode == efx::CaptureMode::Scene) {
        SceneProject* source = a.target->sourceScene();
        if (!source) {
            problems.push_back(problem("bad_mode", "mode", "a .particles asset has no scene to show", "capture it isolated, or a GameObject playing it in scene mode"));
            return false;
        }
        if (ctx.isPlaying()) {
            const uint32_t playId = project->getPlaySessionSceneId();
            SceneProject* runtime = McpRefs::runtimeScene(project, source);
            if (playId == 0 || (source->id != playId && (!runtime || runtime->id != playId))) {
                problems.push_back(problem("bad_mode", "mode", "scene mode in Play captures the played scene, and the target is in another",
                                           "capture a GameObject of the played scene, or stop play"));
                return false;
            }
            a.capture.sceneId = playId;
            a.capture.acquireAdvance = [&ctx]() { return std::static_pointer_cast<void>(std::make_shared<w3::GameAdvance>(ctx)); };
        }
    }

    // ---- times
    double duration = a.target->defaultDuration();
    if (args.contains("duration") && !args["duration"].is_null()) {
        if (!args["duration"].is_number() || args["duration"].get<double>() <= 0.0 || args["duration"].get<double>() > kMaxDuration) {
            problems.push_back(problem("bad_value", "duration", "duration must be seconds above 0, at most 60", "give a duration in (0, 60]"));
            return false;
        }
        duration = args["duration"].get<double>();
    }
    bool loop = a.target->defaultLoop();
    if (args.contains("loop") && args["loop"].is_boolean()) loop = args["loop"].get<bool>();
    std::vector<double> times;
    if (args.contains("times") && !args["times"].is_null()) {
        if (args.contains("frames")) {
            problems.push_back(problem("conflict", "times", "give frames or times, not both", "drop one"));
            return false;
        }
        if (!args["times"].is_array() || args["times"].empty() || args["times"].size() > (size_t)kMaxFrames) {
            problems.push_back(problem("bad_value", "times", "times must be 1 to 64 seconds", "give a list such as [0, 0.1, 0.2]"));
            return false;
        }
        for (const Json& t : args["times"]) {
            if (!t.is_number() || t.get<double>() < 0.0 || t.get<double>() > kMaxDuration) {
                problems.push_back(problem("bad_value", "times", "each time is seconds from 0 to 60", "fix the list"));
                return false;
            }
            times.push_back(t.get<double>());
        }
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end()), times.end());
        if (!args.contains("duration")) duration = std::max(times.back(), 1.0 / 60.0);
        if (times.back() > duration + 1e-9) {
            problems.push_back(problem("bad_value", "times", "a time is past the duration", "raise duration or drop the late times"));
            return false;
        }
    } else {
        int frames = 8;
        if (!readInt(args, "frames", 1, kMaxFrames, frames, problems)) return false;
        times = efx::sheetTimes(frames, duration, loop);
    }
    a.capture.times = times;
    a.capture.duration = duration;
    a.capture.loop = loop;
    a.capture.project = project;
    a.capture.pixelsPerUnit = std::max(1, (int)project->getPixelsPerUnit());

    // ---- sheet
    int margin = 24, scale = 0, columns = 0, maxWidth = 1024;
    if (!readInt(args, "margin", 0, 256, margin, problems) || !readInt(args, "scale", 0, 16, scale, problems) ||
        !readInt(args, "columns", 0, kMaxFrames, columns, problems) || !readInt(args, "max_width", 64, 8192, maxWidth, problems)) {
        return false;
    }
    a.capture.margin = margin;
    a.sheet.scale = scale;
    a.sheet.columns = columns;
    a.sheet.maxWidth = maxWidth;
    a.sheet.labels = args.value("labels", true);
    if (args.contains("backdrops") && !args["backdrops"].is_null()) {
        if (!args["backdrops"].is_array() || args["backdrops"].empty() || args["backdrops"].size() > 7) {
            problems.push_back(problem("bad_value", "backdrops", "backdrops must list 1 to 7 of: " + efx::backdropNames(), "give e.g. [\"dark\", \"light\"]"));
            return false;
        }
        a.sheet.backdrops.clear();
        for (const Json& b : args["backdrops"]) {
            efx::Backdrop backdrop;
            if (!b.is_string() || !efx::parseBackdrop(b.get<std::string>(), backdrop)) {
                problems.push_back(problem("bad_value", "backdrops", (b.is_string() ? "\"" + b.get<std::string>() + "\"" : std::string("a value")) +
                                           " is not a backdrop", "use: " + efx::backdropNames()));
                return false;
            }
            a.sheet.backdrops.push_back(backdrop);
        }
    }

    // ---- lints
    if (args.contains("lint") && !args["lint"].is_null()) {
        const Json& l = args["lint"];
        if (l.is_boolean()) {
            a.capture.lints = l.get<bool>();
        } else if (l.is_array() && !l.empty()) {
            a.capture.lints = true;
            for (const Json& id : l) {
                if (!id.is_string() || !lint::isEffectLintId(id.get<std::string>())) {
                    std::string all;
                    for (const std::string& known : lint::effectLintIds()) all += (all.empty() ? "" : ", ") + known;
                    problems.push_back(problem("unknown_lint", "lint", (id.is_string() ? "\"" + id.get<std::string>() + "\"" : std::string("a value")) +
                                               " is not a lint", "use true or names from: " + all));
                    return false;
                }
                a.capture.lintIds.push_back(id.get<std::string>());
            }
        } else {
            problems.push_back(problem("bad_value", "lint", "lint is true, false or a list of lint names", "give lint: true"));
            return false;
        }
    }

    // ---- output
    a.write = args.value("output", false);
    if (args.contains("path") && !args["path"].is_null()) {
        a.path = args["path"].is_string() ? args["path"].get<std::string>() : std::string();
        if (fs::path(a.path).extension() != ".png") {
            problems.push_back(problem("bad_path", "path", "path must be a project-relative .png", "give e.g. assets/sheets/hit.png"));
            return false;
        }
        if (!McpRefs::resolveProjectWritePath(project, a.path, a.absolute, error, false)) {
            problems.push_back(problem("bad_path", "path", error, "give a .png path inside the project"));
            return false;
        }
    }
    if (a.write && a.path.empty()) {
        problems.push_back(problem("missing_path", "path", "output: true needs path", "add path: \"<project-relative .png>\""));
        return false;
    }
    return true;
}

Json bboxJson(const epx::PixelImage& frame, bool opaque) {
    if (opaque) return Json::array({0, 0, frame.width(), frame.height()});
    const epx::IntRect r = frame.opaqueBounds(0);
    if (r.empty()) return nullptr;
    return Json::array({r.x, r.y, r.w, r.h});
}

ToolResult finishCapture(McpContext& ctx, const std::shared_ptr<Args>& a, const efx::EffectCapture& run, const std::optional<UndoOrigin>& origin) {
    const efx::CaptureOutput& out = run.getOutput();
    efx::SheetLayout layout;
    const epx::PixelImage sheet = efx::composeCapture(out, a->sheet, &layout);
    const std::string png = sheet.encodePng();

    Json data;
    data["target"] = run.getTarget().label();
    data["mode"] = out.opaque ? "scene" : "isolated";
    data["duration"] = std::round(a->capture.duration * 1000.0) / 1000.0;
    data["loop"] = a->capture.loop;
    data["scale"] = layout.scale;
    data["columns"] = layout.columns;
    data["cell"] = Json::array({out.width, out.height});
    data["size"] = Json::array({sheet.width(), sheet.height()});
    Json bands = Json::array();
    if (out.opaque) bands.push_back("scene");
    else for (efx::Backdrop b : a->sheet.backdrops) bands.push_back(efx::backdropName(b));
    data["backdrops"] = bands;
    Json frameList = Json::array();
    for (size_t i = 0; i < out.frames.size(); i++) {
        frameList.push_back({{"index", i}, {"t", std::round(out.times[i] * 1e6) / 1e6}, {"bbox", bboxJson(out.frames[i], out.opaque)}});
    }
    data["frames"] = frameList;
    if (!out.opaque) data["bounds"] = Json::array({out.bounds.x, out.bounds.y, out.bounds.w, out.bounds.h});

    std::string text = "Captured " + std::to_string(out.frames.size()) + " frames of " + run.getTarget().label() + " (" +
                       data["mode"].get<std::string>() + ") at scale " + std::to_string(layout.scale) + ": a " + std::to_string(sheet.width()) + "x" +
                       std::to_string(sheet.height()) + " sheet of " + std::to_string(out.width) + "x" + std::to_string(out.height) + " frames.";
    std::string next;
    if (a->capture.lints) {
        const lint::LintReport report = efx::lintCapture(out, run.getSettings());
        Json rows = lint::toCheckRows(report, out.times);
        const bool stop = qa::countRounds(rows, failRounds().previous(a->scope), kFixRounds);
        failRounds().record(a->scope, rows);
        const std::string status = qa::verdict(rows);
        data["checks"] = rows;
        data["check_status"] = status;
        data["verdict"] = epx::verdictName(lint::effectVerdict(report));
        qa::ThresholdAudit audit;
        audit.add("spill.margin", a->capture.margin, 24);
        audit.add("flicker.share", 0.06, 0.06);
        audit.add("off_palette.fail_share", 0.01, 0.01);
        audit.add("frame_cost.warn_ms", 1.0, 1.0);
        audit.add("frame_cost.fail_ms", 4.0, 4.0);
        audit.add("hit_stop_length.warn_s", 0.3, 0.3);
        data["thresholds"] = audit.json();
        text += " Checks " + status + ", verdict " + data["verdict"].get<std::string>() + ":";
        for (const Json& row : rows) text += "\n" + qa::rowLine(row);
        next = qa::guidance(stop, status == "FAIL", kFixRounds, "capture again with lint", kClosing);
        if (!stop && status == "WARN") next = "Fix what each WARN's fix_hint says, then capture again with lint. " + next;
    }
    if (a->write) {
        McpWriteScope writeScope;
        std::optional<UndoOriginScope> attribution;
        if (origin) attribution.emplace(*origin);
        std::string error;
        if (!w4::writeFilesUndoable(ctx, {{a->absolute, png}}, error)) {
            return ToolResult::error("The sheet was captured, nothing was written: " + error + " Fix: check the folder is writable, then call again.",
                                     Json{{"errors", Json::array({{{"kind", "write_failed"}, {"field", "path"}, {"message", error}, {"fix", "check the folder is writable"}}})}});
        }
        CommandHistory* history = ctx.project()->getProjectCommandHistory();
        const UndoEntry* top = history ? history->top() : nullptr;
        data["path"] = a->path;
        data["undo"] = top ? Json{{"id", top->info.id}, {"name", top->name()}} : Json{{"name", "Write Files"}};
        text += " Wrote " + a->path + " as one undo step (undo with scope project).";
    } else {
        next = "Preview only: the same arguments plus output: true and path write the PNG (one undo). " + next;
    }
    if (!next.empty()) {
        data["next"] = next;
        text += "\n" + next;
    }
    ToolResult reply = ToolResult::success(text, data);
    reply.images.push_back(ToolImage{Base64::encode(reinterpret_cast<const unsigned char*>(png.data()), png.size()), "image/png"});
    return reply;
}

void captureContactSheetTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    auto a = std::make_shared<Args>();
    std::vector<Problem> problems;
    if (!parseArgs(call, *a, problems)) {
        failCall(call, "Can't capture the contact sheet:", problems);
        return;
    }
    auto run = std::make_shared<efx::EffectCapture>(a->capture, std::move(a->target));
    activeCaptures().push_back(run);
    JobPtr job = ctx.jobs().create("capture_contact_sheet", "Contact sheet of " + run->getTarget().label() + (a->write ? "" : " (preview)"));
    auto reply = std::make_shared<ToolResult>();
    std::weak_ptr<efx::EffectCapture> weak = run;
    job->setCancelHandler([weak]() {
        if (auto r = weak.lock()) r->cancel();
    });
    job->setProgress(0.0, "capturing");
    const std::optional<UndoOrigin> origin = call->attributed() ? std::optional<UndoOrigin>(call->getUndoOrigin()) : std::nullopt;
    McpContext* context = &ctx;
    run->setOnFinished([weak, job, reply, a, origin, context]() {
        auto r = weak.lock();
        if (!r) return;
        if (r->isFailed()) {
            if (job->cancelRequested()) job->markCancelled("Cancelled; nothing was written.");
            else job->fail("The contact sheet failed, nothing was written: " + r->getError());
        } else {
            try {
                *reply = finishCapture(*context, a, *r, origin);
            } catch (const std::exception& e) {
                *reply = ToolResult::error(std::string("The contact sheet failed, nothing was written: ") + e.what());
            }
            Json result = reply->structured.is_object() ? reply->structured : Json::object();
            if (reply->isError) job->fail(reply->text, result);
            else job->succeed(reply->text, result);
        }
        getEditorHost().enqueueMainThreadTask([weak]() {
            auto& runs = activeCaptures();
            runs.erase(std::remove_if(runs.begin(), runs.end(), [&](const std::shared_ptr<efx::EffectCapture>& p) { return p == weak.lock(); }), runs.end());
        });
    });
    run->start();
    call->detach();
    replyWhenJobDone(call, job, true, kJobWaitSeconds, [reply](const McpJob& finished) {
        Json result = finished.result();
        result["job_id"] = finished.id();
        result["status"] = jobStatusName(finished.status());
        if (finished.status() != JobStatus::Succeeded) {
            return ToolResult::error(reply->text.empty() ? finished.message() : reply->text, result);
        }
        ToolResult out = *reply;
        out.structured = result;
        return out;
    });
}

} // namespace
} // namespace pixelfxtools

void registerPixelFxTools(McpRegistry& registry) {
    using namespace schema;
    ToolDef tool;
    tool.name = "capture_contact_sheet";
    tool.title = "Capture Contact Sheet";
    tool.description =
        "Draws a GameObject (its animations, particles, material time; animation: an .anim on it) or a .particles asset at N times "
        "into one labelled PNG at an integer scale, a row of frames per backdrop. isolated (default): the target alone, pinned; "
        "scene: through its camera (in Play, steps the paused game). lint: effect checks and a verdict. A preview until output: true "
        "writes path (one undo).";
    tool.inputSchema = object({
        {"target", object({{"entity", Json::object()}, {"scene", Json::object()}, {"animation", string("")}, {"particles", string("")}})},
        {"frames", integer("", 1, kMaxFrames)},
        {"times", array(number(""), "")},
        {"duration", number("", 0, kMaxDuration)},
        {"loop", boolean("")},
        {"mode", stringEnum("", {"isolated", "scene"})},
        {"backdrops", array(stringEnum("", {"checker", "dark", "light", "sky", "ground", "black", "scene"}), "")},
        {"scale", integer("", 0, 16)},
        {"columns", integer("", 0, kMaxFrames)},
        {"labels", boolean("")},
        {"margin", integer("", 0, 256)},
        {"lint", Json{{"type", Json::array({"boolean", "array"})}, {"items", {{"type", "string"}}}}},
        {"output", boolean("")},
        {"path", string("")},
        {"max_width", integer("", 64, 8192)}
    }, {"target"});
    tool.annotations.readOnly = true;
    tool.annotations.idempotent = true;
    tool.writesFiles = [](const Json& args) { return args.is_object() && args.value("output", false); };
    // scene mode needs Play; a preview never edits the game
    tool.allowedWhilePlaying = true;
    tool.handler = pixelfxtools::captureContactSheetTool;
    registry.addTool(std::move(tool));
}

}
