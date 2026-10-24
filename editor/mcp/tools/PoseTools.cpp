// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Pose animations (spec: pose animation phase 2, section 6): get_pose_animation (the asset, its
// timing and, on a rig, per-pose metrics) and edit_pose_animation (ops applied in order to a copy,
// all or nothing, committed once through PoseAnimationDocument: one project undo step). Angles
// are the rig-independent joint angles of engine/core/pose/PoseDofs (degrees, 0 = neutral
// standing); the agent guide page is docs/mcp/guide/pose-animation.md. render_pose_animation
// (phase 3) draws contact sheets of the key poses or the motion in its own block at the end.
// Templates (phase 6, editor/pose/PoseTemplates): create.template, the apply_template and
// save_template ops and get_pose_animation {templates: true}, in the "templates" block.
#include "McpTools.h"
#include "W4Shared.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"

#include "Project.h"
#include "component/ModelComponent.h"
#include "component/PoseAnimatorComponent.h"
#include "pose/HumanBones.h"
#include "pose/HumanoidMapIO.h"
#include "pose/PoseAnimationDocument.h"
#include "pose/PoseBaker.h"
#include "pose/PoseMetrics.h"
#include "pose/PoseRig.h"
#include "pose/PoseRigProvider.h"
#include "pose/PoseSheetRenderer.h"
#include "pose/PoseTemplates.h"
#include "pose/PoseSampler.h"
#include "pose/PoseTween.h"
#include "pose/PoseAnimationIO.h"
#include "pose/PoseAnimatorRuntime.h"
#include "pose/PoseDofs.h"
#include "pose/PoseRanges.h"
#include "pose/PoseTypes.h"
#include "util/Base64.h"
#include "util/ProjectChecks.h"
#include "util/Util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace jasperengine::editor::mcp {

namespace {

namespace fs = std::filesystem;
using namespace ::jasperengine::pose;

constexpr int MAX_POSES = 200;

// ------------------------------------------------------------------ names and suggestions

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

size_t editDistance(const std::string& a, const std::string& b) {
    std::vector<size_t> row(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) row[j] = j;
    for (size_t i = 1; i <= a.size(); i++) {
        size_t diagonal = row[0];
        row[0] = i;
        for (size_t j = 1; j <= b.size(); j++) {
            const size_t above = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diagonal = above;
        }
    }
    return row[b.size()];
}

// The candidate nearest to `name` (edit distance), "" when none is reasonably close
std::string nearest(const std::string& name, const std::vector<std::string>& candidates) {
    std::string best;
    size_t bestDistance = std::max<size_t>(3, name.size() / 2) + 1;
    for (const std::string& candidate : candidates) {
        const size_t distance = editDistance(lower(name), candidate);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = candidate;
        }
    }
    return best;
}

std::vector<std::string> slotNames() {
    std::vector<std::string> names;
    for (int b = 0; b < HUMAN_BONE_COUNT; b++) names.push_back(humanBoneName(boneAt(b)));
    return names;
}

// A slot name from what an agent may write instead ("left_elbow", "thigh_l", "LeftForeArm",
// "pelvis"): body-part synonyms and side tokens, then the nearest slot name
std::string suggestSlot(const std::string& given) {
    std::string text = lower(given);
    HumanBone bone;
    if (humanBoneFromName(text, bone)) return text;
    // tokens: split on _ . - space and camelCase (before lowering)
    std::vector<std::string> tokens;
    std::string token;
    for (size_t i = 0; i < given.size(); i++) {
        const char c = given[i];
        const bool upper = std::isupper(static_cast<unsigned char>(c)) != 0;
        if (c == '_' || c == '.' || c == '-' || c == ' ' || (upper && !token.empty() &&
            !std::isupper(static_cast<unsigned char>(given[i - 1])))) {
            if (!token.empty()) tokens.push_back(lower(token));
            token.clear();
            if (!upper) continue;
        }
        token += c;
    }
    if (!token.empty()) tokens.push_back(lower(token));
    std::string side;
    std::string part;
    for (const std::string& t : tokens) {
        if (t == "left" || t == "l") side = "left";
        else if (t == "right" || t == "r") side = "right";
        else part += t;
    }
    static const std::vector<std::pair<std::string, std::string>> synonyms = {
        {"elbow", "lower_arm"}, {"forearm", "lower_arm"}, {"lowerarm", "lower_arm"},
        {"upperarm", "upper_arm"}, {"arm", "upper_arm"}, {"wrist", "hand"}, {"hand", "hand"},
        {"clavicle", "shoulder"}, {"collar", "shoulder"}, {"shoulder", "shoulder"},
        {"thigh", "upper_leg"}, {"upleg", "upper_leg"}, {"upperleg", "upper_leg"}, {"hip", "upper_leg"},
        {"knee", "lower_leg"}, {"shin", "lower_leg"}, {"calf", "lower_leg"}, {"lowerleg", "lower_leg"}, {"leg", "upper_leg"},
        {"ankle", "foot"}, {"foot", "foot"}, {"toe", "toes"}, {"toes", "toes"}, {"ball", "toes"}};
    static const std::vector<std::pair<std::string, std::string>> centre = {
        {"pelvis", "hips"}, {"hips", "hips"}, {"hip", "hips"}, {"torso", "spine"}, {"spine", "spine"},
        {"chest", "chest"}, {"upperchest", "upper_chest"}, {"neck", "neck"}, {"head", "head"}};
    if (!side.empty()) {
        for (const auto& [word, slot] : synonyms) {
            if (part == word) return side + "_" + slot;
        }
    } else {
        for (const auto& [word, slot] : centre) {
            if (part == word) return slot;
        }
    }
    return nearest(text, slotNames());
}

// "ops[2].angles: unknown DOF 'left_elbow.bend' (did you mean 'left_lower_arm.bend'?)"
std::string unknownDofError(const std::string& path, const std::string& name) {
    std::string suggestion;
    const size_t dot = name.find('.');
    const std::string slot = suggestSlot(dot == std::string::npos ? name : name.substr(0, dot));
    HumanBone bone;
    if (!slot.empty() && humanBoneFromName(slot, bone)) {
        const BoneDofInfo& info = boneDofInfo(bone);
        std::vector<std::string> dofs;
        for (int k = 0; k < DOFS_PER_BONE; k++) dofs.push_back(info.dofs[k].name);
        const std::string part = dot == std::string::npos ? "" : lower(name.substr(dot + 1));
        std::string dof = part.empty() ? "" : nearest(part, dofs);
        if (dof.empty() && !part.empty()) {
            // a synonym of a DOF of another joint ("flex", "lift"): list the slot's DOFs
            suggestion = " (" + slot + " has " + dofs[0] + ", " + dofs[1] + ", " + dofs[2] + ")";
        } else if (dof.empty()) {
            suggestion = " (did you mean " + slot + "." + dofs[0] + ", " + slot + "." + dofs[1] + " or " + slot + "." + dofs[2] + "?)";
        } else {
            suggestion = " (did you mean '" + slot + "." + dof + "'?)";
        }
    }
    return path + ": unknown DOF '" + name + "'" + suggestion;
}

std::string unknownSlotError(const std::string& path, const std::string& name) {
    const std::string slot = suggestSlot(name);
    return path + ": unknown humanoid slot '" + name + "'" + (slot.empty() ? std::string(" (slots: hips, spine, chest, "
           "upper_chest, neck, head, left_/right_ shoulder, upper_arm, lower_arm, hand, upper_leg, lower_leg, foot, toes)")
           : " (did you mean '" + slot + "'?)");
}

// ------------------------------------------------------------------ numbers and JSON out

// 4 decimals (the file's precision), no -0
double r4(double value) {
    const double r = std::round(value * 10000.0) / 10000.0;
    return r == 0.0 ? 0.0 : r;
}

double r1(double value) {
    const double r = std::round(value * 10.0) / 10.0;
    return r == 0.0 ? 0.0 : r;
}

std::string fmt(double value, int decimals = 2) {
    // what rounds to zero prints as 0 (not "-0.00")
    if (std::fabs(value) < 0.5 * std::pow(10.0, -decimals)) value = 0.0;
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(decimals);
    out << value;
    return out.str();
}

Json transitionJson(const Transition& transition) {
    return {{"curve", curveName(transition.curve)}, {"duration", r4(transition.duration)},
            {"overshoot", r4(transition.overshoot)}, {"anticipation", r4(transition.anticipation)}};
}

Json feelJson(const PoseAnimationSettings& settings) {
    Json weights = Json::object();
    for (int g = 0; g < static_cast<int>(BodyGroup::Count); g++) {
        weights[bodyGroupName(static_cast<BodyGroup>(g))] = r4(settings.weights[g]);
    }
    return {{"transition", transitionJson(settings.transition)}, {"step_rate", r4(settings.stepRate)},
            {"overlap", r4(settings.overlap)}, {"looseness", r4(settings.looseness)}, {"weights", weights}};
}

Json humanizeJson(const HumanizeSettings& h) {
    return {{"enabled", h.enabled}, {"joint_limits", h.jointLimits}, {"moving_holds", r4(h.movingHolds)},
            {"inbetweens", r4(h.inbetweens)}, {"life", r4(h.life)}, {"seed", h.seed}, {"head_follow", r4(h.headFollow)},
            {"torso_follow", r4(h.torsoFollow)}, {"springiness", r4(h.springiness)}};
}

// Timeline (spec phase 1, 4.8, pose::buildTimeline): pose i is reached at start[i]; segment i (to
// pose i+1, the last one back to pose 0 only when looping) starts after its hold and lasts its
// transition's duration
struct Timeline {
    std::vector<double> start;
    double length = 0.0;
    int frameCount = 0;     // frames a bake or the runtime samples (pose::frames: f = 0..count)
};

Timeline timelineOf(const PoseAnimationData& data) {
    Timeline timeline;
    const PoseTimeline built = buildTimeline(data);
    const size_t n = data.poses.size();
    double key = 0.0;
    for (size_t i = 0; i < n; i++) {
        timeline.start.push_back(key);
        if (i < built.segmentCount()) key = built.start[i] + std::max(0.0, static_cast<double>(built.transitions[i].duration));
    }
    timeline.length = built.length;
    const int fps = std::max(1, data.settings.frameRate);
    timeline.frameCount = n == 0 ? 0 : std::max(1, static_cast<int>(std::lround(timeline.length * fps))) + 1;
    return timeline;
}

std::string projectRel(Project* project, const fs::path& absolute) {
    return ProjectChecks::projectRelative(project->getProjectPath(), absolute);
}

// preview_model is stored relative to the assets folder (as model files are); tools show it project-relative
fs::path previewModelFile(Project* project, const PoseAnimationData& data) {
    return project->resolveAssetPath(fs::u8path(data.previewModel)).lexically_normal();
}

std::string previewModelPath(Project* project, const PoseAnimationData& data) {
    if (data.previewModel.empty()) return "";
    return projectRel(project, previewModelFile(project, data));
}

// The rig the tools use: a model GameObject's (`model`), else the preview model's
struct RigUse {
    std::shared_ptr<const PoseRigInfo> info;   // null: no preview model and no model given
    Json model;                                // rig.model: the GameObject {id, name, path} or the model file
};

RigUse previewRig(Project* project, const PoseAnimationData& data) {
    RigUse use;
    if (data.previewModel.empty()) return use;
    use.info = PoseRigProvider::forModelFile(project, previewModelFile(project, data));
    use.model = previewModelPath(project, data);
    return use;
}

Json rigJson(const RigUse& use, bool full) {
    if (!use.info) return nullptr;
    Json rig = {{"model", use.model}};
    const PoseRigInfo& info = *use.info;
    int mapped = 0;
    Json bones = Json::object();
    for (int b = 0; b < HUMAN_BONE_COUNT; b++) {
        if (info.humanoid.bones[b].empty()) continue;
        mapped++;
        bones[humanBoneName(boneAt(b))] = info.humanoid.bones[b];
    }
    rig["mapped"] = mapped;
    if (full) rig["bones"] = bones;
    rig["missing"] = info.missing;
    if (info.rig) rig["human_scale"] = r4(info.rig->humanScale());
    if (!info.error.empty()) rig["error"] = info.error;
    if (full && !info.notes.empty()) rig["notes"] = info.notes;
    return rig;
}

