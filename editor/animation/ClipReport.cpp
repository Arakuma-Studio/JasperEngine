// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ClipReport.h"

#include "animation/AnimatorAssetIO.h"
#include "animation/AnimatorPose.h"
#include "animation/AnimatorRetarget.h"
#include "animation/KeyframeSampling.h"
#include "animation/ModelSubClips.h"
#include "io/FileData.h"
#include "pool/AnimationClipPool.h"
#include "pose/HumanoidMapIO.h"
#include "pose/ModelRig.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

using namespace jasperengine;

namespace{

    bool same(float a, float b){ return a == b; }
    bool same(const Vector3& a, const Vector3& b){ return a.x == b.x && a.y == b.y && a.z == b.z; }
    bool same(const Quaternion& a, const Quaternion& b){ return a.w == b.w && a.x == b.x && a.y == b.y && a.z == b.z; }

    bool isZero(float value){ return value == 0.0f; }
    bool isZero(const Vector3& value){ return value.x == 0.0f && value.y == 0.0f && value.z == 0.0f; }
    bool isZero(const Quaternion& value){ return value.w == 0.0f && value.x == 0.0f && value.y == 0.0f && value.z == 0.0f; }

    // every row of `width` values equals the first; tangents, when there are any, are all zero
    template<class T>
    bool holdsStill(const T* values, const T* in, const T* out, size_t keys, size_t width){
        for (size_t i = 0; i < keys * width; i++){
            if (!same(values[i], values[i % width])) return false;
            if (in && !(isZero(in[i]) && isZero(out[i]))) return false;
        }
        return true;
    }

    // Exact: near-constant is the reducer's decision, with a measured error, not a fact to report
    bool isConstant(const AnimationClip::Track& track){
        if (track.keyCount < 2) return false;
        if (track.rotations) return holdsStill(track.rotations, track.inRotations, track.outRotations, track.keyCount, track.width);
        if (track.vectors) return holdsStill(track.vectors, track.inVectors, track.outVectors, track.keyCount, track.width);
        if (track.weights) return holdsStill(track.weights, track.inWeights, track.outWeights, track.keyCount, track.width);
        return false;
    }

    std::optional<uint64_t> fileSizeOf(const std::string& reference){
        if (reference.empty() || anim::isSubClipReference(reference)) return std::nullopt;
        std::error_code error;
        const uintmax_t size = std::filesystem::file_size(editor::FileUtils::pathFromUtf8(FileData::getSystemPath(reference)), error);
        if (error) return std::nullopt;
        return (uint64_t)size;
    }

    std::string withThousands(uint64_t value){
        std::string digits = std::to_string(value);
        for (size_t at = digits.size(); at > 3; at -= 3) digits.insert(at - 3, ",");
        return digits;
    }

    // ---- posing on a rig ----

    struct Point{
        double x = 0.0, y = 0.0, z = 0.0;
    };

    // One clip posed on a rig the way it plays: sampleTrack per track, the relative rule of
    // AnimatorRig, then poseToRigSpace. A bone with no track holds its rest.
    class RigPoser{
    public:
        RigPoser(const AnimationClip& clip, const anim::ClipRig& rig): parents(rig.parents.data()){
            const size_t bones = rig.size();
            rest.resize(bones, 0);
            local.resize(bones, 0);
            for (size_t b = 0; b < bones; b++){
                rest.setPosition(b, rig.restPositions[b]);
                rest.setRotation(b, rig.restRotations[b]);
                rest.setScale(b, rig.restScales[b]);
            }
            world.resize(bones);
            positions.resize(bones);
            rotations.resize(bones);
            scales.resize(bones);
            for (const AnimationClip::Track& track : clip.getTracks()){
                if (track.keyCount == 0) continue;
                if (track.channel != ClipChannel::Position && track.channel != ClipChannel::Rotation && track.channel != ClipChannel::Scale) continue;
                const int bone = rig.findBone(track.path);
                if (bone >= 0) bindings.push_back({&track, bone, anim::KeyCursor{}});
            }
        }

        void pose(float time){
            local.copyFrom(rest);
            for (Binding& binding : bindings){
                if (!anim::sampleTrack(*binding.track, time, sample, &binding.cursor)) continue;
                const size_t bone = (size_t)binding.bone;
                switch (binding.track->channel){
                    case ClipChannel::Position:
                        local.setPosition(bone, binding.track->relative ? rest.position(bone) + sample.vector : sample.vector);
                        break;
                    case ClipChannel::Scale:
                        local.setScale(bone, binding.track->relative ? rest.scale(bone) * sample.vector : sample.vector);
                        break;
                    default:
                        local.setRotation(bone, binding.track->relative ? rest.rotation(bone) * sample.rotation : sample.rotation);
                        break;
                }
            }
            anim::poseToRigSpace(local, parents, anim::RigSpace{world.data(), positions.data(), rotations.data(), scales.data()});
        }

