// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Clip quality (S74 R51, R52, D6): check_animation, what a clip costs and how good it is, and
// optimize_animation, the key reduction with the bake_vertex_animation shape: without output
// nothing is written, the same arguments + output write exactly that. Both print the structs of
// animation/ClipReport and animation/ClipOptimize and calculate nothing.
//
// Both are jobs. A worker thread does the reading and the measuring; the main thread resolves the
// arguments before it (a GameObject's rig reads the scene) and writes after it (a command history
// step), then a second worker reads the result back from disk, so the error reported after a write
// is the one the file has.
//
// check_animation's checks are one table, kChecks. foot_sliding and transition measure clips
// (animation/ClipReport); record_seconds watches a playing Animator through makeSnapshot
// (animation/AnimatorDebug) on the main thread, a frame at a time.
#include "McpTools.h"

#include "W4Shared.h"
#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"

#include "App.h"
#include "EditorHost.h"
#include "Project.h"
#include "Scene.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/AnimatorDebug.h"
#include "animation/AnimatorInstance.h"
#include "animation/AnimatorAssetIO.h"
#include "animation/ClipConverters.h"
#include "animation/ClipOptimize.h"
#include "animation/ClipReduction.h"
#include "animation/ClipReport.h"
#include "animation/ClipStatistics.h"
#include "animation/ControllerDef.h"
#include "animation/ModelSubClips.h"
#include "command/type/ClipSceneActions.h"
#include "component/AnimationPlayerComponent.h"
#include "component/AnimatorComponent.h"
#include "component/Transform.h"
#include "io/FileData.h"
#include "pool/AnimationClipPool.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace jasperengine::editor::mcp {

namespace fs = std::filesystem;

namespace {

constexpr double LOOP_SEAM_LIMIT_MM = anim::kClipLoopSeamLimitMm;
constexpr double ROOT_DRIFT_LIMIT_MM = anim::kClipRootDriftLimitMm;
constexpr double IN_PLACE_SPEED_MPS = anim::kClipInPlaceSpeedMps;
constexpr double FOOT_SLIDE_LIMIT_MM = anim::kClipFootSlideLimitMm;
// a joint stepping more than this in 1/60 s through a crossfade, and over twice its step around it, pops
constexpr double POP_LIMIT_MM = 30.0;
constexpr double POP_RATIO = 2.0;
constexpr size_t DEFAULT_TOP = 20;
constexpr size_t COMPARE_DEFAULT_SKIN_MM = 100;

// A clip as the tools name it and as the pool takes it
struct NamedClip {
    std::string display;        // project-relative: "assets/anim/walk.anim", "assets/models/hero.glb#Walk"
    std::string reference;      // absolute, what the pool and the readers take
    bool model = false;         // a bare model: all of its clips
};

std::string lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return text;
}

bool resolveNamedClip(Project* project, const std::string& given, NamedClip& out, std::string& error) {
    anim::SubClipReference parts;
    const bool subClip = anim::parseSubClipReference(given, parts);
    const std::string file = subClip ? parts.model : given;
    const bool isModel = anim::isModelFilePath(file);
    if (!isModel && lowered(FileUtils::pathFromUtf8(file).extension().string()) != ".anim") {
        error = "\"" + given + "\" is neither an .anim nor a model (.gltf, .glb, .fbx) nor \"<model>#<clip>\".";
        return false;
    }
    fs::path absolute;
    if (!McpRefs::resolveProjectPath(project, file, absolute, error, true)) return false;
    absolute = absolute.lexically_normal();
    const std::string relative = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(absolute));
    out.display = subClip ? relative + "#" + parts.clip : relative;
    out.reference = FileUtils::pathToGenericUtf8(absolute) + (subClip ? "#" + parts.clip : std::string());
    out.model = isModel && !subClip;
    return true;
}

// A model's clips, by name
bool expandModel(const NamedClip& model, std::vector<NamedClip>& out, std::string& error) {
    std::vector<anim::ModelClipInfo> clips;
    if (!anim::listModelFileClips(model.reference, clips, &error)) return false;
    for (const anim::ModelClipInfo& clip : clips) {
        out.push_back({model.display + "#" + clip.name, anim::formatSubClipReference(model.reference, clip.name), false});
    }
    return true;
}

// A GameObject's hierarchy as a rig: its rest is what the Transforms hold now
bool rigFromEntity(SceneProject* sceneProject, Entity root, anim::ClipRig& rig, std::string& error) {
    Scene* scene = sceneProject->scene;
    std::map<Entity, int> index;
    std::vector<Entity> members;
    for (Entity entity : sceneProject->entities) {
        // on or under the root
        for (const Transform* at = scene->findComponent<Transform>(entity); at != nullptr;
             at = at->parent == NULL_ENTITY ? nullptr : scene->findComponent<Transform>(at->parent)) {
            if (entity != root && at->parent != root) continue;
            index[entity] = (int)members.size();
            members.push_back(entity);
            break;
        }
    }
    if (index.find(root) == index.end()) {
        error = "the GameObject has no Transform";
        return false;
    }
    std::vector<anim::ModelNodeInfo> nodes;
    for (Entity entity : members) {
        const Transform* transform = scene->findComponent<Transform>(entity);
        anim::ModelNodeInfo node;
        node.name = scene->getEntityName(entity);
        if (entity != root) {
            const auto parent = index.find(transform->parent);
            node.parent = parent == index.end() ? -1 : parent->second;
        }
        node.addressable = ClipConverters::clipPath(scene, root, entity, node.path);
        node.position = transform->position;
        node.rotation = transform->rotation;
        node.scale = transform->scale;
        nodes.push_back(std::move(node));
    }
    rig.assign(scene->getEntityName(root), nodes);
    return true;
}

// The rigs of one job, each model read once
class RigCache {
public:
    std::shared_ptr<const anim::ClipRig> entity;    // `entity` / `on`: wins over what a clip names

    std::shared_ptr<const anim::ClipRig> get(const std::string& model, std::string& error) {
        const auto found = rigs.find(model);
        if (found != rigs.end()) return found->second;
        auto rig = std::make_shared<anim::ClipRig>();
        if (!anim::clipRigFromModel(model, *rig, &error)) return nullptr;
        rigs[model] = rig;
        return rig;
    }

private:
    std::map<std::string, std::shared_ptr<const anim::ClipRig>> rigs;
};

// The value form of an .anim, for what names a rig and its record; a sub-clip has neither
bool readClipData(const std::string& reference, ClipData& out, std::string& error) {
    if (anim::isSubClipReference(reference)) return true;
    std::ifstream stream(FileUtils::pathFromUtf8(reference), std::ios::binary);
    if (!stream) {
        error = "can't read the file";
        return false;
    }
    std::ostringstream text;
    text << stream.rdbuf();
    anim::ClipParseResult parsed = anim::parseClip(text.str());
    if (!parsed.ok()) {
        error = parsed.error.toString();
        return false;
    }
    out = std::move(*parsed.data);
    return true;
}

// The rig a clip measures on; null + a fix-it error when it names none: never a flat fallback
std::shared_ptr<const anim::ClipRig> rigFor(RigCache& rigs, const std::string& reference, const ClipData& data, const char* argument,
                                            std::string& error) {
    if (rigs.entity) return rigs.entity;
    const std::string model = anim::clipRigModel(reference, data);
    if (model.empty()) {
        error = std::string("no_rig: the clip names no model to measure on. Fix: pass ") + argument +
                " (the GameObject that plays it), or set the clip's Preview Model (the Inspector, or `preview` in its file).";
        return nullptr;
    }
    return rigs.get(model, error);
}

ProjectChecks::Finding finding(const std::string& path, const std::string& severity, const std::string& code, const std::string& message,
                               const std::string& suggestion) {
    ProjectChecks::Finding out;
    out.tool = "anim";
    out.path = path;
    out.severity = severity;
    out.code = code;
    out.message = message;
    out.suggestion = suggestion;
    return out;
}

std::string leafName(const std::string& display) {
    anim::SubClipReference parts;
    if (anim::parseSubClipReference(display, parts)) return parts.clip;
    return FileUtils::pathToUtf8(FileUtils::pathFromUtf8(display).stem());
}