double r3(double value) {
    const double r = std::round(value * 1000.0) / 1000.0;
    return r == 0.0 ? 0.0 : r;
}

Json metricsJson(const PoseMetrics& m) {
    Json hits = Json::array();
    for (int dof : m.limitHits) hits.push_back(dofName(dof));
    auto foot = [](const FootMetrics& f) { return Json{{"height", r3(f.height)}, {"contact", f.contact}}; };
    return {{"feet", {{"left", foot(m.left)}, {"right", foot(m.right)}}}, {"lowest", r3(m.lowest)},
            {"balanced", m.balanced}, {"balance_margin", r3(m.balanceMargin)},
            {"center_of_mass", {r3(m.centerOfMass.x), r3(m.centerOfMass.y), r3(m.centerOfMass.z)}}, {"limit_hits", hits}};
}

Json poseJson(const PoseAnimationData& data, const Timeline& timeline, const PoseRealisation* realised, size_t index, bool full) {
    const PoseKey& key = data.poses[index];
    Json pose = {{"index", index}, {"name", key.name}, {"hold", r4(key.hold)}, {"grounded", key.grounded},
                 {"lift", r4(key.lift)},
                 {"transition", key.customTransition ? transitionJson(key.transition) : Json(nullptr)},
                 {"start_time", r4(index < timeline.start.size() ? timeline.start[index] : 0.0)},
                 {"offset", {{"left", r4(key.body.offset.x)}, {"forward", r4(key.body.offset.z)}}}};
    if (full) {
        Json angles = Json::object();
        for (int dof = 0; dof < DOF_COUNT; dof++) {
            const float value = key.body.angles[dof];
            if (std::fabs(value) >= 0.05f) angles[dofName(dof)] = r1(value);
        }
        pose["angles"] = angles;
    }
    if (realised && index < realised->metrics.size()) pose["metrics"] = metricsJson(realised->metrics[index]);
    return pose;
}

Json animationJson(Project* project, PoseAnimationDocument& document, const RigUse& rig, const std::vector<size_t>& indices, bool full) {
    const PoseAnimationData& data = document.data();
    const Timeline timeline = timelineOf(data);
    std::shared_ptr<const PoseRealisation> realised;
    if (rig.info && rig.info->rig) realised = document.realise(rig.info->rig);
    Json poses = Json::array();
    for (size_t index : indices) poses.push_back(poseJson(data, timeline, realised.get(), index, full));
    const FeelPreset* preset = matchingFeelPreset(data.settings);
    Json out = {{"path", document.path()}, {"preview_model", previewModelPath(project, data)},
                {"rig", rigJson(rig, full)},
                {"loop", data.settings.loop}, {"frame_rate", data.settings.frameRate}, {"length", r4(timeline.length)},
                {"frame_count", realised ? static_cast<int>(realised->frames.size()) : timeline.frameCount},
                {"pose_count", data.poses.size()},
                {"feel", feelJson(data.settings)}, {"humanize", humanizeJson(data.settings.humanize)},
                {"preset", preset ? Json(preset->name) : Json(nullptr)},
                {"poses", poses}};
    if (!document.loadWarnings().empty()) out["warnings"] = document.loadWarnings();
    return out;
}

// "0 "Contact R" at 0.00 s, hold 0.10, grounded, 2 angles; feet L 0.00 R 0.00, balanced"
std::string poseLine(const Json& pose) {
    std::string line = std::to_string(pose["index"].get<size_t>()) + " \"" + pose["name"].get<std::string>() + "\" at " +
                       fmt(pose["start_time"].get<double>()) + " s, hold " + fmt(pose["hold"].get<double>());
    line += pose["grounded"].get<bool>() ? ", grounded" : ", lift " + fmt(pose["lift"].get<double>());
    if (!pose["transition"].is_null()) line += ", own transition " + fmt(pose["transition"]["duration"].get<double>()) + " s";
    if (pose.contains("angles")) line += ", " + std::to_string(pose["angles"].size()) + " angles";
    if (pose.contains("metrics")) {
        const Json& m = pose["metrics"];
        line += "; feet L " + fmt(m["feet"]["left"]["height"].get<double>()) + " R " + fmt(m["feet"]["right"]["height"].get<double>()) +
                (m["balanced"].get<bool>() ? ", balanced" : ", unbalanced (" + fmt(m["balance_margin"].get<double>(), 3) + ")");
        if (!m["limit_hits"].empty()) line += ", " + std::to_string(m["limit_hits"].size()) + " limit hit(s)";
    }
    return line;
}

// ------------------------------------------------------------------ argument parsing

bool readNumber(const Json& value, const std::string& path, float& out, std::string& error) {
    if (!value.is_number() || !std::isfinite(value.get<double>())) {
        error = path + " must be a number";
        return false;
    }
    const double v = value.get<double>();
    out = static_cast<float>(std::max(-1.0e30, std::min(1.0e30, v)));
    return true;
}

bool readBool(const Json& value, const std::string& path, bool& out, std::string& error) {
    if (!value.is_boolean()) {
        error = path + " must be true or false";
        return false;
    }
    out = value.get<bool>();
    return true;
}

std::string poseRange(size_t count) {
    return count == 0 ? "the animation has no poses" : "the animation has " + std::to_string(count) + " pose(s): 0.." + std::to_string(count - 1);
}

bool readIndex(const Json& value, const std::string& path, size_t count, int& out, std::string& error, int minimum = 0) {
    if (!value.is_number_integer()) {
        error = path + " must be a pose index (integer, 0-based)";
        return false;
    }
    const long long v = value.get<long long>();
    if (v < minimum || v >= static_cast<long long>(count)) {
        error = path + ": no pose " + std::to_string(v) + " (" + poseRange(count) + (minimum < 0 ? "; -1 = before the first" : "") + ")";
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

// Unknown keys of an object argument fail, listing the valid ones
bool checkKeys(const Json& object, const std::string& path, const std::vector<std::string>& allowed, std::string& error) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (std::find(allowed.begin(), allowed.end(), it.key()) != allowed.end()) continue;
        std::string valid;
        for (const std::string& key : allowed) valid += (valid.empty() ? "" : ", ") + key;
        const std::string close = nearest(it.key(), allowed);
        error = path + ": unknown key '" + it.key() + "'" + (close.empty() ? "" : " (did you mean '" + close + "'?)") +
                "; valid: " + valid;
        return false;
    }
    return true;
}

bool readObject(const Json& value, const std::string& path, std::string& error) {
    if (!value.is_object()) {
        error = path + " must be an object";
        return false;
    }
    return true;
}

// {curve, duration, overshoot, anticipation}, each optional: starts from `base`
bool readTransition(const Json& value, const std::string& path, Transition& out, std::string& error) {
    if (!readObject(value, path, error) || !checkKeys(value, path, {"curve", "duration", "overshoot", "anticipation"}, error)) return false;
    if (value.contains("curve")) {
        if (!value["curve"].is_string() || !curveFromName(value["curve"].get<std::string>(), out.curve)) {
            error = path + ".curve must be spring, linear or smooth";
            return false;
        }
    }
    if (value.contains("duration") && !readNumber(value["duration"], path + ".duration", out.duration, error)) return false;
    if (value.contains("overshoot") && !readNumber(value["overshoot"], path + ".overshoot", out.overshoot, error)) return false;
    if (value.contains("anticipation") && !readNumber(value["anticipation"], path + ".anticipation", out.anticipation, error)) return false;
    return true;
}

// {left, forward} in humanScale units (the hips horizontal offset)
bool readOffset(const Json& value, const std::string& path, Vector3& offset, std::string& error) {
    if (!readObject(value, path, error) || !checkKeys(value, path, {"left", "forward"}, error)) return false;
    if (value.contains("left") && !readNumber(value["left"], path + ".left", offset.x, error)) return false;
    if (value.contains("forward") && !readNumber(value["forward"], path + ".forward", offset.z, error)) return false;
    return true;
}

// {"left_upper_arm.raise": 30, ...}
bool readAngles(const Json& value, const std::string& path, std::vector<std::pair<int, float>>& out, std::string& error) {
    if (!readObject(value, path, error)) return false;
    for (auto it = value.begin(); it != value.end(); ++it) {
        int dof = 0;
        if (!dofFromName(it.key(), dof)) {
            error = unknownDofError(path, it.key());
            return false;
        }
        float degrees = 0.0f;
        if (!readNumber(it.value(), path + "." + it.key(), degrees, error)) return false;
        out.emplace_back(dof, degrees);
    }
    return true;
}

// Shortest-way angle lerp (degrees)
float lerpAngle(float a, float b, float t) {
    return a + wrapAngle(b - a) * t;
}

// ------------------------------------------------------------------ ops

struct SidecarEdit {
    fs::path path;                  // absolute .humanoid path
    std::string model;              // project-relative model path
    HumanoidOverrides overrides;
};

constexpr uint8_t TOUCHED = 1;      // returned in the result
constexpr uint8_t ANGLES_SET = 2;   // this call set its angles: joint limits apply to it

struct EditState {
    Project* project = nullptr;
    PoseAnimationData data;
    std::vector<uint8_t> touched;   // per pose: TOUCHED (added, updated, moved, mirrored, retimed), ANGLES_SET
    std::vector<std::string> changes;
    std::vector<std::string> warnings;
    std::map<std::string, SidecarEdit> sidecars;   // by sidecar path
    std::map<std::string, std::string> templateFiles;   // save_template (phase 6): absolute path -> text
};

const std::map<std::string, std::vector<std::string>>& opArguments() {
    static const std::map<std::string, std::vector<std::string>> table = {
        {"add_pose", {"op", "after", "name", "from", "blend_to", "blend", "hold", "grounded", "lift", "offset", "angles",
                      "add_angles", "transition"}},
        {"update_pose", {"op", "index", "name", "hold", "grounded", "lift", "offset", "angles", "add_angles", "transition"}},
        {"delete_pose", {"op", "index"}},
        {"move_pose", {"op", "index", "to"}},
        {"mirror_pose", {"op", "index", "as_new", "name"}},
        {"set_settings", {"op", "loop", "frame_rate", "transition", "step_rate", "overlap", "looseness", "weights", "humanize",
                          "clear_custom"}},
        {"apply_preset", {"op", "name", "clear_custom"}},
        {"auto_timing", {"op"}},
        {"set_preview_model", {"op", "model"}},
        {"set_humanoid", {"op", "model", "bones"}},
        // templates (phase 6)
        {"apply_template", {"op", "name", "mode"}},
        {"save_template", {"op", "name", "description", "category"}},
    };
    return table;
}

std::vector<std::string> opNames() {
    std::vector<std::string> names;
    for (const auto& entry : opArguments()) names.push_back(entry.first);
    return names;
}