        const anim::Affine& matrix(size_t bone) const { return world[bone]; }
        const Quaternion& rotation(size_t bone) const { return rotations[bone]; }
        // the bones a track of this clip moves, itself and every descendant
        void markMoved(std::vector<uint8_t>& moved) const{
            for (const Binding& binding : bindings) moved[(size_t)binding.bone] = 1;
        }

    private:
        struct Binding{
            const AnimationClip::Track* track;
            int bone;
            anim::KeyCursor cursor;
        };
        const int* parents;
        anim::Pose rest, local;
        std::vector<anim::Affine> world;
        std::vector<Vector3> positions;
        std::vector<Quaternion> rotations;
        std::vector<Vector3> scales;
        std::vector<Binding> bindings;
        anim::TrackSample sample;
    };

    void spreadToDescendants(const anim::ClipRig& rig, std::vector<uint8_t>& moved){
        for (size_t b = 0; b < rig.size(); b++){
            if (rig.parents[b] >= 0 && moved[(size_t)rig.parents[b]]) moved[b] = 1;
        }
    }

    // A joint's origin and the six points `distance` metres from it along its axes. `reference`
    // gives each axis its length, so a scale error in `pose` shows and the distance is in metres
    // whatever scale the rig is authored at.
    void jointPoints(const anim::Affine& pose, const anim::Affine& reference, double distance, Point out[7]){
        out[0] = Point{pose.m[9], pose.m[10], pose.m[11]};
        for (int axis = 0; axis < 3; axis++){
            const double rx = reference.m[axis * 3], ry = reference.m[axis * 3 + 1], rz = reference.m[axis * 3 + 2];
            const double length = std::sqrt(rx * rx + ry * ry + rz * rz);
            const double factor = length > 1e-12 ? distance / length : 0.0;
            const Point offset{pose.m[axis * 3] * factor, pose.m[axis * 3 + 1] * factor, pose.m[axis * 3 + 2] * factor};
            out[1 + axis * 2] = Point{out[0].x + offset.x, out[0].y + offset.y, out[0].z + offset.z};
            out[2 + axis * 2] = Point{out[0].x - offset.x, out[0].y - offset.y, out[0].z - offset.z};
        }
    }

