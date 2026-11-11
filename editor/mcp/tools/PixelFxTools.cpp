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
#include "animation/AnimationClipIO.h"
#include "pixelart/PixelFxPlayer.h"
#include "pixelart/ScreenTransition.h"
#include "util/Color.h"
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

// the job of the capture this tool started and that has not ended, for the "already running" error
std::string& runningJobId() {
    static std::string id;
    return id;
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

// "#rrggbb" or "#rrggbbaa" as display sRGB 0..1
bool parseSheetHex(const Json& value, Vector4& out) {
    if (!value.is_string()) return false;
    const std::string text = value.get<std::string>();
    if (text.size() != 7 && text.size() != 9) return false;
    if (text[0] != '#') return false;
    float c[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    for (size_t i = 1, k = 0; i + 1 < text.size(); i += 2, k++) {
        int v = 0;
        for (size_t j = i; j < i + 2; j++) {
            const char ch = text[j];
            const int d = ch >= '0' && ch <= '9' ? ch - '0' : (ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : (ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1));
            if (d < 0) return false;
            v = v * 16 + d;
        }
        c[k] = (float)v / 255.0f;
    }
    out = Vector4(c[0], c[1], c[2], c[3]);
    return true;
}

// target.effect_options: {duration, ease, color, count, ramp, hold}
bool parseEffectOptions(const Json& options, efx::CaptureEffect& effect, std::vector<Problem>& problems) {
    if (options.is_null()) return true;
    if (!options.is_object()) {
        problems.push_back(problem("bad_value", "target.effect_options", "effect_options must be an object", "give e.g. {\"duration\": 0.6}"));
        return false;
    }
    for (auto it = options.begin(); it != options.end(); ++it) {
        const std::string& key = it.key();
        const Json& v = it.value();
        const std::string field = "target.effect_options." + key;
        if (key == "duration" && v.is_number() && v.get<double>() > 0.0 && v.get<double>() <= kMaxDuration) {
            effect.duration = v.get<float>();
        } else if (key == "ease" && v.is_string() && ::jasperengine::anim::easeFromName(v.get<std::string>(), effect.options.ease)) {
        } else if (key == "color" && parseSheetHex(v, effect.options.color)) {
            effect.options.color = Color::sRGBToLinear(effect.options.color);
            effect.options.hasColor = true;
        } else if (key == "count" && v.is_number_integer() && v.get<int>() >= 1 && v.get<int>() <= 64) {
            effect.options.count = v.get<int>();
        } else if (key == "ramp" && v.is_string()) {
            effect.options.ramp = v.get<std::string>();
        } else if (key == "hold" && v.is_number() && v.get<double>() >= 0.0) {
            effect.options.hold = v.get<float>();
        } else {
            problems.push_back(problem("bad_value", field, key + " is not an effect option or its value is out of range",
                                       "use duration (seconds), ease (linear, quad_out, ...), color (#rrggbb), count (1-64), ramp (a ramp strip), hold (seconds)"));
            return false;
        }
    }
    return true;
}

// target.transition_options: Transition.play's options (snake_case), phase, width and height
bool parseTransitionOptions(const Json& options, jasperengine::pixelart::TransitionOptions& out, efx::CaptureTransitionPhase& phase,
                            int& width, int& height, std::vector<Problem>& problems) {
    using namespace jasperengine::pixelart;
    if (options.is_null()) return true;
    if (!options.is_object()) {
        problems.push_back(problem("bad_value", "target.transition_options", "transition_options must be an object", "give e.g. {\"shape\": \"diamond\"}"));
        return false;
    }
    for (auto it = options.begin(); it != options.end(); ++it) {
        const std::string& key = it.key();
        const Json& v = it.value();
        bool ok = true;
        if (key == "color") ok = parseSheetHex(v, out.ink);
        else if (key == "palette" && v.is_string()) out.palette = v.get<std::string>();
        else if (key == "ramp" && v.is_string()) out.ramp = v.get<std::string>();
        else if (key == "rim" && v.is_number_integer() && v.get<int>() >= 0 && v.get<int>() <= kTransitionMaxRim) out.rim = v.get<int>();
        else if (key == "glow" && v.is_number() && v.get<double>() >= 0.0 && v.get<double>() <= 1.0) out.glow = v.get<float>();
        else if (key == "pixel_size" && v.is_number_integer() && v.get<int>() >= 0 && v.get<int>() <= 16) out.pixelSize = v.get<int>();
        else if (key == "seed" && v.is_number_integer() && v.get<long long>() >= 0) out.seed = (uint32_t)v.get<long long>();
        else if (key == "ease" && v.is_string()) ok = transitionEaseFromName(v.get<std::string>(), out.ease);
        else if (key == "hold" && v.is_number() && v.get<double>() >= 0.0) out.hold = v.get<float>();
        else if (key == "shape" && v.is_string()) ok = irisShapeFromName(v.get<std::string>(), out.shape);
        else if (key == "center" && v.is_array() && v.size() == 2 && v[0].is_number() && v[1].is_number()) out.center = Vector2(v[0].get<float>(), v[1].get<float>());
        else if (key == "angle" && v.is_number()) out.angle = v.get<float>();
        else if (key == "count" && v.is_number_integer() && v.get<int>() >= 1 && v.get<int>() <= 64) out.count = v.get<int>();
        else if (key == "vertical" && v.is_boolean()) out.vertical = v.get<bool>();
        else if (key == "cell" && v.is_number_integer() && v.get<int>() >= 1 && v.get<int>() <= 256) out.cell = v.get<int>();
        else if (key == "through_ramp" && v.is_boolean()) out.throughRamp = v.get<bool>();
        else if (key == "phase" && v.is_string() && (v == "play" || v == "cover" || v == "reveal")) {
            phase = v == "cover" ? efx::CaptureTransitionPhase::Cover : (v == "reveal" ? efx::CaptureTransitionPhase::Reveal : efx::CaptureTransitionPhase::Play);
        }
        else if (key == "width" && v.is_number_integer() && v.get<int>() >= 8 && v.get<int>() <= 1024) width = v.get<int>();
        else if (key == "height" && v.is_number_integer() && v.get<int>() >= 8 && v.get<int>() <= 1024) height = v.get<int>();
        else ok = false;
        if (!ok) {
            problems.push_back(problem("bad_value", "target.transition_options." + key, key + " is not a transition option or its value is out of range",
                                       "use Transition.play's options in snake_case (color #rrggbb, palette, ramp, rim 0-8, glow 0-1, pixel_size, seed, "
                                       "ease, hold, shape, center [x, y], angle, count, vertical, cell, through_ramp), phase (play, cover, reveal), "
                                       "width and height (art pixels, 8-1024)"));
            return false;
        }
    }
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
        if (it.key() != "entity" && it.key() != "scene" && it.key() != "animation" && it.key() != "particles" && it.key() != "effect" &&
            it.key() != "effect_options" && it.key() != "transition" && it.key() != "transition_options") {
            problems.push_back(problem("unknown_key", "target." + it.key(), "target has no key " + it.key(),
                                       "use entity (with scene, animation, effect and effect_options), particles, or transition (with transition_options)"));
            return false;
        }
    }
    const bool hasEntity = target.contains("entity"), hasParticles = target.contains("particles"), hasTransition = target.contains("transition");
    if ((int)hasEntity + (int)hasParticles + (int)hasTransition != 1) {
        problems.push_back(problem("bad_target", "target", "target needs exactly one of entity, particles or transition",
                                   "give target {entity: ...} for a GameObject, target {particles: \"assets/fx/x.particles\"}, or target {transition: \"iris\"}"));
        return false;
    }
    if (!hasEntity && (target.contains("effect") || target.contains("effect_options"))) {
        problems.push_back(problem("bad_target", "target.effect", "an effect plays on a GameObject", "give target {entity: ..., effect: \"dissolve\"}"));
        return false;
    }
    if (!hasTransition && target.contains("transition_options")) {
        problems.push_back(problem("bad_target", "target.transition_options", "transition_options go with target.transition", "add transition: \"iris\""));
        return false;
    }
    std::string error;
    if (hasTransition) {
        if (target.contains("animation") || target.contains("scene")) {
            problems.push_back(problem("bad_target", "target.transition", "a transition target takes no scene or animation", "drop them"));
            return false;
        }
        jasperengine::pixelart::TransitionKind kind = jasperengine::pixelart::TransitionKind::IRIS;
        if (!target["transition"].is_string() || !jasperengine::pixelart::transitionKindFromName(target["transition"].get<std::string>(), kind)) {
            problems.push_back(problem("bad_value", "target.transition", "transition must be a kind",
                                       "use iris, blinds, slash, pixel_dissolve, crt_off, burn_away or fade"));
            return false;
        }
        jasperengine::pixelart::TransitionOptions options = jasperengine::pixelart::TransitionOptions::fromDefaults();
        efx::CaptureTransitionPhase phase = efx::CaptureTransitionPhase::Play;
        int width = 160, height = 90;
        if (!parseTransitionOptions(target.value("transition_options", Json()), options, phase, width, height, problems)) return false;
        // no Pixel Perfect Camera draws a capture: pixel size 0 is one screen pixel per art pixel
        options.pixelSize = std::max(options.pixelSize, 1);
        const double duration = args.contains("duration") && args["duration"].is_number() ? args["duration"].get<double>()
                                                                                            : (double)jasperengine::pixelart::ScreenTransition::getDefaults().duration;
        a.target = efx::makeTransitionTarget(kind, options, phase, duration, width, height);
        a.scope = std::string("transition:") + jasperengine::pixelart::transitionKindName(kind);
    } else if (hasParticles) {
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
        efx::CaptureEffect effect;
        if (target.contains("effect")) {
            if (!target["effect"].is_string() || !jasperengine::pixelart::pixelFxEffectFromName(target["effect"].get<std::string>(), effect.effect)) {
                problems.push_back(problem("bad_value", "target.effect", "effect must be a PixelFX effect",
                                           "use flash, fade_out, fade_in, blink, dissolve, undissolve, petrify, freeze, poison or outline_pulse"));
                return false;
            }
            effect.active = true;
            if (!parseEffectOptions(target.value("effect_options", Json()), effect, problems)) return false;
        } else if (target.contains("effect_options")) {
            problems.push_back(problem("bad_target", "target.effect_options", "effect_options go with target.effect", "add effect: \"dissolve\""));
            return false;
        }
        a.target = efx::makeEntityTarget(project, scene, entity, clip, error, effect);
        a.scope = scene->name + "/" + McpRefs::entityPath(scene, entity) + (clip.empty() ? "" : "#" + clip) +
                  (effect.active ? std::string("@") + jasperengine::pixelart::pixelFxEffectName(effect.effect) : std::string());
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
            problems.push_back(problem("bad_mode", "mode", "a .particles asset or a transition has no scene to show",
                                       "capture it isolated, or a GameObject playing it in scene mode"));
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
    std::string tooLarge;
    if (!efx::captureFitsLimits(*a.target, (int)times.size(), tooLarge)) {
        problems.push_back(problem("capture_too_large", "target.transition_options", tooLarge,
                                   "lower target.transition_options' width, height or pixel_size, or ask for fewer frames"));
        return false;
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
        const bool reloaded = a->capture.sceneId != 0 &&
                              (a->capture.lintIds.empty() || std::find(a->capture.lintIds.begin(), a->capture.lintIds.end(), "determinism") != a->capture.lintIds.end());
        if (reloaded) next = "The determinism check reloaded the played scene for its two fresh captures, so the game's state restarted. " + next;
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
    if (efx::EffectCapture::anyRunning()) {
        const std::string jobId = runningJobId();
        problems.push_back(problem("capture_running", "", "another capture is still running" + (jobId.empty() ? std::string(" (in the Contact Sheet window)") : " (job " + jobId + ")"),
                                   jobId.empty() ? std::string("wait for it to end (the Contact Sheet window, or get_job), then call again")
                                                 : "wait for job " + jobId + " with get_job, or cancel_job it, then call again"));
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
    runningJobId() = job->id();
    const std::optional<UndoOrigin> origin = call->attributed() ? std::optional<UndoOrigin>(call->getUndoOrigin()) : std::nullopt;
    McpContext* context = &ctx;
    run->setOnFinished([weak, job, reply, a, origin, context]() {
        runningJobId().clear();
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
        "Draws a GameObject (animation: an .anim on it; effect: a PixelFX effect), a .particles asset or a screen transition at N "
        "times into one labelled PNG, a row of frames per backdrop. isolated (default): the target alone; scene: through its camera "
        "(in Play, steps the paused game). lint: checks (spill, one_shot_end, loop_seam, determinism, flicker, off_palette, "
        "transition_edges...) and a verdict. A preview until output: true writes path (one undo).";
    tool.inputSchema = object({
        {"target", object({{"entity", entityRef()}, {"scene", sceneRef()}, {"animation", string("")}, {"effect", string("")},
                           {"effect_options", Json{{"type", "object"}}}, {"particles", string("")}, {"transition", string("")},
                           {"transition_options", Json{{"type", "object"}}}})},
        {"frames", integer("")},
        {"times", array(number(""), "")},
        {"duration", number("")},
        {"loop", boolean("")},
        {"mode", stringEnum("", {"isolated", "scene"})},
        {"backdrops", array(stringEnum("", {"checker", "dark", "light", "sky", "ground", "black", "scene"}), "")},
        {"scale", integer("")},
        {"columns", integer("")},
        {"labels", boolean("")},
        {"margin", integer("")},
        {"lint", Json{{"type", Json::array({"boolean", "array"})}, {"items", {{"type", "string"}}}}},
        {"output", boolean("")},
        {"path", string("")},
        {"max_width", integer("")}
    }, {"target"});
    // not readOnly or idempotent: output: true writes a file, and determinism in Play reloads the played scene
    tool.activity = ToolActivity::Read;
    tool.writesFiles = [](const Json& args) { return args.is_object() && args.value("output", false); };
    // scene mode needs Play; a preview never edits the game
    tool.allowedWhilePlaying = true;
    tool.handler = pixelfxtools::captureContactSheetTool;
    registry.addTool(std::move(tool));
}

}