// The fields add_pose and update_pose share: hold, grounded, lift, offset, angles, add_angles,
// transition (null = the animation's). `fields` collects the changed field names.
bool applyKeyFields(EditState& state, size_t index, const Json& op, const std::string& path,
                    std::vector<std::string>& fields, std::string& error) {
    PoseKey& key = state.data.poses[index];
    if (op.contains("name")) {
        if (!op["name"].is_string()) {
            error = path + ".name must be a string";
            return false;
        }
        if (op["name"].get<std::string>() != key.name) fields.push_back("name");
        key.name = op["name"].get<std::string>();
    }
    if (op.contains("hold")) {
        if (!readNumber(op["hold"], path + ".hold", key.hold, error)) return false;
        fields.push_back("hold");
    }
    if (op.contains("grounded")) {
        if (!readBool(op["grounded"], path + ".grounded", key.grounded, error)) return false;
        fields.push_back("grounded");
    }
    if (op.contains("lift")) {
        if (!readNumber(op["lift"], path + ".lift", key.lift, error)) return false;
        fields.push_back("lift");
        if (key.grounded && key.lift > 0.0f && !op.contains("grounded")) {
            state.warnings.push_back(path + ": lift only applies when grounded is false (the pose is grounded)");
        }
    }
    if (op.contains("offset")) {
        if (!readOffset(op["offset"], path + ".offset", key.body.offset, error)) return false;
        fields.push_back("offset");
    }
    if (op.contains("transition")) {
        if (op["transition"].is_null()) {
            key.customTransition = false;
            key.transition = Transition();
        } else {
            // a partial transition starts from the one the pose uses now
            Transition transition = transitionOut(state.data, index);
            if (!readTransition(op["transition"], path + ".transition", transition, error)) return false;
            key.transition = transition;
            key.customTransition = true;
        }
        fields.push_back("transition");
    }
    if (op.contains("angles")) {
        std::vector<std::pair<int, float>> angles;
        if (!readAngles(op["angles"], path + ".angles", angles, error)) return false;
        for (const auto& [dof, degrees] : angles) key.body.angles[dof] = degrees;
        fields.push_back("angles (" + std::to_string(angles.size()) + ")");
    }
    if (op.contains("add_angles")) {
        std::vector<std::pair<int, float>> angles;
        if (!readAngles(op["add_angles"], path + ".add_angles", angles, error)) return false;
        for (const auto& [dof, degrees] : angles) key.body.angles[dof] += degrees;
        fields.push_back("add_angles (" + std::to_string(angles.size()) + ")");
    }
    return true;
}

bool addPose(EditState& state, const Json& op, const std::string& path, std::string& error) {
    std::vector<PoseKey>& poses = state.data.poses;
    const size_t count = poses.size();
    if (count >= static_cast<size_t>(MAX_POSES)) {
        error = path + ": an animation has at most " + std::to_string(MAX_POSES) + " poses";
        return false;
    }
    int after = static_cast<int>(count) - 1;
    if (op.contains("after") && !readIndex(op["after"], path + ".after", count, after, error, -1)) return false;
    int from = after >= 0 ? after : (count > 0 ? 0 : -1);
    if (op.contains("from") && !readIndex(op["from"], path + ".from", count, from, error)) return false;
    PoseKey key = from >= 0 ? poses[from] : PoseKey();
    std::string how = from >= 0 ? "copy of " + std::to_string(from) : "neutral";
    if (op.contains("blend_to")) {
        int to = 0;
        if (!readIndex(op["blend_to"], path + ".blend_to", count, to, error)) return false;
        float blend = 0.5f;
        if (op.contains("blend") && !readNumber(op["blend"], path + ".blend", blend, error)) return false;
        if (blend < 0.0f || blend > 1.0f) {
            error = path + ".blend must be 0..1 (0 = from, 1 = blend_to)";
            return false;
        }
        const PoseKey& target = poses[to];
        for (int dof = 0; dof < DOF_COUNT; dof++) key.body.angles[dof] = lerpAngle(key.body.angles[dof], target.body.angles[dof], blend);
        key.body.offset.x += (target.body.offset.x - key.body.offset.x) * blend;
        key.body.offset.z += (target.body.offset.z - key.body.offset.z) * blend;
        key.lift += (target.lift - key.lift) * blend;
        how += ", " + fmt(blend * 100.0, 0) + "% toward " + std::to_string(to);
    } else if (op.contains("blend")) {
        error = path + ".blend needs blend_to (the pose to blend toward)";
        return false;
    }
    const size_t at = static_cast<size_t>(after + 1);
    key.name = "Pose " + std::to_string(count + 1);
    poses.insert(poses.begin() + at, key);
    state.touched.insert(state.touched.begin() + at, static_cast<uint8_t>(TOUCHED | ANGLES_SET));
    std::vector<std::string> fields;
    if (!applyKeyFields(state, at, op, path, fields, error)) return false;
    state.changes.push_back("add_pose: \"" + poses[at].name + "\" at " + std::to_string(at) + " (" + how + ")");
    return true;
}

bool updatePose(EditState& state, const Json& op, const std::string& path, std::string& error) {
    if (!op.contains("index")) {
        error = path + ".index is required";
        return false;
    }
    int index = 0;
    if (!readIndex(op["index"], path + ".index", state.data.poses.size(), index, error)) return false;
    std::vector<std::string> fields;
    if (!applyKeyFields(state, static_cast<size_t>(index), op, path, fields, error)) return false;
    state.touched[index] |= TOUCHED;
    if (op.contains("angles") || op.contains("add_angles")) state.touched[index] |= ANGLES_SET;
    std::string list;
    for (const std::string& field : fields) list += (list.empty() ? "" : ", ") + field;
    state.changes.push_back("update_pose " + std::to_string(index) + " \"" + state.data.poses[index].name + "\": " +
                            (list.empty() ? "nothing" : list));
    return true;
}

bool deletePose(EditState& state, const Json& op, const std::string& path, std::string& error) {
    std::vector<PoseKey>& poses = state.data.poses;
    if (!op.contains("index")) {
        error = path + ".index is required";
        return false;
    }
    int index = 0;
    if (!readIndex(op["index"], path + ".index", poses.size(), index, error)) return false;
    if (poses.size() == 1) {
        error = path + ": refusing to delete the last pose (an animation keeps at least one)";
        return false;
    }
    const std::string name = poses[index].name;
    poses.erase(poses.begin() + index);
    state.touched.erase(state.touched.begin() + index);
    state.changes.push_back("delete_pose " + std::to_string(index) + " \"" + name + "\"");
    return true;
}

bool movePose(EditState& state, const Json& op, const std::string& path, std::string& error) {
    std::vector<PoseKey>& poses = state.data.poses;
    if (!op.contains("index") || !op.contains("to")) {
        error = path + ": index and to are required";
        return false;
    }
    int index = 0, to = 0;
    if (!readIndex(op["index"], path + ".index", poses.size(), index, error)) return false;
    if (!readIndex(op["to"], path + ".to", poses.size(), to, error)) return false;
    PoseKey key = poses[index];
    const uint8_t mark = state.touched[index];
    poses.erase(poses.begin() + index);
    state.touched.erase(state.touched.begin() + index);
    poses.insert(poses.begin() + to, key);
    state.touched.insert(state.touched.begin() + to, static_cast<uint8_t>(mark | TOUCHED));
    state.changes.push_back("move_pose \"" + key.name + "\": " + std::to_string(index) + " -> " + std::to_string(to));
    return true;
}

// clear_custom: true -> every pose uses the animation's transition again
bool clearCustomTransitions(EditState& state, const Json& op, const std::string& path, std::string& error) {
    bool clear = false;
    if (op.contains("clear_custom") && !readBool(op["clear_custom"], path + ".clear_custom", clear, error)) return false;
    if (!clear) return true;
    int cleared = 0;
    for (size_t i = 0; i < state.data.poses.size(); i++) {
        PoseKey& key = state.data.poses[i];
        if (!key.customTransition) continue;
        key.customTransition = false;
        key.transition = Transition();
        state.touched[i] |= TOUCHED;
        cleared++;
    }
    state.changes.push_back("clear_custom: " + std::to_string(cleared) + " pose(s) use the animation's transition again");
    return true;
}

bool setSettings(EditState& state, const Json& op, const std::string& path, std::string& error) {
    PoseAnimationSettings& settings = state.data.settings;
    std::vector<std::string> fields;
    if (op.contains("loop")) {
        if (!readBool(op["loop"], path + ".loop", settings.loop, error)) return false;
        fields.push_back("loop");
    }
    if (op.contains("frame_rate")) {
        float rate = 0.0f;
        if (!readNumber(op["frame_rate"], path + ".frame_rate", rate, error)) return false;
        settings.frameRate = static_cast<int>(std::lround(std::max(-1.0e9f, std::min(1.0e9f, rate))));
        if (static_cast<float>(settings.frameRate) != rate) state.warnings.push_back(path + ".frame_rate rounded to " + std::to_string(settings.frameRate));
        fields.push_back("frame_rate");
    }
    if (op.contains("transition")) {
        if (!readTransition(op["transition"], path + ".transition", settings.transition, error)) return false;
        fields.push_back("transition");
    }
    if (op.contains("step_rate")) {
        if (!readNumber(op["step_rate"], path + ".step_rate", settings.stepRate, error)) return false;
        fields.push_back("step_rate");
    }
    if (op.contains("overlap")) {
        if (!readNumber(op["overlap"], path + ".overlap", settings.overlap, error)) return false;
        fields.push_back("overlap");
    }
    if (op.contains("looseness")) {
        if (!readNumber(op["looseness"], path + ".looseness", settings.looseness, error)) return false;
        fields.push_back("looseness");
    }
    if (op.contains("weights")) {
        const Json& weights = op["weights"];
        if (!readObject(weights, path + ".weights", error) ||
            !checkKeys(weights, path + ".weights", {"spine", "head", "arms", "legs"}, error)) return false;
        for (int g = 0; g < static_cast<int>(BodyGroup::Count); g++) {
            const char* name = bodyGroupName(static_cast<BodyGroup>(g));
            if (weights.contains(name) && !readNumber(weights[name], path + ".weights." + name, settings.weights[g], error)) return false;
        }
        fields.push_back("weights");
    }
    if (op.contains("humanize")) {
        const Json& h = op["humanize"];
        const std::string p = path + ".humanize";
        HumanizeSettings& humanize = settings.humanize;
        if (!readObject(h, p, error) || !checkKeys(h, p, {"enabled", "joint_limits", "moving_holds", "inbetweens", "life", "seed",
                                                           "head_follow", "torso_follow", "springiness"}, error)) return false;
        if (h.contains("enabled") && !readBool(h["enabled"], p + ".enabled", humanize.enabled, error)) return false;
        if (h.contains("joint_limits") && !readBool(h["joint_limits"], p + ".joint_limits", humanize.jointLimits, error)) return false;
        if (h.contains("moving_holds") && !readNumber(h["moving_holds"], p + ".moving_holds", humanize.movingHolds, error)) return false;
        if (h.contains("inbetweens") && !readNumber(h["inbetweens"], p + ".inbetweens", humanize.inbetweens, error)) return false;
        if (h.contains("life") && !readNumber(h["life"], p + ".life", humanize.life, error)) return false;
        if (h.contains("head_follow") && !readNumber(h["head_follow"], p + ".head_follow", humanize.headFollow, error)) return false;
        if (h.contains("torso_follow") && !readNumber(h["torso_follow"], p + ".torso_follow", humanize.torsoFollow, error)) return false;
        if (h.contains("springiness") && !readNumber(h["springiness"], p + ".springiness", humanize.springiness, error)) return false;
        if (h.contains("seed")) {
            if (!h["seed"].is_number_integer() || h["seed"].get<long long>() < 0 ||
                h["seed"].get<long long>() > static_cast<long long>(ranges::SEED_MAX)) {
                error = p + ".seed must be an integer 0.." + std::to_string(ranges::SEED_MAX);
                return false;
            }
            humanize.seed = static_cast<uint32_t>(h["seed"].get<long long>());
        }
        fields.push_back("humanize");
    }
    std::string list;
    for (const std::string& field : fields) list += (list.empty() ? "" : ", ") + field;
    state.changes.push_back("set_settings: " + (list.empty() ? std::string("nothing") : list));
    return clearCustomTransitions(state, op, path, error);
}

// A model asset argument: project-relative, an existing model file inside the assets folder
bool readModelPath(Project* project, const Json& value, const std::string& path, fs::path& absolute, std::string& error) {
    if (!value.is_string() || value.get<std::string>().empty()) {
        error = path + " must be a project-relative model file path (assets/models/hero.gltf)";
        return false;
    }
    std::string why;
    if (!McpRefs::resolveProjectPath(project, value.get<std::string>(), absolute, why, true)) {
        error = path + ": " + why;
        return false;
    }
    absolute = absolute.lexically_normal();
    if (!Util::isModelFile(absolute.string())) {
        error = path + ": '" + value.get<std::string>() + "' is not a model file (.gltf, .glb, .obj)";
        return false;
    }
    if (!project->isInsideAssetsPath(project->normalizeToAssetsRelative(absolute))) {
        error = path + ": '" + value.get<std::string>() + "' is outside the assets folder (" +
                projectRel(project, project->getAssetsPath()) + "), which games cannot load";
        return false;
    }
    return true;
}