    double distanceBetween(const Point& a, const Point& b){
        const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // the runtime's order (AnimatorInstance::bindClip): the clip's bone, the hips, then the
    // shallowest bone the clip has a position track on; -1 when it has none
    int rootMotionBone(const AnimationClip& clip, const anim::ClipRig& rig){
        const anim::ClipRootMotion declared = anim::readClipRootMotion(clip.getUnknownKeys());
        if (!declared.bone.empty()){
            const int bone = rig.findBone(declared.bone);
            if (bone >= 0) return bone;
        }
        if (rig.hips >= 0) return rig.hips;
        int best = -1, bestDepth = 1 << 30;
        for (const AnimationClip::Track& track : clip.getTracks()){
            if (track.channel != ClipChannel::Position) continue;
            const int bone = rig.findBone(track.path);
            if (bone < 0) continue;
            int depth = 0;
            for (int p = rig.parents[(size_t)bone]; p >= 0; p = rig.parents[(size_t)p]) depth++;
            if (depth < bestDepth){
                bestDepth = depth;
                best = bone;
            }
        }
        return best;
    }

    double wrapRadians(double angle){
        return std::remainder(angle, 2.0 * 3.14159265358979323846);
    }

    // the heading a bone's rotation gives, when its forward axis is not close to vertical
    bool headingOf(const Quaternion& rotation, double& heading){
        const Vector3 forward = rotation * Vector3(0, 0, 1);
        if (std::hypot((double)forward.x, (double)forward.z) < 0.2) return false;
        heading = std::atan2((double)forward.x, (double)forward.z);
        return true;
    }

    anim::ClipLoopSeam measureLoopSeam(const AnimationClip& clip, const anim::ClipRig& rig, double skinDistanceMm){
        anim::ClipLoopSeam seam;
        if (!(clip.getLength() > 0.0f)) return seam;
        const double distance = std::max(skinDistanceMm, 0.0) / 1000.0;
        RigPoser poser(clip, rig);
        std::vector<uint8_t> moved(rig.size(), 0);
        poser.markMoved(moved);
        spreadToDescendants(rig, moved);
        poser.pose(0.0f);
        std::vector<anim::Affine> first(rig.size());
        for (size_t b = 0; b < rig.size(); b++) first[b] = poser.matrix(b);
        const int bone = rootMotionBone(clip, rig);
        Point rootStart, rootEnd;
        double headingStart = 0.0;
        bool hasHeading = false;
        if (bone >= 0){
            const anim::Affine& root = poser.matrix((size_t)bone);
            rootStart = Point{root.m[9], root.m[10], root.m[11]};
            hasHeading = headingOf(poser.rotation((size_t)bone), headingStart);
        }
        poser.pose(clip.getLength());
        double turn = 0.0;
        if (bone >= 0){
            const anim::Affine& root = poser.matrix((size_t)bone);
            rootEnd = Point{root.m[9], root.m[10], root.m[11]};
            double headingEnd = 0.0;
            if (hasHeading && headingOf(poser.rotation((size_t)bone), headingEnd)) turn = wrapRadians(headingStart - headingEnd);
        }
        const double cosTurn = std::cos(turn), sinTurn = std::sin(turn);
        for (size_t b = 0; b < rig.size(); b++){
            if (!moved[b]) continue;
            Point at[7], back[7];
            jointPoints(first[b], first[b], distance, at);
            jointPoints(poser.matrix(b), first[b], distance, back);
            double worst = 0.0;
            for (int i = 0; i < 7; i++){
                // the end pose carried back along the root's travel: its height stays, so a jump counts
                const double x = back[i].x - rootEnd.x, z = back[i].z - rootEnd.z;
                const Point aligned{x * cosTurn + z * sinTurn + rootStart.x, back[i].y, -x * sinTurn + z * cosTurn + rootStart.z};
                worst = std::max(worst, distanceBetween(at[i], aligned));
            }
            if (worst * 1000.0 > seam.maxErrorMm){
                seam.maxErrorMm = worst * 1000.0;
                seam.worstJoint = rig.names[b];
            }
        }
        return seam;
    }

    double rounded(double value, double decimals = 1000.0){
        return std::round(value * decimals) / decimals;
    }

    std::string lowered(std::string text){
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return text;
    }

    // R23's names: mixamorig:LeftFoot, LeftToeBase, foot_l, ball_l (Unreal's toe), ankle_R
    bool namesAFoot(const std::string& name){
        const std::string text = lowered(name);
        return text.find("foot") != std::string::npos || text.find("toe") != std::string::npos ||
               text.find("ankle") != std::string::npos || text.rfind("ball_", 0) == 0;
    }

}

int anim::ClipRig::findBone(const std::string& path) const{
    const auto found = byPath.find(path);
    return found == byPath.end() ? -1 : found->second;
}

void anim::ClipRig::assign(const std::string& sourceName, const std::vector<ModelNodeInfo>& nodes){
    source = sourceName;
    names.clear();
    paths.clear();
    parents.clear();
    restPositions.clear();
    restRotations.clear();
    restScales.clear();
    feet.clear();
    byPath.clear();
    hips = -1;
    const size_t count = nodes.size();
    // parents first, whatever order the file lists them in: by depth, the file's order within one
    std::vector<int> depth(count, 0);
    for (size_t i = 0; i < count; i++){
        size_t steps = 0;
        for (int p = nodes[i].parent; p >= 0 && (size_t)p < count && steps <= count; p = nodes[(size_t)p].parent, steps++) depth[i]++;
        if (steps > count) depth[i] = 0;
    }
    std::vector<size_t> order(count);
    for (size_t i = 0; i < count; i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b){ return depth[a] < depth[b]; });
    std::vector<int> position(count, -1);
    for (size_t i = 0; i < count; i++){
        const ModelNodeInfo& node = nodes[order[i]];
        position[order[i]] = (int)i;
        names.push_back(node.name);
        paths.push_back(node.path);
        const bool hasParent = node.parent >= 0 && (size_t)node.parent < count && position[(size_t)node.parent] >= 0;
        parents.push_back(hasParent ? position[(size_t)node.parent] : -1);
        restPositions.push_back(node.position);
        restRotations.push_back(node.rotation);
        restScales.push_back(node.scale);
        feet.push_back(namesAFoot(node.name) ? 1 : 0);
        // a repeat of a path cannot bind: the first node wins, as tracks resolve
        if (node.addressable) byPath.emplace(node.path, (int)i);
    }
}