// a rig's source as the tools name files: under the project when it is
std::string rigName(const fs::path& projectRoot, const std::string& source) {
    const fs::path path = FileUtils::pathFromUtf8(source);
    if (!path.is_absolute()) return source;
    const std::string relative = FileUtils::pathToGenericUtf8(path.lexically_relative(projectRoot));
    return relative.empty() || relative.rfind("..", 0) == 0 ? source : relative;
}

// ---------------------------------------------------------------- check_animation

struct CheckLimits {
    std::optional<double> errorMm;
    double rootDriftMm = ROOT_DRIFT_LIMIT_MM;
    bool rootDriftGiven = false;
    double footSlideMm = FOOT_SLIDE_LIMIT_MM;
    double popMm = POP_LIMIT_MM;
};

// `transition`: a crossfade of the GameObject's states, its clips resolved on the main thread
struct CheckTransition {
    std::string from;
    std::string to;
    NamedClip toClip;
    std::optional<float> duration;          // seconds (none: the controller's, as a length of `from` when not fixed)
    std::optional<float> durationOfFrom;    // the controller's duration in lengths of `from`
    std::optional<float> at;                // seconds into `from`
    std::optional<float> atOfFrom;          // the controller's exit time
    std::string note;                       // why the controller gave no duration
};

struct CheckRequest {
    std::vector<NamedClip> clips;       // empty: the project
    fs::path projectRoot;
    std::set<std::string> checks;
    NamedClip compareTo;
    bool hasCompare = false;
    CheckLimits limits;
    size_t top = DEFAULT_TOP;
    std::shared_ptr<const anim::ClipRig> entityRig;
    std::vector<std::string> feet;
    std::optional<double> moveSpeed;
    std::optional<CheckTransition> transition;
};

// Every check the schema offers
struct CheckRow {
    const char* name;
};
const CheckRow kChecks[] = {
    {"stats"},
    {"loop_seam"},
    {"root_motion"},
    {"foot_sliding"},
    {"transition"},
};

std::vector<std::string> checkNames() {
    std::vector<std::string> names;
    for (const CheckRow& row : kChecks) names.push_back(row.name);
    return names;
}

const CheckRow* findCheck(const std::string& name) {
    for (const CheckRow& row : kChecks) {
        if (name == row.name) return &row;
    }
    return nullptr;
}

std::string formattedLimit(double value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%g", value);
    return text;
}

struct CheckOutcome {
    std::vector<ProjectChecks::Finding> findings;
    Json rig;
    uint64_t totalBytes = 0;
    size_t totalKeys = 0;
    size_t total = 0;
    std::vector<std::pair<uint64_t, Json>> entries;
    std::vector<std::pair<uint64_t, std::string>> lines;
};