bool setPreviewModel(EditState& state, const Json& op, const std::string& path, std::string& error) {
    if (!op.contains("model")) {
        error = path + ".model is required (\"\" clears it)";
        return false;
    }
    if (op["model"].is_string() && op["model"].get<std::string>().empty()) {
        state.data.previewModel.clear();
        state.changes.push_back("set_preview_model: none");
        return true;
    }
    fs::path absolute;
    if (!readModelPath(state.project, op["model"], path + ".model", absolute, error)) return false;
    state.data.previewModel = state.project->normalizeToAssetsRelative(absolute).generic_string();
    state.changes.push_back("set_preview_model: " + projectRel(state.project, absolute));
    return true;
}

// The sidecar of a model as this call sees it: the edited copy, else the file (missing = all auto)
bool sidecarFor(EditState& state, const fs::path& model, SidecarEdit*& out, const std::string& path, std::string& error) {
    fs::path sidecar = model;   // humanoidSidecarPath: "<model file>.humanoid"
    sidecar += ".humanoid";
    const std::string key = sidecar.generic_string();
    auto it = state.sidecars.find(key);
    if (it == state.sidecars.end()) {
        SidecarEdit edit;
        edit.path = sidecar;
        edit.model = projectRel(state.project, model);
        std::error_code ec;
        if (fs::exists(sidecar, ec)) {
            std::string text;
            std::vector<std::string> warnings;
            std::string why;
            if (!ProjectChecks::readFile(sidecar, text) || !parseHumanoidOverrides(text, edit.overrides, warnings, &why)) {
                error = path + ": the sidecar " + projectRel(state.project, sidecar) + " cannot be read" + (why.empty() ? "" : ": " + why);
                return false;
            }
            for (const std::string& warning : warnings) state.warnings.push_back(projectRel(state.project, sidecar) + ": " + warning);
        }
        it = state.sidecars.emplace(key, std::move(edit)).first;
    }
    out = &it->second;
    return true;
}

// The nearest bone name (case-insensitive), "" when none is close
std::string nearestBone(const std::string& name, const std::vector<std::string>& bones) {
    std::string best;
    size_t bestDistance = std::max<size_t>(3, name.size() / 2) + 1;
    for (const std::string& bone : bones) {
        const size_t distance = editDistance(lower(name), lower(bone));
        if (distance < bestDistance) {
            bestDistance = distance;
            best = bone;
        }
    }
    return best;
}

bool setHumanoid(EditState& state, const Json& op, const std::string& path, std::string& error) {
    if (!op.contains("model") || !op.contains("bones")) {
        error = path + ": model and bones are required";
        return false;
    }
    fs::path model;
    if (!readModelPath(state.project, op["model"], path + ".model", model, error)) return false;
    const Json& bones = op["bones"];
    if (!readObject(bones, path + ".bones", error)) return false;
    SidecarEdit* edit = nullptr;
    if (!sidecarFor(state, model, edit, path, error)) return false;
    // the model's bone names (they do not depend on the sidecar)
    const std::shared_ptr<const PoseRigInfo> info = PoseRigProvider::forModelFile(state.project, model);
    if (info->boneNames.empty()) {
        state.warnings.push_back(path + ": bone names not checked: " + info->error);
    }
    std::vector<std::string> set;
    for (auto it = bones.begin(); it != bones.end(); ++it) {
        HumanBone bone;
        if (!humanBoneFromName(it.key(), bone)) {
            error = unknownSlotError(path + ".bones", it.key());
            return false;
        }
        if (it.value().is_null()) {
            edit->overrides.bones[boneIndex(bone)].reset();
            set.push_back(it.key() + " auto");
        } else if (it.value().is_string()) {
            const std::string name = it.value().get<std::string>();
            if (!name.empty() && !info->boneNames.empty() &&
                std::find(info->boneNames.begin(), info->boneNames.end(), name) == info->boneNames.end()) {
                const std::string close = nearestBone(name, info->boneNames);
                error = path + ".bones." + it.key() + ": the model has no bone '" + name + "'" +
                        (close.empty() ? "" : " (did you mean '" + close + "'?)");
                return false;
            }
            edit->overrides.bones[boneIndex(bone)] = name;
            set.push_back(it.key() + (name.empty() ? " unmapped" : " = " + name));
        } else {
            error = path + ".bones." + it.key() + " must be a bone name, \"\" (unmapped) or null (auto-detect)";
            return false;
        }
    }
    std::string list;
    for (const std::string& item : set) list += (list.empty() ? "" : ", ") + item;
    state.changes.push_back("set_humanoid " + edit->model + ": " + (list.empty() ? std::string("nothing") : list));
    return true;
}

bool mirrorPose(EditState& state, const Json& op, const std::string& path, std::string& error) {
    std::vector<PoseKey>& poses = state.data.poses;
    if (!op.contains("index")) {
        error = path + ".index is required";
        return false;
    }
    int index = 0;
    if (!readIndex(op["index"], path + ".index", poses.size(), index, error)) return false;
    bool asNew = false;
    if (op.contains("as_new") && !readBool(op["as_new"], path + ".as_new", asNew, error)) return false;
    if (op.contains("name") && !op["name"].is_string()) {
        error = path + ".name must be a string";
        return false;
    }
    PoseKey key = poses[index];
    key.body = mirrorBodyPose(key.body);
    size_t at = static_cast<size_t>(index);
    if (asNew) {
        if (poses.size() >= static_cast<size_t>(MAX_POSES)) {
            error = path + ": an animation has at most " + std::to_string(MAX_POSES) + " poses";
            return false;
        }
        key.name = op.contains("name") ? op["name"].get<std::string>() : key.name + " (mirrored)";
        at = static_cast<size_t>(index + 1);
        poses.insert(poses.begin() + at, key);
        state.touched.insert(state.touched.begin() + at, static_cast<uint8_t>(TOUCHED | ANGLES_SET));
        state.changes.push_back("mirror_pose " + std::to_string(index) + ": mirrored copy \"" + key.name + "\" at " + std::to_string(at));
    } else {
        if (op.contains("name")) key.name = op["name"].get<std::string>();
        poses[at] = key;
        state.touched[at] |= TOUCHED | ANGLES_SET;
        state.changes.push_back("mirror_pose " + std::to_string(index) + " \"" + key.name + "\": mirrored in place");
    }
    return true;
}

bool applyPreset(EditState& state, const Json& op, const std::string& path, std::string& error) {
    std::string names;
    for (const FeelPreset& preset : feelPresets()) names += (names.empty() ? "" : ", ") + std::string(preset.name);
    if (!op.contains("name") || !op["name"].is_string()) {
        error = path + ".name is required: " + names;
        return false;
    }
    const FeelPreset* preset = findFeelPreset(op["name"].get<std::string>());
    if (!preset) {
        error = path + ".name: unknown preset '" + op["name"].get<std::string>() + "'; presets: " + names;
        return false;
    }
    applyFeelPreset(*preset, state.data.settings);
    state.changes.push_back(std::string("apply_preset: ") + preset->name);
    return clearCustomTransitions(state, op, path, error);
}

bool autoTimingOp(EditState& state, const std::string& path, std::string& error) {
    if (state.data.poses.size() < 2) {
        error = path + ": auto_timing needs at least two poses";
        return false;
    }
    const RigUse rig = previewRig(state.project, state.data);
    if (!rig.info || !rig.info->rig) {
        error = path + ": auto_timing measures the motion on the preview model's rig: " +
                (rig.info ? rig.info->error : std::string("set_preview_model first"));
        return false;
    }
    const std::shared_ptr<PoseRealisation> realised = PoseAnimationDocument::realiseData(state.data, rig.info->rig);
    const double before = buildTimeline(state.data).length;
    autoTiming(state.data, realised->keys, *rig.info->rig);
    for (uint8_t& mark : state.touched) mark |= TOUCHED;
    state.changes.push_back("auto_timing: holds and transitions from the motion (length " + fmt(before) + " -> " +
                            fmt(buildTimeline(state.data).length) + " s)");
    return true;
}

// ------------------------------------------------------------------ templates (phase 6, section 5)
// apply_template / save_template ops, create.template and get_pose_animation {templates: true};
// built-in and project templates are editor/pose/PoseTemplates. A saved template is written in
// the call's undo step (an extra file of the document commit).

// The template named by `value`; `path` names the argument in errors
bool loadTemplateArg(EditState& state, const Json& value, const std::string& path, PoseTemplate& out, std::string& error) {
    if (!value.is_string()) {
        error = path + " must be a template name (get_pose_animation {templates: true} lists them)";
        return false;
    }
    if (!PoseTemplates::load(state.project->getProjectPath(), value.get<std::string>(), out, &error)) {
        error = path + ": " + error;
        return false;
    }
    return true;
}

bool applyTemplateOp(EditState& state, const Json& op, const std::string& path, std::string& error) {
    if (!op.contains("name")) {
        error = path + ".name is required: the template (get_pose_animation {templates: true} lists them)";
        return false;
    }
    PoseTemplate found;
    if (!loadTemplateArg(state, op["name"], path + ".name", found, error)) return false;
    const std::string mode = op.contains("mode") && op["mode"].is_string() ? op["mode"].get<std::string>() : "replace";
    if (mode != "replace" && mode != "append") {
        error = path + ".mode must be \"replace\" (settings and poses) or \"append\" (poses after the existing ones)";
        return false;
    }
    const bool append = mode == "append";
    const size_t first = append ? state.data.poses.size() : 0;
    if (first + found.data.poses.size() > static_cast<size_t>(MAX_POSES)) {
        error = path + ": an animation has at most " + std::to_string(MAX_POSES) + " poses";
        return false;
    }
    PoseTemplates::apply(found.data, state.data, append ? PoseTemplateApply::Append : PoseTemplateApply::Replace);
    state.touched.resize(first);
    state.touched.resize(state.data.poses.size(), static_cast<uint8_t>(TOUCHED | ANGLES_SET));
    state.changes.push_back("apply_template \"" + found.info.name + "\" (" + mode + "): " +
                            std::to_string(found.data.poses.size()) + " pose(s)" +
                            (append ? " added at " + std::to_string(first) : std::string(", settings and poses replaced")));
    return true;
}

bool saveTemplateOp(EditState& state, const Json& op, const std::string& path, std::string& error) {
    for (const char* key : {"name", "description", "category"}) {
        if (op.contains(key) && !op[key].is_string()) {
            error = path + "." + key + " must be a string";
            return false;
        }
    }
    if (!op.contains("name")) {
        error = path + ".name is required: the template name";
        return false;
    }
    const std::string name = op["name"].get<std::string>();
    if (!PoseTemplates::validName(name, &error)) {
        error = path + ".name: " + error;
        return false;
    }
    if (state.data.poses.empty()) {
        error = path + ": the animation has no poses to save";
        return false;
    }
    // what this call's commit holds at this point: joint limits on the poses it set
    PoseAnimationData data = state.data;
    if (data.settings.humanize.jointLimits) {
        for (size_t i = 0; i < data.poses.size() && i < state.touched.size(); i++) {
            if (state.touched[i] & ANGLES_SET) clampToLimits(data.poses[i].body);
        }
    }
    const fs::path file = PoseTemplates::userFile(state.project->getProjectPath(), name);
    const PoseAnimationTemplateInfo info{name, op.value("description", std::string()), op.value("category", std::string("User"))};
    state.templateFiles[file.lexically_normal().u8string()] = PoseTemplates::fileText(data, info);
    state.changes.push_back("save_template \"" + name + "\": " + std::to_string(data.poses.size()) + " pose(s) -> " +
                            projectRel(state.project, file));
    return true;
}

Json templateListJson(Project* project) {
    Json list = Json::array();
    for (const PoseTemplateInfo& info : PoseTemplates::list(project->getProjectPath())) {
        list.push_back({{"name", info.name}, {"category", info.category}, {"description", info.description},
                        {"poses", info.poseCount}, {"loop", info.loop}, {"user", info.user}});
    }
    return list;
}

// ------------------------------------------------------------------ ops dispatch