bool anim::clipRigFromModel(const std::string& modelPath, ClipRig& out, std::string* error){
    std::vector<ModelNodeInfo> nodes;
    if (!readModelFileNodes(modelPath, nodes, error)) return false;
    out.assign(modelPath, nodes);
    std::string stamp;
    pose::HumanoidOverrides overrides;
    std::vector<std::string> warnings;
    if (!pose::humanoidSidecarStamp(modelPath, stamp, nullptr) || stamp.empty()) return true;
    if (!pose::parseHumanoidOverrides(stamp, overrides, warnings, nullptr) || !overrides.isHumanoid()) return true;
    std::string sourceError;
    const std::shared_ptr<const RetargetSource> source = retargetSource(modelPath, &sourceError);
    if (!source) return true;
    auto boneOfSlot = [&](pose::HumanBone slot) -> int{
        const std::string& name = source->humanoid.bones[(size_t)slot];
        if (name.empty()) return -1;
        const auto found = std::find(out.names.begin(), out.names.end(), name);
        return found == out.names.end() ? -1 : (int)(found - out.names.begin());
    };
    out.hips = boneOfSlot(pose::HumanBone::Hips);
    for (pose::HumanBone slot : {pose::HumanBone::LeftFoot, pose::HumanBone::RightFoot, pose::HumanBone::LeftToes, pose::HumanBone::RightToes}){
        const int bone = boneOfSlot(slot);
        if (bone >= 0) out.feet[(size_t)bone] = 1;
    }
    return true;
}

anim::ClipReport anim::analyzeClip(const ClipReportRequest& request){
    ClipReport report;
    std::shared_ptr<const AnimationClip> clip = request.clip;
    bool loaded = false;
    if (!clip){
        if (request.reference.empty()){
            report.error = "no clip";
            return report;
        }
        clip = AnimationClipPool::get(request.reference);
        if (!clip){
            clip = AnimationClipPool::getOrLoad(request.reference, &report.error);
            loaded = clip != nullptr;
        }
        if (!clip) return report;
    }

    report.ok = true;
    report.runtimeBytes = clip->memoryBytes();
    report.fileBytes = fileSizeOf(request.reference);
    report.tracks = clip->getTracks().size();
    report.lengthSeconds = clip->getLength();
    for (const AnimationClip::Track& track : clip->getTracks()){
        report.keys += track.keyCount;
        if (isConstant(track)) report.constantTracks++;
    }
    if (request.rig){
        if (request.loopSeam) report.loopSeam = measureLoopSeam(*clip, *request.rig, request.skinDistanceMm);
    }

    if (loaded){
        clip.reset();
        AnimationClipPool::releaseUnused(request.reference);
    }
    return report;
}

const anim::ClipReportField* anim::findClipReportField(std::string_view key){
    for (const ClipReportField& field : kClipReportFields){
        if (key == field.key) return &field;
    }
    return nullptr;
}

std::string anim::formatClipField(const ClipReportField& field, double value){
    char text[32];
    switch (field.format){
        case ClipFieldFormat::Bytes:
            if (value < 1024.0) return withThousands((uint64_t)value) + " B";
            if (value < 1024.0 * 1024.0){
                std::snprintf(text, sizeof(text), "%.0f KB", value / 1024.0);
            }else{
                std::snprintf(text, sizeof(text), "%.1f MB", value / (1024.0 * 1024.0));
            }
            return text;
        case ClipFieldFormat::Count:
            return withThousands((uint64_t)value);
        case ClipFieldFormat::Seconds:
            std::snprintf(text, sizeof(text), "%.2f s", value);
            return text;
        case ClipFieldFormat::Millimetres:
            std::snprintf(text, sizeof(text), "%.2f mm", value);
            return text;
        case ClipFieldFormat::Text:
            break;
    }
    return std::string();
}

std::string anim::formatClipField(std::string_view key, double value){
    const ClipReportField* field = findClipReportField(key);
    return field ? formatClipField(*field, value) : std::string();
}

nlohmann::json anim::clipReportJson(const ClipReport& report){
    nlohmann::json out = nlohmann::json::object();
    out["runtime_bytes"] = report.runtimeBytes;
    out["file_bytes"] = report.fileBytes ? nlohmann::json(*report.fileBytes) : nlohmann::json(nullptr);
    out["keys"] = report.keys;
    out["tracks"] = report.tracks;
    // the float's own digits, not its expansion as a double (1.33, not 1.3300000429)
    out["length_s"] = std::round((double)report.lengthSeconds * 1000000.0) / 1000000.0;
    out["constant_tracks"] = report.constantTracks;
    if (report.loopSeam){
        out["loop_seam"] = {{"max_error_mm", rounded(report.loopSeam->maxErrorMm)}, {"worst_joint", report.loopSeam->worstJoint}};
    }
    return out;
}