// One clip's entry, findings and summary line
void checkOneClip(const CheckRequest& request, const NamedClip& clip, RigCache& rigs, const std::atomic<bool>& cancel, CheckOutcome& out) {
    anim::ClipReportRequest report;
    report.reference = clip.reference;
    report.cancel = &cancel;
    report.loopSeam = request.checks.count("loop_seam") > 0;
    report.rootMotion = request.checks.count("root_motion") > 0;
    report.footSliding = request.checks.count("foot_sliding") > 0;
    report.feet = request.feet;
    report.moveSpeedMps = request.moveSpeed;
    const bool needsRig = report.loopSeam || report.rootMotion || report.footSliding || request.transition || request.hasCompare;

    ClipData data;
    std::string error;
    std::shared_ptr<const anim::ClipRig> rig;
    std::string rigError;
    if (needsRig && readClipData(clip.reference, data, rigError)) {
        rig = rigFor(rigs, clip.reference, data, "entity", rigError);
        // a compare's rig can come from what it compares with
        if (!rig && request.hasCompare) {
            ClipData other;
            std::string ignored;
            if (readClipData(request.compareTo.reference, other, ignored)) rig = rigFor(rigs, request.compareTo.reference, other, "entity", ignored);
            if (rig) rigError.clear();
        }
    }
    report.rig = rig.get();

    std::shared_ptr<const AnimationClip> reference;
    bool referenceLoaded = false;
    if (request.hasCompare) {
        referenceLoaded = AnimationClipPool::get(request.compareTo.reference) == nullptr;
        reference = AnimationClipPool::getOrLoad(request.compareTo.reference, &error);
        if (!reference) {
            out.total++;
            out.findings.push_back(finding(clip.display, "error", "anim-compare-failed", "compare_to " + request.compareTo.display + ": " + error, "check the path"));
            return;
        }
        report.compareTo = reference;
        report.compareToName = request.compareTo.display;
        // the clip's own record says what it was held to, when nobody says otherwise
        const anim::ClipOptimizeRecord record = anim::readClipOptimizeRecord(data);
        if (request.limits.errorMm) report.toleranceMm = request.limits.errorMm;
        else if (record.present) report.toleranceMm = record.toleranceMm;
        report.skinDistanceMm = record.present && record.skinDistanceMm > 0.0 ? record.skinDistanceMm : (double)COMPARE_DEFAULT_SKIN_MM;
    }

    // a crossfade from this clip into the transition's: the controller's timing, in this clip's lengths
    std::shared_ptr<const AnimationClip> destination;
    bool destinationLoaded = false;
    bool ownLoaded = false;
    if (request.transition) {
        const CheckTransition& transition = *request.transition;
        destinationLoaded = AnimationClipPool::get(transition.toClip.reference) == nullptr;
        destination = AnimationClipPool::getOrLoad(transition.toClip.reference, &error);
        ownLoaded = AnimationClipPool::get(clip.reference) == nullptr;
        std::shared_ptr<const AnimationClip> own = AnimationClipPool::getOrLoad(clip.reference, &error);
        if (!destination || !own) {
            own.reset();
            destination.reset();
            if (ownLoaded) AnimationClipPool::releaseUnused(clip.reference);
            if (destinationLoaded) AnimationClipPool::releaseUnused(transition.toClip.reference);
            out.total++;
            out.findings.push_back(finding(clip.display, "error", "anim-unreadable", "transition: " + error, "check the states' clips"));
            return;
        }
        const float length = own->getLength();
        report.transitionTo = destination;
        report.transitionFromName = transition.from;
        report.transitionToName = transition.to;
        report.transitionDuration = transition.duration ? *transition.duration : transition.durationOfFrom ? *transition.durationOfFrom * length : 0.0f;
        report.transitionAt = transition.at ? *transition.at : transition.atOfFrom ? *transition.atOfFrom * length : 0.5f * length;
    }

    const anim::ClipReport result = anim::analyzeClip(report);
    if (destinationLoaded) {
        report.transitionTo.reset();
        destination.reset();
        AnimationClipPool::releaseUnused(request.transition->toClip.reference);
    }
    // asking does not change Clip memory: what this call loaded leaves the pool again
    if (ownLoaded) AnimationClipPool::releaseUnused(clip.reference);
    if (referenceLoaded) {
        report.compareTo.reset();
        reference.reset();
        AnimationClipPool::releaseUnused(request.compareTo.reference);
    }
    out.total++;
    if (!result.ok) {
        out.findings.push_back(finding(clip.display, "error", "anim-unreadable", result.error, "check_animation reads .anim files and model clips the project can load"));
        return;
    }
    out.totalBytes += result.runtimeBytes;
    out.totalKeys += result.keys;

    Json entry = anim::clipReportJson(result);
    entry["clip"] = clip.display;
    if (rig) {
        entry["rig"] = rigName(request.projectRoot, rig->source);
        if (out.rig.is_null()) out.rig = {{"model", rigName(request.projectRoot, rig->source)}, {"joints", rig->size()}};
    }
    if (needsRig && !rig) {
        entry["rig_error"] = rigError;
        out.findings.push_back(finding(clip.display, "warning", "anim-no-rig", rigError, "pass entity, or set the clip's Preview Model"));
    }

    std::string line = leafName(clip.display) + ": Memory " + anim::formatClipField("runtime_bytes", (double)result.runtimeBytes) + ", " +
                       anim::formatClipField("keys", (double)result.keys) + " keys";
    if (result.compare) {
        const anim::ClipCompareFacts& compare = *result.compare;
        if (compare.error.measured) {
            line += "; " + anim::clipErrorLine(compare.error);
            if (compare.toleranceMm) line += " (limit " + formattedLimit(*compare.toleranceMm) + ")";
            if (compare.withinTolerance && !*compare.withinTolerance) {
                out.findings.push_back(finding(clip.display, "error", "anim-compare-error",
                                               "the error against " + compare.reference + " is " + anim::formatClipField("max_error_mm", compare.error.maxErrorMm) +
                                               " at " + compare.error.worstJoint + " (limit " + formattedLimit(*compare.toleranceMm) + " mm)",
                                               "reduce again with a smaller tolerance_mm"));
            }
        } else {
            line += "; nothing to compare on this rig";
            out.findings.push_back(finding(clip.display, "error", "anim-compare-unmeasured",
                                           "no track of either clip moves a bone of " + rigName(request.projectRoot, rig->source) + ", so there is no number to trust",
                                           "pass entity: the GameObject that plays it"));
        }
        if (compare.unmeasuredDifferences > 0) {
            out.findings.push_back(finding(clip.display, "warning", "anim-compare-other-tracks",
                                           std::to_string(compare.unmeasuredDifferences) + " tracks that are not bones differ from " + compare.reference,
                                           "get_animation lists the tracks of both"));
        }
        if (!compare.eventsMatch || !compare.audioCuesMatch) {
            out.findings.push_back(finding(clip.display, "warning", "anim-compare-events",
                                           std::string(compare.eventsMatch ? "its audio cues" : compare.audioCuesMatch ? "its events" : "its events and audio cues") +
                                           " differ from " + compare.reference, "get_animation lists the events and audio cues of both"));
        }
        if (!compare.lengthMatches) {
            out.findings.push_back(finding(clip.display, "warning", "anim-compare-length", "its length differs from " + compare.reference, "get_animation shows both lengths"));
        }
    }
    if (result.loopSeam) {
        line += "; Loop Match " + anim::formatClipField("loop_seam.max_error_mm", result.loopSeam->maxErrorMm);
        // only a clip that loops has a seam to show
        if (result.wrap == ClipWrapMode::Loop && result.loopSeam->maxErrorMm > LOOP_SEAM_LIMIT_MM) {
            out.findings.push_back(finding(clip.display, "warning", "anim-loop-seam",
                                           leafName(clip.display) + " is Loop and its seam jumps " + anim::formatClipField("loop_seam.max_error_mm", result.loopSeam->maxErrorMm) +
                                           " at " + result.loopSeam->worstJoint, "make the first and last keys match, or set wrap: once"));
        }
    }
    if (result.rootMotion) {
        const anim::ClipRootMotionFacts& facts = *result.rootMotion;
        line += "; root " + anim::formatClipField("root_motion.average_speed_mps", facts.averageSpeedMps);
        const double netMm = std::hypot((double)facts.netTranslationMetres.x, (double)facts.netTranslationMetres.z) * 1000.0;
        const bool inPlace = request.limits.rootDriftGiven || facts.averageSpeedMps < IN_PLACE_SPEED_MPS;
        if (!facts.bone.empty() && inPlace && netMm > request.limits.rootDriftMm) {
            out.findings.push_back(finding(clip.display, "warning", "anim-root-drift",
                                           "the root (" + facts.bone + ") ends " + anim::formatClipField("root_motion.loop_gap_mm", netMm) + " from where it starts (limit " +
                                           formattedLimit(request.limits.rootDriftMm) + " mm)", "zero the root's keys, or use root motion"));
        }
    }
    if (result.footSliding) {
        const anim::ClipFootSliding& sliding = *result.footSliding;
        for (const std::string& unknown : sliding.unknownFeet) {
            out.findings.push_back(finding(clip.display, "warning", "anim-unknown-foot", "feet: the rig has no bone \"" + unknown + "\"",
                                           "name a foot as get_model_info lists its bones"));
        }
        if (sliding.contactSource == "none") {
            line += "; foot contact: none (pass feet)";
        } else {
            line += "; foot slide " + anim::formatClipField("foot_sliding.max_slide_mm", sliding.maxSlideMm);
            for (const anim::ClipFootSlide& foot : sliding.feet) {
                if (foot.maxSlideMm <= request.limits.footSlideMm) continue;
                char span[64];
                std::snprintf(span, sizeof(span), " (%.2f-%.2f s)", foot.worstStart, foot.worstEnd);
                out.findings.push_back(finding(clip.display, "warning", "anim-foot-slide",
                                               foot.bone + " slides " + anim::formatClipField("foot_sliding.max_slide_mm", foot.maxSlideMm) + " on the ground" + span +
                                               " (limit " + formattedLimit(request.limits.footSlideMm) + " mm)",
                                               "move the character at " + anim::formatClipField("foot_sliding.matching_ground_speed_mps", sliding.matchingGroundSpeedMps) +
                                               " (move_speed checks a speed), or add a Foot Lock IK constraint"));
            }
        }
    }
    if (result.transition) {
        const anim::ClipTransitionFacts& transition = *result.transition;
        line += "; " + transition.from + " to " + transition.to + " steps " + anim::formatClipField("transition.max_step_mm", transition.maxStepMm);
        std::string causes;
        for (const std::string& cause : transition.causes) causes += (causes.empty() ? "" : "; ") + cause;
        if (!request.transition->note.empty()) causes += (causes.empty() ? "" : "; ") + request.transition->note;
        if (transition.maxStepMm > request.limits.popMm && transition.popRatio > POP_RATIO) {
            out.findings.push_back(finding(clip.display, "warning", "anim-transition-pop",
                                           transition.from + " to " + transition.to + ": " + transition.worstJoint + " jumps " +
                                           anim::formatClipField("transition.max_step_mm", transition.maxStepMm) + " in a frame" +
                                           (transition.baselineStepMm < 0.1 ? std::string(", where it holds still around it")
                                                                            : ", " + formattedLimit(std::round(transition.popRatio * 10.0) / 10.0) + " x its step around it") +
                                           (causes.empty() ? std::string() : " (" + causes + ")"),
                                           "lengthen the transition's duration, or key those bones in both clips"));
        }
        if (!request.transition->note.empty()) entry["transition"]["causes"].push_back(request.transition->note);
    }
    out.entries.push_back({result.runtimeBytes, std::move(entry)});
    out.lines.push_back({result.runtimeBytes, line});
}