bool applyOp(EditState& state, const Json& op, size_t opIndex, std::string& error) {
    const std::string path = "ops[" + std::to_string(opIndex) + "]";
    if (!op.is_object() || !op.contains("op") || !op["op"].is_string()) {
        error = path + " must be an object with \"op\": " + [] {
            std::string names;
            for (const std::string& name : opNames()) names += (names.empty() ? "" : ", ") + name;
            return names;
        }();
        return false;
    }
    const std::string name = op["op"].get<std::string>();
    auto it = opArguments().find(name);
    if (it == opArguments().end()) {
        const std::string close = nearest(name, opNames());
        std::string names;
        for (const std::string& n : opNames()) names += (names.empty() ? "" : ", ") + n;
        error = path + ".op: unknown op '" + name + "'" + (close.empty() ? "" : " (did you mean '" + close + "'?)") + "; ops: " + names;
        return false;
    }
    if (!checkKeys(op, path + " (" + name + ")", it->second, error)) return false;
    if (name == "add_pose") return addPose(state, op, path, error);
    if (name == "update_pose") return updatePose(state, op, path, error);
    if (name == "delete_pose") return deletePose(state, op, path, error);
    if (name == "move_pose") return movePose(state, op, path, error);
    if (name == "mirror_pose") return mirrorPose(state, op, path, error);
    if (name == "set_settings") return setSettings(state, op, path, error);
    if (name == "apply_preset") return applyPreset(state, op, path, error);
    if (name == "auto_timing") return autoTimingOp(state, path, error);
    if (name == "set_preview_model") return setPreviewModel(state, op, path, error);
    if (name == "apply_template") return applyTemplateOp(state, op, path, error);
    if (name == "save_template") return saveTemplateOp(state, op, path, error);
    return setHumanoid(state, op, path, error);
}

// "0, 2-4" from sorted indices
std::string indexRanges(const std::vector<size_t>& indices) {
    std::string text;
    for (size_t i = 0; i < indices.size();) {
        size_t j = i;
        while (j + 1 < indices.size() && indices[j + 1] == indices[j] + 1) j++;
        text += (text.empty() ? "" : ", ") + std::to_string(indices[i]) + (j > i ? "-" + std::to_string(indices[j]) : "");
        i = j + 1;
    }
    return text;
}

// The animation's transition changed while some poses keep their own (auto_timing gives every pose
// one): those poses do not follow it
void warnOwnTransitions(EditState& state, const Transition& before) {
    const Transition& now = state.data.settings.transition;
    if (now.curve == before.curve && now.duration == before.duration && now.overshoot == before.overshoot &&
        now.anticipation == before.anticipation) return;
    std::vector<size_t> own;
    for (size_t i = 0; i < state.data.poses.size(); i++) {
        if (state.data.poses[i].customTransition) own.push_back(i);
    }
    if (own.empty()) return;
    state.warnings.push_back("poses " + indexRanges(own) + " have their own transition (auto_timing or a pose transition) and "
                             "keep it; clear with update_pose transition: null, or clear_custom: true on apply_preset / set_settings");
}

bool insideAssets(Project* project, const fs::path& absolute) {
    return project->isInsideAssetsPath(project->normalizeToAssetsRelative(absolute));
}

std::string outsideAssetsWarning(Project* project, const std::string& relative) {
    return relative + " is outside the assets folder (" + projectRel(project, project->getAssetsPath()) +
           "): a Pose Animator in a game cannot load it";
}

// humanize.joint_limits: the poses whose angles this call set (add_pose, mirror_pose, angles,
// add_angles) are clamped to the joint limits (reported); metadata-only edits are left alone
void clampTouchedToLimits(EditState& state, std::vector<ClampReport>& reports) {
    if (!state.data.settings.humanize.jointLimits) return;
    for (size_t i = 0; i < state.data.poses.size() && i < state.touched.size(); i++) {
        if (!(state.touched[i] & ANGLES_SET)) continue;
        BodyPose& body = state.data.poses[i].body;
        const BodyPose before = body;
        for (int dof : clampToLimits(body)) {
            reports.push_back({"poses[" + std::to_string(i) + "].angles." + dofName(dof), before.angles[dof], body.angles[dof]});
        }
    }
}

Json clampJson(const std::vector<ClampReport>& reports) {
    Json out = Json::array();
    for (const ClampReport& report : reports) {
        out.push_back({{"field", report.field}, {"requested", r4(report.requested)}, {"applied", r4(report.applied)}});
    }
    return out;
}

bool resolveAnimationPath(Project* project, const Json& args, std::string& relative, fs::path& absolute, std::string& error) {
    if (!args.contains("path") || !args["path"].is_string()) {
        error = "path is required: the project-relative .poseanim file (e.g. assets/anim/walk.poseanim).";
        return false;
    }
    relative = args["path"].get<std::string>();
    if (lower(fs::u8path(relative).extension().string()) != ".poseanim") {
        error = "path: '" + relative + "' must end in .poseanim";
        return false;
    }
    if (!McpRefs::resolveProjectPath(project, relative, absolute, error, false)) return false;
    absolute = absolute.lexically_normal();
    relative = projectRel(project, absolute);
    return true;
}

void installSyncHook(McpContext& ctx) {
    McpContext* context = &ctx;
    PoseAnimationDocument::setFilesWrittenHook([context](const std::vector<fs::path>& changed) {
        w4::syncFiles(*context, changed, false);
    });
}

// A model GameObject of `scene` (the selected scene when omitted)
bool resolveModelEntity(Project* project, const Json& args, const Json& ref, const std::string& path,
                        SceneProject*& scene, Entity& entity, std::string& error) {
    scene = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!scene) {
        error = "scene: " + error;
        return false;
    }
    if (!scene->scene) {
        error = McpRefs::notOpenError(project, *scene);
        return false;
    }
    entity = McpRefs::resolveEntity(scene, ref, error);
    if (entity == NULL_ENTITY) {
        error = path + ": " + error;
        return false;
    }
    if (!scene->scene->findComponent<ModelComponent>(entity)) {
        error = path + ": \"" + scene->scene->getEntityName(entity) + "\" is not a model GameObject (no Model component)";
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ get_pose_animation

void getPoseAnimationTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    installSyncHook(call->ctx());
    if (args.contains("templates")) {
        // the template list (phase 6)
        if (!args["templates"].is_boolean() || (args["templates"].get<bool>() && args.contains("path"))) {
            call->fail("templates: true lists the pose templates; leave out path (and give path without templates to read an animation).");
            return;
        }
        if (args["templates"].get<bool>()) {
            const Json list = templateListJson(project);
            std::string text = std::to_string(list.size()) + " pose template(s); use one with edit_pose_animation create: {template} "
                               "or the op apply_template:";
            for (const Json& t : list) {
                text += "\n" + t["name"].get<std::string>() + " (" + t["category"].get<std::string>() + ", " +
                        std::to_string(t["poses"].get<size_t>()) + " poses, " + (t["loop"].get<bool>() ? "loop" : "once") +
                        (t["user"].get<bool>() ? ", project" : "") + "): " + t["description"].get<std::string>();
            }
            call->ok(text, Json{{"templates", list}});
            return;
        }
    }
    std::string relative, error;
    fs::path absolute;
    if (!resolveAnimationPath(project, args, relative, absolute, error)) {
        call->fail(error);
        return;
    }
    const std::string detail = args.value("detail", std::string("full"));
    if (detail != "full" && detail != "summary") {
        call->fail("detail must be \"full\" or \"summary\".");
        return;
    }
    std::shared_ptr<PoseAnimationDocument> document = PoseAnimationDocument::open(project, relative, &error);
    if (!document) {
        call->fail(error + ". Create one with edit_pose_animation {path, create: {preview_model}}.");
        return;
    }
    RigUse rig;
    if (args.contains("model")) {
        SceneProject* scene = nullptr;
        Entity entity = NULL_ENTITY;
        if (!resolveModelEntity(project, args, args["model"], "model", scene, entity, error)) {
            call->fail(error);
            return;
        }
        rig.info = PoseRigProvider::forSceneModel(project, scene->scene, entity);
        rig.model = McpRefs::entityJson(scene, entity);
    } else {
        rig = previewRig(project, document->data());
    }
    const size_t count = document->data().poses.size();
    std::vector<size_t> indices;
    if (args.contains("poses")) {
        if (!args["poses"].is_array()) {
            call->fail("poses must be an array of pose indices.");
            return;
        }
        for (size_t i = 0; i < args["poses"].size(); i++) {
            int index = 0;
            if (!readIndex(args["poses"][i], "poses[" + std::to_string(i) + "]", count, index, error)) {
                call->fail(error);
                return;
            }
            if (std::find(indices.begin(), indices.end(), static_cast<size_t>(index)) == indices.end()) indices.push_back(static_cast<size_t>(index));
        }
    } else {
        for (size_t i = 0; i < count; i++) indices.push_back(i);
    }
    Json out = animationJson(project, *document, rig, indices, detail == "full");
    if (!insideAssets(project, absolute)) {
        if (!out.contains("warnings")) out["warnings"] = Json::array();
        out["warnings"].push_back(outsideAssetsWarning(project, document->path()));
    }
    std::string text = document->path() + ": " + std::to_string(count) + " pose(s), " + fmt(out["length"].get<double>()) + " s, " +
                       (out["loop"].get<bool>() ? "loop" : "one-shot") + ", " + std::to_string(out["frame_rate"].get<int>()) + " fps, preview " +
                       (out["preview_model"].get<std::string>().empty() ? std::string("none") : out["preview_model"].get<std::string>());
    if (out["preset"].is_string()) text += ", feel " + out["preset"].get<std::string>();
    if (rig.info && !rig.info->rig) text += "\nno metrics: " + rig.info->error;
    if (out.contains("warnings")) {
        for (const Json& warning : out["warnings"]) text += "\nwarning: " + warning.get<std::string>();
    }
    for (const Json& pose : out["poses"]) text += "\n" + poseLine(pose);
    call->ok(text, out);
}

// ------------------------------------------------------------------ edit_pose_animation

// bake without path: the animation the model's Pose Animator plays, as its Inspector Bake button does
void bakeAnimatorTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string error;
    const Json& bake = args["bake"];
    if (args.contains("ops") || args.contains("create")) {
        call->fail("ops and create need path: the .poseanim file to edit.");
        return;
    }
    if (!readObject(bake, "bake", error) || !checkKeys(bake, "bake", {"model"}, error)) {
        call->fail(error + " (without path the bake uses the Pose Animator's animation and its file name; pass path to choose a name)");
        return;
    }
    if (!bake.contains("model")) {
        call->fail("bake.model is required: the model GameObject to bake on.");
        return;
    }
    SceneProject* scene = nullptr;
    Entity model = NULL_ENTITY;
    if (!resolveModelEntity(project, args, bake["model"], "bake.model", scene, model, error)) {
        call->fail(error);
        return;
    }
    const PoseAnimatorComponent* animator = scene->scene->findComponent<PoseAnimatorComponent>(model);
    if (!animator || animator->animation.empty()) {
        call->fail("bake.model: \"" + scene->scene->getEntityName(model) + "\" has no Pose Animator with an animation; pass path.");
        return;
    }
    PoseBakeResult result;
    if (!PoseBaker::bakeAnimatorAsset(project, scene->id, model, animator->animation, result, error)) {
        call->fail("bake: " + error);
        return;
    }
    Json out = {{"bake", {{"animation", McpRefs::entityJson(scene, result.animation)}, {"name", result.name},
                          {"created", result.created}, {"frames", result.frames}, {"tracks", result.tracks},
                          {"length", r4(result.length)}, {"source", animator->animation}}}};
    call->ok(PoseBaker::bakeStatusLine(animator->animation, result) + " (scene undo step)", out);
}

void editPoseAnimationTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    installSyncHook(call->ctx());
    if (!args.contains("path") && args.contains("bake")) {
        bakeAnimatorTool(call);
        return;
    }
    std::string relative, error;
    fs::path absolute;
    if (!resolveAnimationPath(project, args, relative, absolute, error)) {
        call->fail(error);
        return;
    }
    const bool creating = args.contains("create");
    const Json ops = args.contains("ops") ? args["ops"] : Json::array();
    if (!ops.is_array()) {
        call->fail("ops must be an array of {op, ...} objects.");
        return;
    }
    if (!creating && ops.empty() && !args.contains("bake")) {
        call->fail("Nothing to do: give ops, create (a new file) and/or bake.");
        return;
    }

    // the bake target is checked before anything is written
    SceneProject* bakeScene = nullptr;
    Entity bakeModel = NULL_ENTITY;
    std::string bakeName;
    if (args.contains("bake")) {
        const Json& bake = args["bake"];
        if (!readObject(bake, "bake", error) || !checkKeys(bake, "bake", {"model", "name"}, error)) {
            call->fail(error);
            return;
        }
        if (!bake.contains("model")) {
            call->fail("bake.model is required: the model GameObject to bake on.");
            return;
        }
        if (bake.contains("name")) {
            if (!bake["name"].is_string() || bake["name"].get<std::string>().empty()) {
                call->fail("bake.name must be a non-empty string (default: the file name).");
                return;
            }
            bakeName = bake["name"].get<std::string>();
        }
        if (!resolveModelEntity(project, args, bake["model"], "bake.model", bakeScene, bakeModel, error)) {
            call->fail(error);
            return;
        }
        if (bakeScene->playState != ScenePlayState::STOPPED) {
            call->fail("bake: the scene is playing; stop play mode before baking.");
            return;
        }
        const std::shared_ptr<const PoseRigInfo> info = PoseRigProvider::forSceneModel(project, bakeScene->scene, bakeModel);
        if (!info->rig) {
            call->fail("bake.model: " + info->error);
            return;
        }
    }

    EditState state;
    state.project = project;
    std::shared_ptr<PoseAnimationDocument> document;
    if (creating) {
        const Json& create = args["create"];
        if (!readObject(create, "create", error) || !checkKeys(create, "create", {"preview_model", "preset", "template"}, error)) {
            call->fail(error);
            return;
        }
        std::error_code ec;
        if (fs::exists(absolute, ec)) {
            call->fail("create: " + relative + " already exists; leave out create to edit it.");
            return;
        }
        std::string fromTemplate;
        if (create.contains("template")) {
            PoseTemplate found;
            if (!loadTemplateArg(state, create["template"], "create.template", found, error)) {
                call->fail(error);
                return;
            }
            state.data = PoseTemplates::newAnimation(found.data, "");
            fromTemplate = found.info.name;
        }
        if (create.contains("preview_model")) {
            if (!setPreviewModel(state, {{"op", "set_preview_model"}, {"model", create["preview_model"]}}, "create", error)) {
                // the error names "create.model": the argument is preview_model
                const std::string from = "create.model";
                if (error.rfind(from, 0) == 0) error = "create.preview_model" + error.substr(from.size());
                call->fail(error);
                return;
            }
            state.changes.clear();
        }
        if (create.contains("preset")) {
            if (!applyPreset(state, {{"op", "apply_preset"}, {"name", create["preset"]}}, "create", error)) {
                const std::string from = "create.name";
                if (error.rfind(from, 0) == 0) error = "create.preset" + error.substr(from.size());
                call->fail(error);
                return;
            }
            state.changes.clear();
        }
        state.changes.push_back("created " + relative + (state.data.previewModel.empty() ? std::string("") :
                                " (preview " + previewModelPath(project, state.data) + ")") +
                                (create.contains("preset") ? ", feel " + create["preset"].get<std::string>() : std::string("")) +
                                (fromTemplate.empty() ? std::string("") : ", from the template \"" + fromTemplate + "\" (" +
                                 std::to_string(state.data.poses.size()) + " poses)"));
    } else {
        document = PoseAnimationDocument::open(project, relative, &error);
        if (!document) {
            call->fail(error + ". Pass create: {preview_model} to make a new one.");
            return;
        }
        state.data = document->data();
    }
    state.touched.assign(state.data.poses.size(), 0);
    const Transition initialTransition = state.data.settings.transition;

    // all ops on the copy; the first failing one fails the call with nothing written
    for (size_t i = 0; i < ops.size(); i++) {
        if (!applyOp(state, ops[i], i, error)) {
            call->fail(error + (ops.size() > 1 ? " (nothing was changed: ops are all or nothing)" : ""));
            return;
        }
    }
    if (bakeModel != NULL_ENTITY && state.data.poses.empty()) {
        call->fail("bake: the animation has no poses (nothing was changed).");
        return;
    }
    std::vector<ClampReport> clamped;
    clampTouchedToLimits(state, clamped);
    warnOwnTransitions(state, initialTransition);
    if (document) {
        for (const std::string& warning : document->loadWarnings()) state.warnings.push_back(relative + ": " + warning);
    }
    if (!insideAssets(project, absolute)) state.warnings.push_back(outsideAssetsWarning(project, relative));

    std::vector<PoseAnimationDocument::ExtraFile> extras;
    bool extrasChanged = false;
    for (const auto& [key, sidecar] : state.sidecars) {
        extras.push_back({sidecar.path, writeHumanoidOverrides(sidecar.overrides)});
        std::string existing;
        std::error_code ec;
        if (!fs::is_regular_file(sidecar.path, ec) || !ProjectChecks::readFile(sidecar.path, existing) || existing != extras.back().content) {
            extrasChanged = true;
        }
    }
    for (const auto& [file, text] : state.templateFiles) {
        extras.push_back({fs::u8path(file), text});
        std::string existing;
        std::error_code ec;
        if (!fs::is_regular_file(extras.back().path, ec) || !ProjectChecks::readFile(extras.back().path, existing) || existing != text) {
            extrasChanged = true;
        }
    }

    const uint64_t before = document ? document->revision() : 0;
    if (creating) {
        document = PoseAnimationDocument::create(project, relative, state.data, "Create Pose Animation", &clamped, &error, extras);
        if (!document) {
            call->fail(error);
            return;
        }
    } else if (!document->commit(state.data, "Edit Pose Animation", "", &clamped, &error, extras)) {
        call->fail(error);
        return;
    }
    const bool changed = creating || document->revision() != before || extrasChanged;

    std::vector<size_t> touched;
    for (size_t i = 0; i < state.touched.size() && i < document->data().poses.size(); i++) {
        if (state.touched[i] & TOUCHED) touched.push_back(i);
    }
    const Json full = animationJson(project, *document, previewRig(project, document->data()), touched, true);
    Json out = {{"path", document->path()}, {"pose_count", document->data().poses.size()}, {"length", full["length"]},
                {"changes", state.changes}, {"clamped", clampJson(clamped)}, {"warnings", state.warnings},
                {"poses", full["poses"]}};
    if (!full["preset"].is_null()) out["preset"] = full["preset"];
    std::string text = document->path() + ": " + std::to_string(document->data().poses.size()) + " pose(s), " +
                       fmt(full["length"].get<double>()) + " s" + (changed ? "" : " (no change: nothing written)");
    for (const std::string& line : state.changes) text += "\n" + line;
    for (const ClampReport& report : clamped) text += "\nclamped " + describeClamp(report);
    for (const std::string& warning : state.warnings) text += "\nwarning: " + warning;
    for (const Json& pose : full["poses"]) text += "\n" + poseLine(pose);

    if (bakeModel != NULL_ENTITY) {
        PoseBakeResult result;
        if (!PoseBaker::bake(project, bakeScene->id, bakeModel, *document, bakeName, result, error)) {
            out["bake_error"] = error;
            text += "\nbake failed: " + error + (changed ? " (the edits above were saved: undo them with scope project)" : "");
            call->fail(text, out);
            return;
        }
        out["bake"] = {{"animation", McpRefs::entityJson(bakeScene, result.animation)}, {"name", result.name},
                       {"created", result.created}, {"frames", result.frames}, {"tracks", result.tracks}, {"length", r4(result.length)}};
        text += "\n" + std::string(result.created ? "baked" : "re-baked") + " to the Animation \"" + result.name + "\" of \"" +
                bakeScene->scene->getEntityName(bakeModel) + "\": " + std::to_string(result.tracks) + " tracks of " +
                std::to_string(result.frames) + " keys (scene undo step; play it with Model:playAnimation(\"" + result.name + "\"))";
    }
    call->ok(text, out);
}

// A nested object schema: no additionalProperties (the handler checks the keys, and tool
// definitions are paid for in every agent session)
Json obj(const Json& properties, const std::vector<std::string>& required = {}) {
    Json out = {{"type", "object"}, {"properties", properties}};
    if (!required.empty()) out["required"] = required;
    return out;
}

} // namespace

// ================================================================== render_pose_animation
// (spec: pose animation phase 3, section 4) Contact sheets of key poses or of the motion
// (the frames of the bake and the runtime, pose::frames sampled as the runtime plays them),
// one row per view, rendered offscreen by PoseSheetRenderer without blocking the editor.

