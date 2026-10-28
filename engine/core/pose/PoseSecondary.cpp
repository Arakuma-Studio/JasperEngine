// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pose/PoseSecondary.h"

#include "pose/PoseMath.h"

#include <array>
#include <cmath>

namespace jasperengine::pose {

    namespace {

        struct ChainBone {
            HumanBone bone;
            double share;
        };

        // shares of the present bones: a missing bone's share goes to the next present one, a
        // share left at the end to the last present one
        std::vector<ChainBone> presentShares(const PoseRig& rig, std::initializer_list<ChainBone> chain){
            std::vector<ChainBone> result;
            double carry = 0.0;
            for (const ChainBone& c : chain){
                if (rig.has(c.bone)){
                    result.push_back({c.bone, c.share + carry});
                    carry = 0.0;
                }else{
                    carry += c.share;
                }
            }
            if (!result.empty()){
                result.back().share += carry;
            }
            return result;
        }

        struct Spring {
            double omega;
            double zeta;
            Vec3d x;
            Vec3d v;

            void step(const Vec3d& acc, double h){
                // semi-implicit Euler: velocity first, then position with the new velocity
                v += (x * (-omega * omega) - v * (2.0 * zeta * omega) - acc) * h;
                x += v * h;
            }
        };

        void tilt(const Vec3d& x, double amount, float& lean, float& side){
            const double k = 300.0 * amount;
            lean = static_cast<float>(clampd(k * (x.z - x.y), -35.0, 35.0));
            side = static_cast<float>(clampd(k * x.x, -35.0, 35.0));
        }

    }

    std::vector<FollowTilt> followThroughTilts(const std::vector<Vector3>& hipsCanonical, int fps, bool loop, const HumanizeSettings& humanize){
        const size_t count = hipsCanonical.size();
        std::vector<FollowTilt> tilts(count);
        if (count < 2 || fps <= 0){
            return tilts;
        }
        // a looping sequence repeats its first frame at the end: the period excludes it
        const size_t period = loop ? count - 1 : count;
        const double fps2 = static_cast<double>(fps) * fps;
        std::vector<Vec3d> acc(count);
        for (size_t f = 0; f < period; f++){
            if (loop){
                const Vec3d prev(hipsCanonical[(f + period - 1) % period]);
                const Vec3d next(hipsCanonical[(f + 1) % period]);
                acc[f] = (next - Vec3d(hipsCanonical[f]) * 2.0 + prev) * fps2;
            }else if (f > 0 && f + 1 < count){
                acc[f] = (Vec3d(hipsCanonical[f + 1]) - Vec3d(hipsCanonical[f]) * 2.0 + Vec3d(hipsCanonical[f - 1])) * fps2;
            }
        }

        const double zeta = 0.9 + (0.2 - 0.9) * humanize.springiness;
        Spring torso{2.0 * POSE_PI * 3.5, zeta, Vec3d(), Vec3d()};
        Spring head{2.0 * POSE_PI * 2.5, zeta, Vec3d(), Vec3d()};
        const double h = 1.0 / (fps * 4.0);
        const int passes = loop ? 2 : 1;
        for (int pass = 0; pass < passes; pass++){
            for (size_t f = 0; f < period; f++){
                for (int sub = 0; sub < 4; sub++){
                    torso.step(acc[f], h);
                    head.step(acc[f], h);
                }
                if (pass == passes - 1){
                    tilt(torso.x, humanize.torsoFollow, tilts[f].torsoLean, tilts[f].torsoSide);
                    tilt(head.x, humanize.headFollow, tilts[f].headLean, tilts[f].headSide);
                }
            }
        }
        if (loop){
            tilts[count - 1] = tilts[0];
        }
        return tilts;
    }

    void applyFollowThrough(std::vector<HumanPose>& frames, const PoseRig& rig, int fps, bool loop, const HumanizeSettings& humanize){
        if (!rig.valid() || frames.size() < 2){
            return;
        }
        const Quatd characterInv = Quatd(rig.characterRotation()).normalized().inverse();
        const double scale = rig.humanScale();
        std::vector<Vector3> hips(frames.size());
        for (size_t f = 0; f < frames.size(); f++){
            hips[f] = (characterInv * Vec3d(rig.hipsWorldPosition(frames[f])) / scale).toVector3();
        }
        const std::vector<FollowTilt> tilts = followThroughTilts(hips, fps, loop, humanize);

        using B = HumanBone;
        const std::vector<ChainBone> torsoChain = presentShares(rig, {{B::Spine, 0.3}, {B::Chest, 0.35}, {B::UpperChest, 0.35}});
        const std::vector<ChainBone> headChain = presentShares(rig, {{B::Neck, 0.4}, {B::Head, 0.6}});
        const Vec3d left(1, 0, 0);
        const Vec3d forward(0, 0, 1);

        for (size_t f = 0; f < frames.size(); f++){
            HumanPose& frame = frames[f];
            const FollowTilt& t = tilts[f];
            for (int chain = 0; chain < 2; chain++){
                const double lean = chain == 0 ? t.torsoLean : t.headLean;
                const double side = chain == 0 ? t.torsoSide : t.headSide;
                if (lean == 0.0 && side == 0.0){
                    continue;
                }
                for (const ChainBone& c : chain == 0 ? torsoChain : headChain){
                    // world-space tilt, parent before child: children inherit their parent's tilt
                    std::array<Quatd, HUMAN_BONE_COUNT> world;
                    rig.slotWorldCanonical(frame, world);
                    const Quatd delta = axisAngleRad(left, degToRad(lean * c.share)) * axisAngleRad(forward, -degToRad(side * c.share));
                    const HumanBone parent = rig.effectiveParent(c.bone);
                    const Quatd parentWorld = parent == HumanBone::Count ? Quatd() : world[boneIndex(parent)];
                    frame.rotations[boneIndex(c.bone)] = rig.localFromSlotWorld(c.bone, parentWorld, delta * world[boneIndex(c.bone)]).toQuaternion();
                }
            }
        }
        if (loop){
            frames.back() = frames.front();
        }
    }

}