void runCheck(const CheckRequest& request, const JobPtr& job, const std::shared_ptr<std::atomic<bool>>& cancel) {
    std::vector<NamedClip> clips;
    std::vector<std::string> notes;
    if (request.clips.empty()) {
        for (const anim::ProjectClip& clip : anim::listProjectClips(request.projectRoot, notes, cancel.get())) {
            clips.push_back({clip.display, clip.reference, false});
        }
    } else {
        for (const NamedClip& clip : request.clips) {
            std::string error;
            if (!clip.model) clips.push_back(clip);
            else if (!expandModel(clip, clips, error)) notes.push_back(clip.display + ": " + error);
        }
    }

    RigCache rigs;
    rigs.entity = request.entityRig;
    CheckOutcome out;
    for (size_t i = 0; i < clips.size(); i++) {
        if (cancel->load()) {
            job->markCancelled("Cancelled; check_animation writes nothing.");
            return;
        }
        job->setProgress((double)i / (double)clips.size(), clips[i].display);
        checkOneClip(request, clips[i], rigs, *cancel, out);
    }

    // the largest first: what an agent asks a project for
    std::stable_sort(out.entries.begin(), out.entries.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::stable_sort(out.lines.begin(), out.lines.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    Json entries = Json::array();
    const size_t shown = std::min(request.top, out.entries.size());
    for (size_t i = 0; i < shown; i++) entries.push_back(std::move(out.entries[i].second));

    Json findings = Json::array();
    for (const ProjectChecks::Finding& item : out.findings) findings.push_back(ProjectChecks::toJson(item));
    Json result = {{"ok", out.findings.empty()}, {"rig", out.rig}, {"clips", entries},
                   {"totals", {{"clips", out.total}, {"runtime_bytes", out.totalBytes}, {"keys", out.totalKeys}}},
                   {"findings", findings}};
    if (!notes.empty()) result["notes"] = notes;

    std::string text;
    for (size_t i = 0; i < out.lines.size() && i < 8; i++) text += out.lines[i].second + "\n";
    if (out.lines.size() > 8) text += "(" + std::to_string(out.lines.size() - 8) + " more)\n";
    text += std::to_string(out.total) + (out.total == 1 ? " clip" : " clips") + ", Memory " +
            anim::formatClipField("runtime_bytes", (double)out.totalBytes) + "; " + std::to_string(out.findings.size()) +
            (out.findings.size() == 1 ? " finding" : " findings") + "; ok: " + (out.findings.empty() ? "true" : "false") + ".";
    job->succeed(text, result);
}

// `state` of a GameObject: its Animation Player's entry or its Animator's state
bool clipOfState(SceneProject* sceneProject, Entity entity, const std::string& state, std::string& clip, std::string& error) {
    Scene* scene = sceneProject->scene;
    if (const AnimationPlayerComponent* player = scene->findComponent<AnimationPlayerComponent>(entity)) {
        const std::vector<std::string> states = anim::playerStateNames(player->clips);
        for (size_t i = 0; i < states.size() && i < player->clips.size(); i++) {
            if (states[i] == state) {
                clip = player->clips[i].clip;
                return true;
            }
        }
    }
    if (const AnimatorComponent* animator = scene->findComponent<AnimatorComponent>(entity)) {
        std::ifstream stream(FileUtils::pathFromUtf8(FileData::getSystemPath(animator->controller.path)), std::ios::binary);
        std::ostringstream text;
        text << stream.rdbuf();
        const anim::ControllerParseResult parsed = anim::parseController(text.str());
        if (parsed.ok) {
            std::function<const anim::StateDef*(const anim::StateMachineDef&)> find = [&](const anim::StateMachineDef& machine) -> const anim::StateDef* {
                for (const anim::StateDef& candidate : machine.states) {
                    if (candidate.name == state) return &candidate;
                }
                for (const anim::StateMachineDef& child : machine.subMachines) {
                    if (const anim::StateDef* found = find(child)) return found;
                }
                return nullptr;
            };
            for (const anim::LayerDef& layer : parsed.data.layers) {
                const anim::StateDef* found = find(layer.machine);
                if (!found) continue;
                if (found->motion.clip.empty()) {
                    error = "state \"" + state + "\" plays a blend tree: name its clips in clips";
                    return false;
                }
                clip = found->motion.clip.path;
                return true;
            }
        }
    }
    error = "the GameObject has no state \"" + state + "\" (get_animation {entity} and get_animator list them)";
    return false;
}

// A path a player or controller stores (asset-relative) as a clip the tools name
bool namedFromStored(Project* project, const std::string& stored, NamedClip& out, std::string& error) {
    anim::SubClipReference parts;
    const bool subClip = anim::parseSubClipReference(stored, parts);
    const fs::path absolute = project->resolveAssetPath(FileUtils::pathFromUtf8(subClip ? parts.model : stored)).lexically_normal();
    const std::string relative = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(absolute));
    return resolveNamedClip(project, subClip ? relative + "#" + parts.clip : relative, out, error);
}

bool resolveGameObject(Project* project, const Json& args, const char* key, SceneProject*& sceneProject, Entity& entity, std::string& error) {
    sceneProject = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!sceneProject) {
        error = "scene: " + error;
        return false;
    }
    if (!sceneProject->scene) {
        error = McpRefs::notOpenError(project, *sceneProject);
        return false;
    }
    entity = McpRefs::resolveEntity(sceneProject, args[key], error);
    if (entity == NULL_ENTITY) {
        error = std::string(key) + ": " + error;
        return false;
    }
    return true;
}

ToolResult jobReply(const McpJob& finished);

// The controller's transition from `from` to `to`, when it has one: its duration and exit time
void controllerTiming(SceneProject* sceneProject, Entity entity, CheckTransition& transition) {
    const AnimatorComponent* animator = sceneProject->scene->findComponent<AnimatorComponent>(entity);
    if (!animator) return;
    std::ifstream stream(FileUtils::pathFromUtf8(FileData::getSystemPath(animator->controller.path)), std::ios::binary);
    std::ostringstream text;
    text << stream.rdbuf();
    const anim::ControllerParseResult parsed = anim::parseController(text.str());
    if (!parsed.ok) return;
    std::function<const anim::StateDef*(const anim::StateMachineDef&)> find = [&](const anim::StateMachineDef& machine) -> const anim::StateDef* {
        for (const anim::StateDef& candidate : machine.states) {
            if (candidate.name == transition.from) return &candidate;
        }
        for (const anim::StateMachineDef& child : machine.subMachines) {
            if (const anim::StateDef* found = find(child)) return found;
        }
        return nullptr;
    };
    for (const anim::LayerDef& layer : parsed.data.layers) {
        const anim::StateDef* state = find(layer.machine);
        if (!state) continue;
        for (const anim::TransitionDef& t : state->transitions) {
            if (t.to != transition.to) continue;
            if (!transition.duration) {
                if (t.fixedDuration) transition.duration = t.duration;
                else transition.durationOfFrom = t.duration;
            }
            if (!transition.at && t.hasExitTime) transition.atOfFrom = t.exitTime;
            return;
        }
    }
    if (!transition.duration) {
        transition.duration = 0.0f;
        transition.note = "the controller has no transition from " + transition.from + " to " + transition.to + ": a switch is instant";
    }
}

// ---- record_seconds: a playing Animator's legs, a frame at a time

struct RecordSample {
    float time = 0.0f;
    Vector3 position = Vector3(0, 0, 0);
    bool contact = false;
};

struct RecordCall {
    McpContext* ctx = nullptr;
    JobPtr job;
    Project* project = nullptr;
    uint32_t sceneId = 0;
    Entity entity = NULL_ENTITY;
    double seconds = 0.0;
    double footSlideMm = FOOT_SLIDE_LIMIT_MM;
    std::string display;
    std::shared_ptr<std::atomic<bool>> cancel;
    uint64_t evaluations = 0;
    float startClock = -1.0f;
    float lastClock = 0.0f;
    Vector3 startPosition = Vector3(0, 0, 0);
    Vector3 lastPosition = Vector3(0, 0, 0);
    size_t frames = 0;
    const char* contactSource = "none";
    std::vector<std::string> names;                 // per IK constraint (legs only are kept)
    std::vector<std::vector<RecordSample>> feet;
};

using RecordCallPtr = std::shared_ptr<RecordCall>;