namespace {

using namespace ::jasperengine::pose;

constexpr int RENDER_MAX_COLUMNS = 24;
constexpr int RENDER_ROW_COLUMNS = 12;
constexpr int RENDER_MAX_SHEET = 2048;
constexpr int RENDER_TIMEOUT_MS = 120000;
constexpr size_t RENDER_KEPT_RENDERERS = 2;

struct RenderColumn {
    HumanPose pose;
    Json key;               // {index, label} or {time, label}
    std::string label;
    Json metrics;
    std::string line;       // the caption's metrics summary
};

struct RenderJob {
    std::shared_ptr<PoseSheetRenderer> renderer;   // alive until every cell came back
    std::vector<PoseImage> cells;                  // per sheet slot (row * columns + column)
    std::vector<PoseSheetRenderer::PoseJoints> joints;   // per sheet slot, when asked
    size_t remaining = 0;
    bool failed = false;
};

// Sheet renderers kept warm between calls, per model file, least recently used first.
// Deliberately never destroyed (the editor frees their GPU objects when the view goes away): a
// PoseRender owns a scene that must not outlive the engine.
std::vector<std::pair<std::string, std::shared_ptr<PoseSheetRenderer>>>& sheetRenderers() {
    static auto* renderers = new std::vector<std::pair<std::string, std::shared_ptr<PoseSheetRenderer>>>();
    return *renderers;
}

std::shared_ptr<PoseSheetRenderer> sheetRendererFor(const std::string& modelFile, const std::shared_ptr<PoseRig>& rig,
                                                    std::string& error) {
    auto& renderers = sheetRenderers();
    auto it = std::find_if(renderers.begin(), renderers.end(), [&](const auto& entry) { return entry.first == modelFile; });
    std::shared_ptr<PoseSheetRenderer> renderer;
    // the same rig, or a scene model's rig of the same skeleton and map: the loaded model serves
    if (it != renderers.end() && it->second->sameRig(rig) && it->second->setModel(modelFile, rig, &error)) {
        renderer = it->second;
    } else {
        // a new one (a call still drawing with the old rig keeps the old renderer alive)
        renderer = std::make_shared<PoseSheetRenderer>();
        if (!renderer->setModel(modelFile, rig, &error)) return nullptr;
    }
    if (it != renderers.end()) renderers.erase(it);
    while (renderers.size() >= RENDER_KEPT_RENDERERS) renderers.erase(renderers.begin());
    renderers.emplace_back(modelFile, renderer);
    return renderer;
}

// One caption hint per view: where the character's sides are
std::string viewHint(const std::string& view) {
    if (view == "front") return "front (faces you: its left is on image right)";
    if (view == "side") return "side (faces image right, near side = right, cyan)";
    if (view == "three_quarter") return "three_quarter (from its front-right: faces image right-ish, near side = right)";
    if (view == "back") return "back (its left is on image left)";
    if (view == "top") return "top (from above: forward is up, its left is on image left)";
    return view;
}

std::string rigFailure(const PoseRigInfo& info) {
    std::string text = info.error.empty() ? std::string("the model is not a usable humanoid") : info.error;
    if (!info.missing.empty()) {
        std::string slots;
        for (const std::string& slot : info.missing) slots += (slots.empty() ? "" : ", ") + slot;
        text += ". Missing slots: " + slots + ". Map them with edit_pose_animation ops [{op: \"set_humanoid\", model: \"" +
                info.model + "\", bones: {slot: bone name}}]";
    }
    return text;
}

std::string metricsLine(const PoseMetrics& m) {
    return "feet L " + fmt(m.left.height) + " R " + fmt(m.right.height) +
           (m.balanced ? ", balanced" : ", unbalanced (" + fmt(m.balanceMargin, 3) + ")") +
           (m.limitHits.empty() ? std::string("") : ", " + std::to_string(m.limitHits.size()) + " limit hit(s)");
}

bool readViews(const Json& args, const std::string& mode, std::vector<PoseView>& views, std::vector<std::string>& names,
               std::string& error) {
    Json list = args.contains("views") ? args["views"] : (mode == "poses" ? Json::array({"front", "side"}) : Json::array({"side"}));
    if (!list.is_array() || list.empty()) {
        error = "views must be a non-empty array of front, side, three_quarter, back, top";
        return false;
    }
    for (size_t i = 0; i < list.size(); i++) {
        PoseView view;
        const std::string name = list[i].is_string() ? list[i].get<std::string>() : std::string();
        if (!PoseView::fromName(name, view) || view.kind == PoseView::Custom) {
            error = "views[" + std::to_string(i) + "]: '" + (list[i].is_string() ? name : list[i].dump()) +
                    "' is not a view (front, side, three_quarter, back, top)";
            return false;
        }
        if (std::find(names.begin(), names.end(), name) != names.end()) {
            error = "views[" + std::to_string(i) + "]: '" + name + "' is listed twice";
            return false;
        }
        views.push_back(view);
        names.push_back(name);
    }
    return true;
}

// The columns of the sheet: key poses, evenly spaced motion frames or frames at given times
bool readColumns(const Json& args, const std::string& mode, const PoseAnimationData& data, const PoseRig& rig,
                 const PoseRealisation& realised, std::vector<RenderColumn>& columns, std::string& note, std::string& error) {
    const size_t count = data.poses.size();
    if (mode == "poses") {
        std::vector<size_t> indices;
        if (args.contains("poses")) {
            if (!args["poses"].is_array() || args["poses"].empty()) {
                error = "poses must be a non-empty array of pose indices";
                return false;
            }
            if (args["poses"].size() > static_cast<size_t>(RENDER_MAX_COLUMNS)) {
                error = "poses: at most " + std::to_string(RENDER_MAX_COLUMNS) + " per sheet (got " +
                        std::to_string(args["poses"].size()) + ")";
                return false;
            }
            for (size_t i = 0; i < args["poses"].size(); i++) {
                int index = 0;
                if (!readIndex(args["poses"][i], "poses[" + std::to_string(i) + "]", count, index, error)) return false;
                indices.push_back(static_cast<size_t>(index));
            }
        } else {
            for (size_t i = 0; i < count && i < static_cast<size_t>(RENDER_MAX_COLUMNS); i++) indices.push_back(i);
            if (count > static_cast<size_t>(RENDER_MAX_COLUMNS)) {
                note = "only the first " + std::to_string(RENDER_MAX_COLUMNS) + " of " + std::to_string(count) +
                       " poses (pass poses for the others)";
            }
        }
        for (size_t index : indices) {
            RenderColumn column;
            column.pose = realised.keys[index];
            column.label = PoseContactSheet::poseLabel(static_cast<int>(index), data.poses[index].name);
            column.key = {{"index", index}, {"label", column.label}};
            column.metrics = metricsJson(realised.metrics[index]);
            column.line = column.label + ": " + metricsLine(realised.metrics[index]);
            columns.push_back(std::move(column));
        }
        return true;
    }

    const float length = static_cast<float>(realised.timeline.length);
    const bool loop = data.settings.loop;
    std::vector<float> times;
    if (mode == "motion") {
        int frames = 12;
        if (args.contains("frames")) {
            if (!args["frames"].is_number_integer() || args["frames"].get<long long>() < 4 ||
                args["frames"].get<long long>() > RENDER_MAX_COLUMNS) {
                error = "frames must be an integer 4.." + std::to_string(RENDER_MAX_COLUMNS) + " (default 12)";
                return false;
            }
            frames = static_cast<int>(args["frames"].get<long long>());
        }
        // a loop's end is its start again: frames cover [0, length); a one-shot includes its end
        for (int k = 0; k < frames; k++) {
            times.push_back(static_cast<float>(static_cast<double>(k) * length / (loop ? frames : frames - 1)));
        }
    } else {
        if (!args.contains("times") || !args["times"].is_array() || args["times"].empty()) {
            error = "times mode needs times: an array of seconds (0.." + fmt(length) + ")";
            return false;
        }
        if (args["times"].size() > static_cast<size_t>(RENDER_MAX_COLUMNS)) {
            error = "times: at most " + std::to_string(RENDER_MAX_COLUMNS) + " (got " + std::to_string(args["times"].size()) + ")";
            return false;
        }
        for (size_t i = 0; i < args["times"].size(); i++) {
            float t = 0.0f;
            const std::string path = "times[" + std::to_string(i) + "]";
            if (!readNumber(args["times"][i], path, t, error)) return false;
            if (t < 0.0f || t > length + 1e-4f) {
                error = path + ": " + fmt(t) + " s is outside the animation (0.." + fmt(length) + " s)";
                return false;
            }
            times.push_back(std::min(t, length));
        }
    }
    const bool stepped = data.settings.stepRate > 0.0f;
    for (float t : times) {
        RenderColumn column;
        // the runtime's sampling of the baked frames
        sampleFrames(realised.frames, length, loop, stepped, t, column.pose);
        const PoseMetrics m = computeMetrics(rig, column.pose, rig.toBodyPose(column.pose));
        column.label = PoseContactSheet::timeLabel(t, m.left.contact, m.right.contact);
        column.key = {{"time", r4(t)}, {"label", column.label}};
        column.metrics = metricsJson(m);
        column.line = column.label + ": " + metricsLine(m);
        columns.push_back(std::move(column));
    }
    return true;
}

void renderPoseAnimationTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    installSyncHook(call->ctx());
    std::string relative, error;
    fs::path absolute;
    if (!resolveAnimationPath(project, args, relative, absolute, error)) {
        call->fail(error);
        return;
    }
    const std::string mode = args.value("mode", std::string("poses"));
    if (mode != "poses" && mode != "motion" && mode != "times") {
        call->fail("mode must be poses (key poses), motion (evenly spaced frames) or times (frames at times).");
        return;
    }
    for (const char* key : {"poses", "frames", "times"}) {
        const std::string owner = std::string(key) == "poses" ? "poses" : (std::string(key) == "frames" ? "motion" : "times");
        if (args.contains(key) && mode != owner) {
            call->fail(std::string(key) + " is only for mode \"" + owner + "\" (mode is \"" + mode + "\"): leave it out or set mode.");
            return;
        }
    }
    std::shared_ptr<PoseAnimationDocument> document = PoseAnimationDocument::open(project, relative, &error);
    if (!document) {
        call->fail(error + ". Create one with edit_pose_animation {path, create: {preview_model}}.");
        return;
    }
    const PoseAnimationData& data = document->data();
    if (data.poses.empty()) {
        call->fail(document->path() + " has no poses: add some with edit_pose_animation add_pose.");
        return;
    }

    // the rig and the model file drawn: a scene model's, else the preview model's
    std::shared_ptr<const PoseRigInfo> info;
    fs::path modelFile;
    Json modelJson;
    if (args.contains("model")) {
        SceneProject* scene = nullptr;
        Entity entity = NULL_ENTITY;
        if (!resolveModelEntity(project, args, args["model"], "model", scene, entity, error)) {
            call->fail(error);
            return;
        }
        info = PoseRigProvider::forSceneModel(project, scene->scene, entity);
        const std::string& file = scene->scene->getComponent<ModelComponent>(entity).filename;
        modelFile = project->resolveAssetPath(fs::u8path(file)).lexically_normal();
        modelJson = McpRefs::entityJson(scene, entity);
    } else {
        if (data.previewModel.empty()) {
            call->fail("Set a preview model: edit_pose_animation set_preview_model, or pass model");
            return;
        }
        modelFile = previewModelFile(project, data);
        info = PoseRigProvider::forModelFile(project, modelFile);
        modelJson = previewModelPath(project, data);
    }
    if (!info->rig) {
        call->fail(rigFailure(*info));
        return;
    }
    const std::shared_ptr<const PoseRealisation> realised = document->realise(info->rig);
    if (!realised) {
        call->fail("The animation could not be realised on " + info->model + ".");
        return;
    }

    std::vector<RenderColumn> columns;
    std::string note;
    if (!readColumns(args, mode, data, *info->rig, *realised, columns, note, error)) {
        call->fail(error);
        return;
    }
    std::vector<PoseView> views;
    std::vector<std::string> viewNames;
    if (!readViews(args, mode, views, viewNames, error)) {
        call->fail(error);
        return;
    }
    int cellWidth = 160, cellHeight = 200;
    if (args.contains("cell")) {
        const Json& cell = args["cell"];
        auto inRange = [](const Json& v) { return v.is_number_integer() && v.get<long long>() >= 64 && v.get<long long>() <= 512; };
        if (!cell.is_array() || cell.size() != 2 || !inRange(cell[0]) || !inRange(cell[1])) {
            call->fail("cell must be [width, height], integers 64..512 (default [160, 200]).");
            return;
        }
        cellWidth = cell[0].get<int>();
        cellHeight = cell[1].get<int>();
    }
    bool overlay = true;
    if (args.contains("overlay") && !readBool(args["overlay"], "overlay", overlay, error)) {
        call->fail(error);
        return;
    }
    bool wantJoints = false;
    if (args.contains("joints") && !readBool(args["joints"], "joints", wantJoints, error)) {
        call->fail(error);
        return;
    }
    fs::path saveTarget;
    if (args.contains("save_path")) {
        const std::string savePath = args["save_path"].is_string() ? args["save_path"].get<std::string>() : std::string();
        if (lower(fs::u8path(savePath).extension().string()) != ".png") {
            call->fail("save_path must be a project-relative .png path.");
            return;
        }
        if (!McpRefs::resolveProjectPath(project, savePath, saveTarget, error, false)) {
            call->fail("save_path: " + error);
            return;
        }
    }

    // layout: up to 12 columns a row; each block of columns has one row per view
    PoseSheetLayout layout;
    const int count = static_cast<int>(columns.size());
    const int blocks = (count + RENDER_ROW_COLUMNS - 1) / RENDER_ROW_COLUMNS;
    layout.columns = std::min(count, RENDER_ROW_COLUMNS);
    layout.rows = blocks * static_cast<int>(views.size());
    layout.labelHeight = overlay ? PoseSheetLayout::LABEL_HEIGHT : 0;
    layout.titleHeight = overlay ? PoseSheetLayout::LABEL_HEIGHT : 0;
    // cells shrink (keeping their aspect) to fit the sheet into 2048 px each way
    const int title = layout.titleHeight > 0 ? layout.titleHeight + PoseSheetLayout::SEPARATOR : 0;
    const int maxWidth = (RENDER_MAX_SHEET - (layout.columns - 1)) / layout.columns;
    const int maxHeight = (RENDER_MAX_SHEET - title - (layout.rows - 1)) / layout.rows - layout.labelHeight;
    const double shrink = std::min({1.0, static_cast<double>(maxWidth) / cellWidth, static_cast<double>(maxHeight) / cellHeight});
    if (shrink < 1.0) {
        cellWidth = std::max(16, static_cast<int>(std::floor(cellWidth * shrink)));
        cellHeight = std::max(16, static_cast<int>(std::floor(cellHeight * shrink)));
    }
    layout.cellWidth = cellWidth;
    layout.cellHeight = cellHeight;

    std::shared_ptr<PoseSheetRenderer> renderer = sheetRendererFor(modelFile.string(), info->rig, error);
    if (!renderer) {
        call->fail("model: " + error);
        return;
    }

    PoseRenderStyle style;
    style.skeletonOverlay = overlay;
    auto job = std::make_shared<RenderJob>();
    job->renderer = renderer;
    job->cells.resize(static_cast<size_t>(layout.columns * layout.rows));
    job->joints.resize(job->cells.size());
    std::vector<HumanPose> framing;
    for (const RenderColumn& column : columns) framing.push_back(column.pose);
    for (size_t v = 0; v < views.size(); v++) {
        // one framing per view over every column: the cells of a view share one scale
        renderer->beginBatch(framing, views[v]);
        for (int c = 0; c < count; c++) {
            const int row = (c / RENDER_ROW_COLUMNS) * static_cast<int>(views.size()) + static_cast<int>(v);
            const size_t slot = static_cast<size_t>(row * layout.columns + c % RENDER_ROW_COLUMNS);
            job->remaining++;
            renderer->requestCell(columns[c].pose, views[v], cellWidth, cellHeight, style, [job, slot](bool ok, PoseImage&& image) {
                if (ok) job->cells[slot] = std::move(image);
                else job->failed = true;
                job->remaining--;
            }, wantJoints ? &job->joints[slot] : nullptr);
        }
    }
    renderer->endBatches();

    std::string titleText;
    if (overlay) {
        std::string rows;
        for (const std::string& name : viewNames) rows += (rows.empty() ? "" : ", ") + name;
        titleText = "rows: " + rows + "  coral = left, cyan = right";
    }
    const std::string path = document->path();
    const bool loop = data.settings.loop;
    const double length = realised->timeline.length;
    const std::string modelName = info->model;
    call->waitUntil([job] { return job->remaining == 0; }, RENDER_TIMEOUT_MS,
                    [call, job, layout, columns, viewNames, titleText, saveTarget, project, path, mode, note, loop, length,
                     modelJson, modelName, blocks, wantJoints](bool timedOut) {
        if (timedOut || job->failed) {
            call->fail(timedOut ? "The pose render timed out (the editor drew no frames?)." : "The pose render failed (no graphics view?).");
            return;
        }
        PoseContactSheet sheet(layout, titleText);
        if (!titleText.empty()) sheet.setTitle(titleText);
        const int views = static_cast<int>(viewNames.size());
        for (int row = 0; row < layout.rows; row++) {
            for (int c = 0; c < layout.columns; c++) {
                const int column = (row / views) * RENDER_ROW_COLUMNS + c;
                if (column >= static_cast<int>(columns.size())) continue;
                sheet.setCell(c, row, job->cells[static_cast<size_t>(row * layout.columns + c)]);
                if (layout.labelHeight > 0) sheet.setLabel(c, row, columns[column].label);
            }
        }
        std::vector<uint8_t> png;
        if (!sheet.getImage().encodePng(png)) {
            call->fail("Could not encode the sheet as PNG.");
            return;
        }

        Json columnList = Json::array(), metrics = Json::array();
        for (const RenderColumn& column : columns) {
            columnList.push_back(column.key);
            metrics.push_back(column.metrics);
        }
        const int width = sheet.getImage().getWidth(), height = sheet.getImage().getHeight();
        Json out = {{"path", path}, {"model", modelJson}, {"mode", mode}, {"columns", columnList}, {"rows", viewNames},
                    {"cell", {layout.cellWidth, layout.cellHeight}}, {"size", {width, height}},
                    {"columns_per_row", layout.columns}, {"metrics", metrics}};
        if (wantJoints) {
            // per cell: its top-left corner and the slot joints, in sheet pixels
            Json cells = Json::array();
            for (int row = 0; row < layout.rows; row++) {
                for (int c = 0; c < layout.columns; c++) {
                    const int column = (row / views) * RENDER_ROW_COLUMNS + c;
                    if (column >= static_cast<int>(columns.size())) continue;
                    const int x0 = layout.cellX(c), y0 = layout.cellY(row);
                    Json joints = Json::object();
                    for (const auto& [bone, at] : job->joints[static_cast<size_t>(row * layout.columns + c)]) {
                        joints[humanBoneName(bone)] = {r1(x0 + at.x), r1(y0 + at.y)};
                    }
                    cells.push_back({{"column", column}, {"view", viewNames[row % views]}, {"x", x0}, {"y", y0}, {"joints", joints}});
                }
            }
            out["cells"] = cells;
        }

        // the caption: what is where, so the picture needs no guessing
        std::string what;
        if (mode == "poses") {
            what = "key poses";
            for (size_t i = 0; i < columns.size(); i++) what += (i == 0 ? " " : ", ") + columns[i].label;
        } else {
            what = std::to_string(columns.size()) + (mode == "motion" ? " evenly spaced frames" : " frames") + " (t=" +
                   fmt(columns.front().key["time"].get<double>()) + ".." + fmt(columns.back().key["time"].get<double>()) +
                   " s of the " + fmt(length) + " s " + (loop ? "loop" : "one-shot") + ", as baked and played)";
        }
        std::string rows;
        for (const std::string& name : viewNames) rows += (rows.empty() ? "" : ", ") + viewHint(name);
        std::string text = path + " on " + modelName + ", " + std::to_string(width) + "x" + std::to_string(height) + " sheet. Columns: " +
                           what + ". Rows, one view each with one scale per view" +
                           (blocks > 1 ? " (columns wrap after " + std::to_string(RENDER_ROW_COLUMNS) + ", the rows repeat)" : std::string("")) +
                           ": " + rows + ". " + (layout.labelHeight > 0 ? "Labels under the cells; coral = the character's left limbs, cyan = right, "
                                                             "white = centre; " : "") +
                           "orange line = floor.";
        if (!note.empty()) text += " Note: " + note + ".";
        for (const RenderColumn& column : columns) text += "\n" + column.line;

        if (!saveTarget.empty()) {
            std::error_code ec;
            if (saveTarget.has_parent_path()) fs::create_directories(saveTarget.parent_path(), ec);
            std::ofstream file(saveTarget, std::ios::binary | std::ios::trunc);
            if (!file || !file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()))) {
                call->fail("Could not write the PNG to " + projectRel(project, saveTarget) + ".");
                return;
            }
            out["saved_to"] = projectRel(project, saveTarget);
            text += "\nSaved to " + projectRel(project, saveTarget) + ".";
        }
        ToolResult reply = ToolResult::success(text, out);
        reply.images.push_back({Base64::encode(png.data(), png.size()), "image/png"});
        call->reply(std::move(reply));
    });
}

void registerRenderPoseAnimation(McpRegistry& registry) {
    using namespace schema;
    ToolDef tool;
    tool.name = "render_pose_animation";
    tool.title = "Render Pose Animation";
    tool.description =
        "Contact sheet PNG of a .poseanim on its preview model (or model: a scene model), drawn offscreen: one column "
        "per key pose (mode poses) or per frame (motion: evenly spaced; times: at given seconds; the baked frames the "
        "runtime plays), one row per view, each view one scale. Overlay: coral = the character's left limbs, cyan = "
        "right, labels, orange floor line. Returns the image, a caption and per-column metrics (as get_pose_animation). "
        "Images cost tokens: render few cells.";
    tool.inputSchema = object({
        {"path", assetPath("")},
        {"mode", withDefault(stringEnum("", {"poses", "motion", "times"}), "poses")},
        {"poses", array(integer(""), "Indices; default all (max 24).")},
        {"frames", withDefault(integer("For motion.", 4, 24), 12)},
        {"times", array(number(""), "Seconds.")},
        {"views", array(stringEnum("", {"front", "side", "three_quarter", "back", "top"}), "Default poses [front, side], else [side].")},
        {"model", entityRef()},
        {"scene", sceneRef()},
        {"cell", array(integer("", 64, 512), "[width, height], default [160, 200]; sheet max 2048 px.")},
        {"overlay", withDefault(boolean(""), true)},
        {"joints", boolean("Also return each cell's joint pixels.")},
        {"save_path", string("Also write the PNG (project-relative).")}
    }, {"path"});
    tool.annotations.readOnly = true;
    tool.annotations.idempotent = true;
    tool.allowedWhilePlaying = true;
    tool.handler = renderPoseAnimationTool;
    registry.addTool(std::move(tool));
}

} // namespace

void registerPoseTools(McpRegistry& registry) {
    using namespace schema;

    const Json transition = obj({{"curve", stringEnum("", {"spring", "linear", "smooth"})}, {"duration", number("")},
                                 {"overshoot", number("")}, {"anticipation", number("")}});
    const Json angles = anyObject("");

    {
        ToolDef tool;
        tool.name = "get_pose_animation";
        tool.title = "Get Pose Animation";
        tool.description =
            "A .poseanim pose animation: settings (loop, frame_rate, feel, humanize, preset), length, the rig (mapped "
            "bones, missing slots) and its key poses (name, hold, grounded, lift, transition or null = the animation's, "
            "start_time, offset {left, forward}, angles: degrees, 0 = neutral standing; see jasper://docs/pose-animation "
            "for the DOF table) with metrics (feet height/contact, balanced, balance_margin, limit_hits). model: a scene "
            "model's rig instead of preview_model. templates: true (no path) lists the pose templates.";
        tool.inputSchema = object({
            {"path", assetPath("")},
            {"templates", boolean("")},
            {"poses", array(integer(""), "Indices; default all.")},
            {"model", entityRef()},
            {"scene", sceneRef()},
            {"detail", withDefault(stringEnum("summary omits angles.", {"summary", "full"}), "full")}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.allowedWhilePlaying = true;
        tool.handler = getPoseAnimationTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "edit_pose_animation";
        tool.title = "Edit Pose Animation";
        tool.description =
            "Edit a .poseanim: ops in order (index 0-based, after earlier ops), all or nothing, one project undo step. "
            "add_pose copies from (default after; neutral if empty), blended toward blend_to; update_pose (metadata never "
            "changes angles); delete_pose; move_pose; mirror_pose; set_settings (partial); apply_preset; auto_timing; "
            "set_preview_model; set_humanoid (writes the model's .humanoid: slot -> bone, \"\" unmapped, null auto); "
            "apply_template (mode replace: settings+poses, append: poses); save_template (project template). "
            "angles: absolute degrees per DOF, add_angles: added. Clamped values are echoed in clamped. create makes the "
            "file (template: from one); bake writes an Animation on a scene model (scene undo; without path: its Pose Animator's). Returns changes "
            "and the touched poses with metrics.";
        tool.inputSchema = object({
            {"path", assetPath("")},
            {"create", obj({{"preview_model", assetPath("")}, {"preset", string("")}, {"template", string("")}})},
            {"ops", array(obj({
                {"op", stringEnum("", opNames())},
                {"index", integer("")},
                {"after", integer("-1 = first; default last.")},
                {"to", integer("")},
                {"name", string("")},
                {"from", integer("")},
                {"blend_to", integer("")},
                {"blend", number("", 0, 1)},
                {"hold", number("")},
                {"grounded", boolean("")},
                {"lift", number("")},
                {"offset", obj({{"left", number("")}, {"forward", number("")}})},
                {"angles", angles},
                {"add_angles", angles},
                {"transition", Json{{"anyOf", Json::array({transition, {{"type", "null"}}})}}},
                {"as_new", boolean("")},
                {"loop", boolean("")},
                {"frame_rate", integer("")},
                {"step_rate", number("")},
                {"overlap", number("")},
                {"looseness", number("")},
                {"weights", obj({{"spine", number("")}, {"head", number("")}, {"arms", number("")}, {"legs", number("")}})},
                {"humanize", anyObject("")},
                {"model", assetPath("")},
                {"bones", anyObject("")},
                {"clear_custom", boolean("")},
                {"mode", stringEnum("", {"replace", "append"})},
                {"description", string("")},
                {"category", string("")}
            }, {"op"}), "")},
            {"bake", obj({{"model", entityRef()}, {"name", string("Default: the file name.")}}, {"model"})},
            {"scene", sceneRef()}
        });
        tool.annotations.readOnly = false;
        tool.annotations.idempotent = false;
        tool.allowedWhilePlaying = false;
        tool.handler = editPoseAnimationTool;
        registry.addTool(std::move(tool));
    }

    registerRenderPoseAnimation(registry);
}

} // namespace jasperengine::editor::mcp