void finishRecord(const RecordCallPtr& state) {
    const double elapsed = std::max(0.0f, state->lastClock - state->startClock);
    Json feet = Json::array();
    std::vector<ProjectChecks::Finding> findings;
    double worst = 0.0;
    for (size_t i = 0; i < state->feet.size(); i++) {
        const std::vector<RecordSample>& samples = state->feet[i];
        if (samples.empty()) continue;
        double maxSlide = 0.0;
        float worstStart = 0.0f, worstEnd = 0.0f;
        size_t onGround = 0;
        size_t start = 0;
        bool inContact = false;
        for (size_t k = 0; k < samples.size(); k++) {
            if (!samples[k].contact) {
                inContact = false;
                continue;
            }
            onGround++;
            if (!inContact) {
                inContact = true;
                start = k;
            }
            const Vector3 moved = samples[k].position - samples[start].position;
            const double mm = std::hypot((double)moved.x, (double)moved.z) * 1000.0;
            if (mm > maxSlide) {
                maxSlide = mm;
                worstStart = samples[start].time - state->startClock;
                worstEnd = samples[k].time - state->startClock;
            }
        }
        worst = std::max(worst, maxSlide);
        auto round3 = [](double v) { return std::round(v * 1000.0) / 1000.0; };
        feet.push_back({{"bone", state->names[i]}, {"contact_ratio", round3((double)onGround / (double)samples.size())}, {"max_slide_mm", round3(maxSlide)},
                        {"worst_contact", {{"start", round3(worstStart)}, {"end", round3(worstEnd)}}}});
        if (maxSlide > state->footSlideMm) {
            findings.push_back(finding(state->display, "warning", "anim-foot-slide",
                                       state->names[i] + " slides " + anim::formatClipField("foot_sliding.max_slide_mm", maxSlide) + " on the ground (limit " +
                                       formattedLimit(state->footSlideMm) + " mm)", "match the move speed to the clip, or add a Foot Lock IK constraint"));
        }
    }
    const bool judged = std::string(state->contactSource) != "none";
    const Vector3 travel = state->lastPosition - state->startPosition;
    const double speed = elapsed > 0.0 ? std::hypot((double)travel.x, (double)travel.z) / elapsed : 0.0;
    Json entry = {{"clip", state->display},
                  {"record_seconds", std::round(elapsed * 1000.0) / 1000.0},
                  {"frames", state->frames},
                  {"foot_sliding", {{"contact_source", state->contactSource},
                                    {"max_slide_mm", judged ? Json(std::round(worst * 1000.0) / 1000.0) : Json(nullptr)},
                                    {"matching_ground_speed_mps", nullptr},
                                    {"move_speed_mps", std::round(speed * 1000.0) / 1000.0},
                                    {"feet", feet}}}};
    Json findingsJson = Json::array();
    for (const ProjectChecks::Finding& item : findings) findingsJson.push_back(ProjectChecks::toJson(item));
    Json result = {{"ok", findings.empty()}, {"rig", nullptr}, {"clips", Json::array({entry})},
                   {"totals", {{"clips", 1}, {"runtime_bytes", 0}, {"keys", 0}}}, {"findings", findingsJson}};
    std::string text = state->display + ": " + std::to_string(state->frames) + " frames over " + formattedLimit(std::round(elapsed * 100.0) / 100.0) + " s; ";
    text += judged ? "foot slide " + anim::formatClipField("foot_sliding.max_slide_mm", worst) : std::string("foot contact: none (no Foot IK or Foot Lock)");
    text += "; ok: " + std::string(findings.empty() ? "true" : "false") + ".";
    state->job->succeed(text, result);
}

void recordTick(const RecordCallPtr& state) {
    if (state->cancel->load()) {
        state->job->markCancelled("Cancelled; check_animation writes nothing.");
        return;
    }
    SceneProject* sceneProject = state->project->getScene(state->sceneId);
    Scene* scene = sceneProject ? sceneProject->scene : nullptr;
    const AnimatorComponent* animator = scene && scene->isEntityCreated(state->entity) ? scene->findComponent<AnimatorComponent>(state->entity) : nullptr;
    const Transform* transform = animator ? scene->findComponent<Transform>(state->entity) : nullptr;
    if (!animator || !transform || sceneProject->playState == ScenePlayState::STOPPED) {
        state->job->fail("record_seconds: the game stopped, or the GameObject or its Animator went away, before " +
                         formattedLimit(state->seconds) + " s were recorded.");
        return;
    }
    const anim::AnimatorInstance* instance = animator->runtime.get();
    if (instance && instance->bound && instance->evaluations != state->evaluations) {
        state->evaluations = instance->evaluations;
        const anim::AnimatorDebugSnapshot snapshot = anim::makeSnapshot(*instance, *animator, *transform);
        if (snapshot.valid) {
            if (state->startClock < 0.0f) {
                state->startClock = instance->clock;
                state->startPosition = transform->worldPosition;
                state->names.resize(snapshot.ik.size());
                state->feet.resize(snapshot.ik.size());
                for (size_t i = 0; i < snapshot.ik.size(); i++) state->names[i] = snapshot.ik[i].name;
            }
            state->contactSource = snapshot.contactSource;
            state->lastClock = instance->clock;
            state->lastPosition = transform->worldPosition;
            state->frames++;
            for (size_t i = 0; i < snapshot.ik.size() && i < state->feet.size(); i++) {
                if (snapshot.ik[i].leg) state->feet[i].push_back({instance->clock, snapshot.ik[i].foot, snapshot.ik[i].contact});
            }
            const double elapsed = instance->clock - state->startClock;
            state->job->setProgress(std::min(1.0, elapsed / state->seconds), "recording");
            if (elapsed >= state->seconds) {
                finishRecord(state);
                return;
            }
        }
    }
    state->ctx->app().requestRedraw();
    getEditorHost().enqueueMainThreadTask([state]() { recordTick(state); });
}

void recordAnimator(const CallPtr& call, SceneProject* sceneProject, Entity entity, double seconds, double footSlideMm) {
    if (sceneProject->playState == ScenePlayState::STOPPED) {
        call->fail("record_seconds watches the running game: play first (play {action: \"start\"}), or check clips without it.");
        return;
    }
    if (!sceneProject->scene->findComponent<AnimatorComponent>(entity)) {
        call->fail("record_seconds: the GameObject has no Animator.");
        return;
    }
    auto state = std::make_shared<RecordCall>();
    state->ctx = &call->ctx();
    state->project = call->ctx().project();
    state->sceneId = sceneProject->id;
    state->entity = entity;
    state->seconds = std::clamp(seconds, 0.1, 600.0);
    state->footSlideMm = footSlideMm;
    state->display = sceneProject->scene->getEntityName(entity) + " (recorded)";
    state->cancel = std::make_shared<std::atomic<bool>>(false);
    state->job = call->ctx().jobs().create("check_animation", "Check Animation");
    auto cancel = state->cancel;
    state->job->setCancelHandler([cancel]() { cancel->store(true); });
    state->job->setProgress(0.0, "recording");
    const bool wait = !call->args().contains("wait") || call->args()["wait"].get<bool>();
    const double timeout = call->args().contains("timeout_seconds") ? call->args()["timeout_seconds"].get<double>() : 120.0;
    recordTick(state);
    call->detach();
    replyWhenJobDone(call, state->job, wait, timeout, jobReply);
}

ToolResult jobReply(const McpJob& finished) {
    Json result = finished.result();
    result["job_id"] = finished.id();
    result["status"] = jobStatusName(finished.status());
    if (finished.status() != JobStatus::Succeeded) return ToolResult::error(finished.message(), result);
    return ToolResult::success(finished.message(), result);
}

void checkAnimationTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    auto request = std::make_shared<CheckRequest>();
    request->projectRoot = project->getProjectPath();

    if (args.contains("checks")) {
        for (const Json& name : args["checks"]) {
            const CheckRow* row = findCheck(name.get<std::string>());
            if (!row) {
                call->fail("checks: unknown check \"" + name.get<std::string>() + "\".");
                return;
            }
            request->checks.insert(row->name);
        }
    }

    std::string error;
    SceneProject* sceneProject = nullptr;
    Entity entity = NULL_ENTITY;
    if (args.contains("entity")) {
        if (!resolveGameObject(project, args, "entity", sceneProject, entity, error)) {
            call->fail(error);
            return;
        }
        auto rig = std::make_shared<anim::ClipRig>();
        if (!rigFromEntity(sceneProject, entity, *rig, error)) {
            call->fail("entity: " + error);
            return;
        }
        request->entityRig = rig;
    }
    if (args.contains("clips")) {
        for (const Json& given : args["clips"]) {
            NamedClip clip;
            if (!resolveNamedClip(project, given.get<std::string>(), clip, error)) {
                call->fail("clips: " + error);
                return;
            }
            request->clips.push_back(clip);
        }
    }
    if (args.contains("state")) {
        if (entity == NULL_ENTITY) {
            call->fail("state names a state of a GameObject: pass entity too.");
            return;
        }
        std::string stored;
        NamedClip clip;
        if (!clipOfState(sceneProject, entity, args["state"].get<std::string>(), stored, error) || !namedFromStored(project, stored, clip, error)) {
            call->fail("state: " + error);
            return;
        }
        request->clips.push_back(clip);
    }
    if (args.contains("compare_to")) {
        if (!resolveNamedClip(project, args["compare_to"].get<std::string>(), request->compareTo, error)) {
            call->fail("compare_to: " + error);
            return;
        }
        if (request->compareTo.model) {
            call->fail("compare_to names one clip: \"<model>#<clip>\" or an .anim.");
            return;
        }
        request->hasCompare = true;
    }
    if (args.contains("limits")) {
        const Json& limits = args["limits"];
        if (limits.contains("error_mm")) request->limits.errorMm = limits["error_mm"].get<double>();
        if (limits.contains("root_drift_mm")) {
            request->limits.rootDriftMm = limits["root_drift_mm"].get<double>();
            request->limits.rootDriftGiven = true;
        }
        if (limits.contains("foot_slide_mm")) request->limits.footSlideMm = limits["foot_slide_mm"].get<double>();
        if (limits.contains("pop_mm")) request->limits.popMm = limits["pop_mm"].get<double>();
    }
    if (args.contains("feet")) {
        for (const Json& foot : args["feet"]) request->feet.push_back(foot.get<std::string>());
        request->checks.insert("foot_sliding");
    }
    if (args.contains("move_speed")) {
        request->moveSpeed = args["move_speed"].get<double>();
        request->checks.insert("foot_sliding");
    }
    if (args.contains("record_seconds")) {
        if (entity == NULL_ENTITY) {
            call->fail("record_seconds watches a GameObject's Animator: pass entity too.");
            return;
        }
        recordAnimator(call, sceneProject, entity, args["record_seconds"].get<double>(), request->limits.footSlideMm);
        return;
    }
    if (args.contains("transition")) {
        if (entity == NULL_ENTITY) {
            call->fail("transition crossfades two states of a GameObject: pass entity too.");
            return;
        }
        const Json& given = args["transition"];
        if (!given.contains("from") || !given.contains("to")) {
            call->fail("transition needs from and to: two states of the GameObject's Animator or Animation Player.");
            return;
        }
        CheckTransition transition;
        transition.from = given["from"].get<std::string>();
        transition.to = given["to"].get<std::string>();
        if (given.contains("duration")) transition.duration = std::max(0.0f, given["duration"].get<float>());
        if (given.contains("at")) transition.at = std::max(0.0f, given["at"].get<float>());
        std::string stored;
        NamedClip fromClip;
        if (!clipOfState(sceneProject, entity, transition.from, stored, error) || !namedFromStored(project, stored, fromClip, error) ||
            !clipOfState(sceneProject, entity, transition.to, stored, error) || !namedFromStored(project, stored, transition.toClip, error)) {
            call->fail("transition: " + error);
            return;
        }
        controllerTiming(sceneProject, entity, transition);
        request->clips = {fromClip};
        request->transition = transition;
        request->checks.insert("transition");
    }
    if (request->checks.empty() || (request->checks.size() == 1 && request->checks.count("foot_sliding"))) {
        // the project as a whole is about what things cost: a rig per clip is for clips asked about
        request->checks.insert("stats");
        if (!request->clips.empty() && !request->hasCompare) {
            request->checks.insert("loop_seam");
            request->checks.insert("root_motion");
        }
    }
    if (args.contains("top")) request->top = (size_t)std::max(1, args["top"].get<int>());

    JobPtr job = call->ctx().jobs().create("check_animation", "Check Animation");
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    job->setCancelHandler([cancel]() { cancel->store(true); });
    job->setProgress(0.0, "starting");
    std::thread([request, job, cancel]() {
        try {
            runCheck(*request, job, cancel);
        } catch (const std::exception& e) {
            job->fail(std::string("check_animation failed: ") + e.what());
        }
    }).detach();

    const bool wait = !args.contains("wait") || args["wait"].get<bool>();
    const double timeout = args.contains("timeout_seconds") ? args["timeout_seconds"].get<double>() : 120.0;
    call->detach();
    replyWhenJobDone(call, job, wait, timeout, jobReply);
}

// ---------------------------------------------------------------- optimize_animation

struct OptimizeOutput {
    std::string relative;       // project-relative, ending in .anim
    fs::path absolute;
    bool exists = false;
};

struct OptimizeCall {
    McpContext* ctx = nullptr;
    JobPtr job;
    fs::path root;
    std::vector<NamedClip> sources;
    anim::ClipReductionOptions reduction;
    bool write = false;
    bool overwrite = false;
    std::string outputPath;     // empty: beside each source
    std::shared_ptr<const anim::ClipRig> entityRig;
    uint32_t sceneId = 0;
    Entity player = NULL_ENTITY;
    std::atomic<bool> cancel{false};
    std::atomic<bool> workDone{false};
    bool finished = false;

    std::vector<NamedClip> clips;   // the sources with models expanded
    std::vector<anim::ClipOptimizeResult> results;
    std::vector<std::shared_ptr<const anim::ClipRig>> resultRigs;
    std::vector<OptimizeOutput> outputs;
    std::string failure;
    Json failureData;
    std::string undoScope;
    bool written = false;
};

using OptimizeCallPtr = std::shared_ptr<OptimizeCall>;

void scheduleOptimizeTick(const OptimizeCallPtr& state);

Json errorEntry(const std::string& kind, const std::string& message, const std::string& fix) {
    return {{"kind", kind}, {"message", message}, {"fix", fix}};
}

// "<model stem>_<clip>" of a model clip, the file's stem otherwise, then .opt.anim beside it
std::string defaultOutputOf(const NamedClip& clip) {
    anim::SubClipReference parts;
    if (!anim::parseSubClipReference(clip.display, parts)) {
        const fs::path file = FileUtils::pathFromUtf8(clip.display);
        return FileUtils::pathToGenericUtf8(file.parent_path() / FileUtils::pathFromUtf8(FileUtils::pathToUtf8(file.stem()) + ".opt.anim"));
    }
    const fs::path model = FileUtils::pathFromUtf8(parts.model);
    std::string name = FileUtils::pathToUtf8(model.stem()) + "_" + parts.clip;
    for (char& c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '|' || c == '?' || c == '*' || c == '"' || c == '<' || c == '>') c = '_';
    }
    return FileUtils::pathToGenericUtf8(model.parent_path() / FileUtils::pathFromUtf8(name + ".opt.anim"));
}

// The worker: every clip reduced and measured
void runOptimizeWork(const OptimizeCallPtr& state) {
    std::vector<NamedClip> expanded;
    for (const NamedClip& source : state->sources) {
        std::string error;
        if (!source.model) {
            expanded.push_back(source);
        } else if (!expandModel(source, expanded, error)) {
            state->failure = "read_failed: " + source.display + ": " + error;
            state->workDone = true;
            return;
        }
    }
    state->clips = expanded;
    RigCache rigs;
    rigs.entity = state->entityRig;
    for (size_t i = 0; i < expanded.size(); i++) {
        if (state->cancel.load()) break;
        state->job->setProgress((double)i / (double)expanded.size(), expanded[i].display);
        ClipData data;
        std::string error;
        std::shared_ptr<const anim::ClipRig> rig;
        if (readClipData(expanded[i].reference, data, error)) rig = rigFor(rigs, expanded[i].reference, data, "on", error);
        if (!rig) {
            const bool noRig = error.rfind("no_rig", 0) == 0;
            state->failure = expanded[i].display + ": " + error;
            state->failureData = {{"errors", Json::array({errorEntry(noRig ? "no_rig" : "read_failed", error,
                "pass on (the GameObject that plays it), or set the clip's Preview Model")})}};
            state->workDone = true;
            return;
        }
        anim::ClipOptimizeRequest request;
        request.reference = expanded[i].reference;
        request.displayName = expanded[i].display;
        request.rig = rig.get();
        request.reduction = state->reduction;
        request.cancel = &state->cancel;
        request.inPlace = state->write && !state->outputPath.empty() && state->outputPath == expanded[i].display;
        anim::ClipOptimizeResult result = anim::runClipOptimize(request);
        if (!result.ok) {
            state->failure = expanded[i].display + ": " + result.errorKind + ": " + result.error + (result.fix.empty() ? "" : ". Fix: " + result.fix);
            state->failureData = {{"errors", Json::array({errorEntry(result.errorKind, result.error, result.fix)})}};
            state->workDone = true;
            return;
        }
        state->results.push_back(std::move(result));
        state->resultRigs.push_back(rig);
    }
    state->workDone = true;
}

Json optimizeResultJson(const OptimizeCall& state) {
    Json clips = Json::array();
    uint64_t before = 0, after = 0;
    double effective = state.reduction.toleranceMm;
    Json warnings = Json::array();
    for (const anim::ClipOptimizeResult& result : state.results) {
        clips.push_back(anim::clipOptimizeJson(result));
        before += result.before.runtimeBytes;
        after += result.after.runtimeBytes;
        effective = std::min(effective, result.effectiveToleranceMm);
        if (result.keptAllKeys) {
            warnings.push_back({{"kind", "kept_all_keys"}, {"clip", result.displayName},
                                {"message", "no per-track value met the limit, so every key stays"},
                                {"fix", "raise tolerance_mm, or leave this clip as it is"}});
        } else if (result.after.runtimeBytes >= result.before.runtimeBytes) {
            warnings.push_back({{"kind", "no_gain"}, {"clip", result.displayName},
                                {"message", "measured no smaller than the source: tracks that lose different keys stop sharing key times"},
                                {"fix", "keep the source"}});
        }
        if (result.skipped.total() > 0) {
            warnings.push_back({{"kind", "skipped_tracks"}, {"clip", result.displayName},
                                {"message", std::to_string(result.skipped.total()) + " tracks keep all their keys (skipped_tracks says why)"},
                                {"fix", ""}});
        }
    }
    return {{"written", state.written}, {"tolerance_mm", state.reduction.toleranceMm}, {"effective_tolerance_mm", effective},
            {"skin_distance_mm", std::max(state.reduction.skinDistanceMm, anim::kClipReductionSkinFloorMm)},
            {"total_bytes_before", before}, {"total_bytes_after", after}, {"clips", clips}, {"warnings", warnings},
            {"undo", state.written ? Json{{"scope", state.undoScope}} : Json(nullptr)}};
}

// What was measured: no line here promises a saving
std::string optimizeText(const OptimizeCall& state) {
    std::string text;
    for (size_t i = 0; i < state.results.size() && i < 8; i++) {
        const anim::ClipOptimizeResult& result = state.results[i];
        text += leafName(result.displayName) + ": Memory " + anim::formatClipField("bytes_before", (double)result.before.runtimeBytes) + " -> " +
                anim::formatClipField("bytes_after", (double)result.after.runtimeBytes) + ", Keys " + anim::formatClipField("keys_before", (double)result.before.keys) +
                " -> " + anim::formatClipField("keys_after", (double)result.after.keys) + ", " + anim::clipErrorLine(result.measured) +
                ", worst " + result.measured.worstJoint + " at " + anim::formatClipField("worst_time_s", result.measured.worstTimeSeconds);
        if (!result.writtenTo.empty()) text += ", written to " + result.writtenTo;
        text += "\n";
    }
    if (state.results.size() > 8) text += "(" + std::to_string(state.results.size() - 8) + " more)\n";
    if (state.written) {
        text += "Measured again from the file. One " + state.undoScope + " undo step. Prove it: check_animation {clips, compare_to}.";
    } else {
        text += "Nothing was written. The same arguments + output write this.";
    }
    return text;
}

// The main thread's part: the checks that need the results, then one undo step
bool writeOptimized(const OptimizeCallPtr& state) {
    Project* project = state->ctx->project();
    state->outputs.clear();
    const bool oneClip = state->results.size() == 1;
    for (size_t i = 0; i < state->results.size(); i++) {
        OptimizeOutput output;
        std::string given = state->outputPath;
        if (given.empty()) {
            given = defaultOutputOf(state->clips[i]);
        } else if (!oneClip) {
            // several clips and one path: a folder
            given = FileUtils::pathToGenericUtf8(FileUtils::pathFromUtf8(given) / FileUtils::pathFromUtf8(defaultOutputOf(state->clips[i])).filename());
        }
        if (lowered(FileUtils::pathFromUtf8(given).extension().string()) != ".anim") {
            state->failure = "output: \"" + given + "\" must end in .anim.";
            return false;
        }
        std::string error;
        if (!McpRefs::resolveProjectWritePath(project, given, output.absolute, error, false)) {
            state->failure = "output: " + error;
            return false;
        }
        output.absolute = output.absolute.lexically_normal();
        output.relative = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(output.absolute));
        std::error_code ec;
        output.exists = fs::exists(output.absolute, ec);
        if (output.exists && !state->overwrite) {
            const std::string message = "'" + output.relative + "' exists";
            state->failure = "output_exists: " + message + ". Fix: pass overwrite: true (one undo step puts it back), or choose another output.";
            state->failureData = {{"errors", Json::array({errorEntry("output_exists", message, "pass overwrite: true, or choose another output")})}};
            return false;
        }
        state->outputs.push_back(output);
    }

    std::vector<ClipSceneActions::OptimizedClip> switched;
    std::vector<w4::WriteFilesCmd::Item> items;
    for (size_t i = 0; i < state->results.size(); i++) {
        const OptimizeOutput& output = state->outputs[i];
        ClipData data = state->results[i].reduced;
        // a new file gets its own GUID; an existing one keeps its (newClipText)
        if (!output.exists) data.guid.clear();
        const std::string text = ClipSceneActions::newClipText(project, data, output.exists ? FileUtils::pathToGenericUtf8(output.absolute) : std::string());
        items.push_back({output.absolute, text});
        switched.push_back({state->clips[i].reference, output.absolute, text});
    }
    std::string error;
    if (state->player != NULL_ENTITY) {
        ClipActionResult actionResult;
        if (!ClipSceneActions::writeOptimizedClips(project, state->sceneId, state->player, switched, actionResult, error)) {
            state->failure = "write_failed: " + error;
            return false;
        }
        state->undoScope = "scene";
    } else {
        if (!w4::writeFilesUndoable(*state->ctx, std::move(items), error)) {
            state->failure = "write_failed: " + error;
            return false;
        }
        state->undoScope = "project";
    }
    for (size_t i = 0; i < state->results.size(); i++) state->results[i].writtenTo = state->outputs[i].relative;
    state->written = true;
    return true;
}

void finishOptimize(const OptimizeCallPtr& state) {
    state->finished = true;
    state->job->succeed(optimizeText(*state), optimizeResultJson(*state));
}

void failOptimize(const OptimizeCallPtr& state) {
    state->finished = true;
    Json data = state->failureData.is_object() ? state->failureData : Json::object();
    if (!data.contains("errors")) data["errors"] = Json::array({errorEntry("failed", state->failure, "call optimize_animation again; read_console for the cause")});
    state->job->fail(state->failure, data);
}

// Re-reads every output from disk on a worker: a reducer bug shows as a failed bound here
void startVerify(const OptimizeCallPtr& state) {
    state->workDone = false;
    std::thread([state]() {
        try {
            for (size_t i = 0; i < state->results.size(); i++) {
                std::string error;
                if (!anim::verifyWrittenClip(state->outputs[i].absolute, *state->resultRigs[i], state->results[i], error)) {
                    state->failure = "verify_failed: " + state->outputs[i].relative + ": " + error;
                    break;
                }
                // a joints override may loosen one joint on purpose: its bound is its own, checked by the search
                const double limit = state->results[i].effectiveToleranceMm;
                if (state->reduction.joints.empty() && !state->results[i].keptAllKeys && state->results[i].measured.maxErrorMm > limit) {
                    state->failure = "bound_failed: " + state->outputs[i].relative + " measures " +
                                     anim::formatClipField("max_error_mm", state->results[i].measured.maxErrorMm) + " from disk, over the limit of " +
                                     anim::formatClipField("max_error_mm", limit) + ". Undo it (one " + state->undoScope + " step).";
                    break;
                }
            }
        } catch (const std::exception& e) {
            state->failure = std::string("verify_failed: ") + e.what();
        }
        state->workDone = true;
    }).detach();
}

void optimizeTick(const OptimizeCallPtr& state) {
    if (state->finished || state->job->finished()) return;
    Project* project = state->ctx->project();
    if (!project || project->getProjectPath() != state->root) {
        state->finished = true;
        state->cancel = true;
        state->job->fail("project_changed: another project was opened while the job ran; nothing more was written.");
        return;
    }
    if (!state->workDone.load()) {
        if (state->job->cancelRequested()) state->cancel = true;
        scheduleOptimizeTick(state);
        return;
    }
    if (state->cancel.load() && !state->written) {
        state->finished = true;
        state->job->markCancelled("Cancelled; nothing was written.");
        return;
    }
    if (!state->failure.empty()) {
        failOptimize(state);
        return;
    }
    if (state->write && !state->written) {
        if (!writeOptimized(state)) {
            failOptimize(state);
            return;
        }
        startVerify(state);
        scheduleOptimizeTick(state);
        return;
    }
    finishOptimize(state);
}

void scheduleOptimizeTick(const OptimizeCallPtr& state) {
    state->ctx->app().requestRedraw();
    getEditorHost().enqueueMainThreadTask([state]() {
        // a continuation writes beneath the project, as the tool's run did
        McpWriteScope writeScope;
        try {
            optimizeTick(state);
        } catch (const std::exception& e) {
            state->finished = true;
            state->job->fail(std::string("optimize_animation failed: ") + e.what());
        }
    });
}

void optimizeAnimationTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    auto state = std::make_shared<OptimizeCall>();
    state->ctx = &call->ctx();
    state->root = project->getProjectPath();
    std::string error;
    for (const Json& given : args["clips"]) {
        NamedClip clip;
        if (!resolveNamedClip(project, given.get<std::string>(), clip, error)) {
            call->fail("clips: " + error);
            return;
        }
        state->sources.push_back(clip);
    }
    if (state->sources.empty()) {
        call->fail("clips: name at least one .anim, model or \"<model>#<clip>\".");
        return;
    }
    if (args.contains("tolerance_mm")) state->reduction.toleranceMm = args["tolerance_mm"].get<double>();
    if (args.contains("skin_distance_mm")) state->reduction.skinDistanceMm = args["skin_distance_mm"].get<double>();
    if (args.contains("joints")) {
        for (const Json& joint : args["joints"]) {
            state->reduction.joints.push_back({joint["path"].get<std::string>(), joint["tolerance_mm"].get<double>()});
        }
    }
    if (args.contains("output")) {
        if (args["output"].is_string()) {
            state->write = true;
            state->outputPath = args["output"].get<std::string>();
        } else {
            state->write = args["output"].get<bool>();
        }
    }
    state->overwrite = args.contains("overwrite") && args["overwrite"].get<bool>();
    // a model file is never changed: its clips write a new .anim
    if (!state->outputPath.empty() && anim::isModelFilePath(state->outputPath)) {
        call->fail("output: a model file is never changed; name an .anim, or a folder for several clips.");
        return;
    }
    if (args.contains("on")) {
        SceneProject* sceneProject = nullptr;
        Entity entity = NULL_ENTITY;
        if (!resolveGameObject(project, args, "on", sceneProject, entity, error)) {
            call->fail(error);
            return;
        }
        auto rig = std::make_shared<anim::ClipRig>();
        if (!rigFromEntity(sceneProject, entity, *rig, error)) {
            call->fail("on: " + error);
            return;
        }
        state->entityRig = rig;
    }
    if (args.contains("root")) {
        SceneProject* sceneProject = nullptr;
        if (!resolveGameObject(project, args, "root", sceneProject, state->player, error)) {
            call->fail(error);
            return;
        }
        state->sceneId = sceneProject->id;
        if (!state->write) {
            call->fail("root switches its Animation Player to the written clips: pass output too.");
            return;
        }
    }

    state->job = call->ctx().jobs().create("optimize_animation", state->write ? "Reduce Keys" : "Reduce Keys (preview)");
    std::weak_ptr<OptimizeCall> weak = state;
    state->job->setCancelHandler([weak]() {
        if (auto s = weak.lock()) s->cancel = true;
    });
    state->job->setProgress(0.0, "starting");
    std::thread([state]() {
        try {
            runOptimizeWork(state);
        } catch (const std::exception& e) {
            state->failure = std::string("optimize_animation failed: ") + e.what();
            state->workDone = true;
        }
    }).detach();
    scheduleOptimizeTick(state);

    const bool wait = !args.contains("wait") || args["wait"].get<bool>();
    const double timeout = args.contains("timeout_seconds") ? args["timeout_seconds"].get<double>() : 120.0;
    call->detach();
    replyWhenJobDone(call, state->job, wait, timeout, jobReply);
}

} // namespace

void registerAnimationQualityTools(McpRegistry& registry) {
    using namespace schema;
    const Json stringItem = Json{{"type", "string"}};
    Json entityOrIndex;
    entityOrIndex["type"] = Json::array({"string", "integer"});
    Json output;
    output["type"] = Json::array({"string", "boolean"});
    {
        ToolDef tool;
        tool.name = "check_animation";
        tool.title = "Check Animation";
        tool.description =
            "What clips cost and how good they are, measured through the runtime sampler: Memory, keys, Loop Match, root motion, "
            "error against compare_to. Empty clips: the project, largest first. ok false is a successful call with findings. "
            "Writes nothing. Guide: jasper://docs/animation.";
        tool.inputSchema = object({
            {"clips", array(stringItem, ".anim, model#Clip, or a model (all its clips).")},
            {"entity", entityOrIndex},
            {"scene", sceneRef()},
            {"state", string("")},
            {"checks", array(stringEnum("", checkNames()), "")},
            {"compare_to", string("")},
            {"transition", object({{"from", string("")}, {"to", string("")}, {"duration", number("")}, {"at", number("")}})},
            {"feet", array(stringItem, "")},
            {"move_speed", number("m/s")},
            {"record_seconds", number("")},
            {"limits", object({{"error_mm", number("")}, {"foot_slide_mm", number("")}, {"root_drift_mm", number("")}, {"pop_mm", number("")}})},
            {"top", withDefault(integer("", 1, 1000), (int)DEFAULT_TOP)},
            {"wait", withDefault(boolean(""), true)},
            {"timeout_seconds", withDefault(number("", 1, 3600), 120)}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = false;
        tool.allowedWhilePlaying = true;
        tool.handler = checkAnimationTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "optimize_animation";
        tool.title = "Optimize Animation";
        tool.description =
            "Remove clip keys a skeleton does not need, keeping the error measured at skin_distance_mm within tolerance_mm. "
            "Without output nothing is written: per-clip Memory, keys, max error, worst joint (often a small change: read them). "
            "The same arguments + output write a new .anim (one undo step, measured again from disk); a model is never changed. "
            "Guide: jasper://docs/animation.";
        tool.inputSchema = object({
            {"clips", array(stringItem, ".anim, model#Clip, or a model (all its clips).")},
            {"on", entityOrIndex},
            {"scene", sceneRef()},
            {"tolerance_mm", withDefault(number("", 0.01, 100), 1)},
            {"skin_distance_mm", withDefault(number("", 10, 1000), 100)},
            {"joints", array(object({{"path", string("")}, {"tolerance_mm", number("", 0.01, 100)}}, {"path", "tolerance_mm"}), "")},
            {"output", output},
            {"overwrite", withDefault(boolean(""), false)},
            {"root", entityOrIndex},
            {"wait", withDefault(boolean(""), true)},
            {"timeout_seconds", withDefault(number("", 1, 3600), 120)}
        }, {"clips"});
        tool.annotations.readOnly = false;
        tool.annotations.idempotent = true;
        tool.allowedWhilePlaying = false;
        tool.activity = ToolActivity::Edit;
        tool.handler = optimizeAnimationTool;
        registry.addTool(std::move(tool));
    }
}

}
