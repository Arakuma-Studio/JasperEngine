// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pose/PoseMetrics.h"

#include "pose/PoseDofs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace jasperengine::pose {

    namespace {

        using B = HumanBone;

        constexpr double HEAD_LENGTH = 0.2;     // head top estimate, humanScale units along the head
        constexpr double HAND_LENGTH = 0.1;
        constexpr double FOOT_LENGTH = 0.15;    // foot tip estimate when the rig has no toes
        constexpr double FOOT_HALF_WIDTH = 0.045;

        struct Point2 {
            double x = 0, z = 0;
        };

        double cross2(const Point2& o, const Point2& a, const Point2& b){
            return (a.x - o.x) * (b.z - o.z) - (a.z - o.z) * (b.x - o.x);
        }

        // Andrew's monotone chain, counter-clockwise, collinear points dropped
        void convexHull(std::vector<Point2> points, std::vector<Point2>& hull){
            std::sort(points.begin(), points.end(), [](const Point2& a, const Point2& b){
                return a.x < b.x || (a.x == b.x && a.z < b.z);
            });
            if (points.size() < 3){
                hull = points;
                return;
            }
            std::vector<Point2> lower, upper;
            for (const Point2& p : points){
                while (lower.size() >= 2 && cross2(lower[lower.size() - 2], lower.back(), p) <= 0) lower.pop_back();
                lower.push_back(p);
            }
            for (auto it = points.rbegin(); it != points.rend(); ++it){
                while (upper.size() >= 2 && cross2(upper[upper.size() - 2], upper.back(), *it) <= 0) upper.pop_back();
                upper.push_back(*it);
            }
            // each chain's last point starts the other one
            hull.assign(lower.begin(), lower.end() - 1);
            hull.insert(hull.end(), upper.begin(), upper.end() - 1);
        }

        double segmentDistance(const Point2& p, const Point2& a, const Point2& b){
            const double dx = b.x - a.x, dz = b.z - a.z;
            const double len2 = dx * dx + dz * dz;
            double t = len2 > 0.0 ? ((p.x - a.x) * dx + (p.z - a.z) * dz) / len2 : 0.0;
            t = std::min(1.0, std::max(0.0, t));
            const double ex = a.x + dx * t - p.x, ez = a.z + dz * t - p.z;
            return std::sqrt(ex * ex + ez * ez);
        }

        // Signed distance of p to the hull's boundary: positive inside, negative outside
        double signedHullDistance(const Point2& p, const std::vector<Point2>& hull, bool& inside){
            inside = false;
            if (hull.empty()){
                return -std::numeric_limits<double>::infinity();
            }
            if (hull.size() == 1){
                return -std::hypot(p.x - hull[0].x, p.z - hull[0].z);
            }
            double nearest = std::numeric_limits<double>::infinity();
            bool allLeft = hull.size() >= 3;
            for (size_t i = 0; i < hull.size(); i++){
                const Point2& a = hull[i];
                const Point2& b = hull[(i + 1) % hull.size()];
                nearest = std::min(nearest, segmentDistance(p, a, b));
                if (cross2(a, b, p) < 0) allLeft = false;
            }
            inside = allLeft;
            return inside ? nearest : -nearest;
        }

    }

    PoseMetrics computeMetrics(const PoseRig& rig, const HumanPose& pose, const BodyPose& body){
        PoseMetrics metrics;
        if (!rig.valid()){
            return metrics;
        }
        const double hs = std::max(1e-9, static_cast<double>(rig.humanScale()));
        const double floor = rig.floorHeight();
        const Vec3d left(rig.characterLeft()), up(rig.characterUp()), forward(rig.characterForward());

        std::vector<Quatd> worldRot;
        std::vector<Affine3d> world;
        rig.computeWorldPrecise(pose, worldRot, world);
        std::array<Quatd, HUMAN_BONE_COUNT> slotWorld;
        rig.slotWorldCanonical(pose, slotWorld);

        // model point -> canonical, humanScale units above the floor
        auto canonical = [&](const Vec3d& p){
            return Vec3d(p.dot(left), p.dot(up) - floor, p.dot(forward)) / hs;
        };
        auto position = [&](B bone){
            return canonical(world[rig.boneIndex(bone)].t);
        };
        // the bone's direction now (canonical): its neutral direction carried by the pose
        auto direction = [&](B bone){
            const Vec3d d(boneDofInfo(bone).neutralDirection);
            return (slotWorld[boneIndex(bone)] * rig.neutralWorldCanonical(bone).inverse() * d).normalized();
        };
        auto along = [&](B bone, double length){
            return position(bone) + direction(bone) * length;
        };

        // ---- feet
        const PoseSkin* skin = rig.skin();
        auto footHeight = [&](B foot, B toes){
            double lowest = std::numeric_limits<double>::infinity();
            if (skin && !skin->vertices.empty()){
                PoseSkin part;
                part.inverseBind = skin->inverseBind;
                const int footBone = rig.boneIndex(foot);
                const int toesBone = rig.has(toes) ? rig.boneIndex(toes) : -2;
                for (const PoseSkinVertex& v : skin->vertices){
                    float w = 0.0f;
                    for (int i = 0; i < 4; i++){
                        if (v.bones[i] >= 0 && (v.bones[i] == footBone || v.bones[i] == toesBone)) w += v.weights[i];
                    }
                    if (w > 0.5f) part.vertices.push_back(v);
                }
                if (!part.vertices.empty()){
                    lowest = part.lowest(world);
                }
            }
            if (!std::isfinite(lowest)){
                for (B bone : {foot, toes}){
                    if (!rig.has(bone)) continue;
                    lowest = std::min(lowest, world[rig.boneIndex(bone)].t.dot(up) - rig.soleOffset(bone));
                }
            }
            return (lowest - floor) / hs;
        };
        auto footMetrics = [&](B foot, B toes){
            FootMetrics out;
            out.height = static_cast<float>(footHeight(foot, toes));
            out.contact = out.height <= FOOT_CONTACT_HEIGHT;
            const Vec3d p = position(foot);
            out.position = Vector3(static_cast<float>(p.x), out.height, static_cast<float>(p.z));
            return out;
        };
        metrics.left = footMetrics(B::LeftFoot, B::LeftToes);
        metrics.right = footMetrics(B::RightFoot, B::RightToes);
        metrics.lowest = static_cast<float>((rig.lowestPointPrecise(pose, skin) - floor) / hs);

        // ---- centre of mass
        Vec3d mass;
        double total = 0.0;
        auto add = [&](const Vec3d& a, const Vec3d& b, double weight){
            mass += (a + b) * (0.5 * weight);
            total += weight;
        };
        const B neckBase = rig.has(B::Neck) ? B::Neck : B::Head;
        add(position(neckBase), along(B::Head, HEAD_LENGTH), 8.0);
        std::vector<B> trunk = {B::Hips, B::Spine};
        if (rig.has(B::Chest)) trunk.push_back(B::Chest);
        if (rig.has(B::UpperChest)) trunk.push_back(B::UpperChest);
        trunk.push_back(neckBase);
        for (size_t i = 0; i + 1 < trunk.size(); i++){
            add(position(trunk[i]), position(trunk[i + 1]), 50.0 / static_cast<double>(trunk.size() - 1));
        }
        for (bool isLeft : {true, false}){
            const B upperArm = isLeft ? B::LeftUpperArm : B::RightUpperArm;
            const B lowerArm = isLeft ? B::LeftLowerArm : B::RightLowerArm;
            const B hand = isLeft ? B::LeftHand : B::RightHand;
            const B upperLeg = isLeft ? B::LeftUpperLeg : B::RightUpperLeg;
            const B lowerLeg = isLeft ? B::LeftLowerLeg : B::RightLowerLeg;
            const B foot = isLeft ? B::LeftFoot : B::RightFoot;
            const B toes = isLeft ? B::LeftToes : B::RightToes;
            add(position(upperArm), position(lowerArm), 2.7);
            add(position(lowerArm), position(hand), 1.6);
            add(position(hand), along(hand, HAND_LENGTH), 0.6);
            add(position(upperLeg), position(lowerLeg), 10.0);
            add(position(lowerLeg), position(foot), 4.6);
            add(position(foot), rig.has(toes) ? position(toes) : along(foot, FOOT_LENGTH), 1.4);
        }
        mass = mass / total;
        metrics.centerOfMass = mass.toVector3();

        // ---- support and balance
        const Point2 com{mass.x, mass.z};
        std::vector<Point2> support, allFeet;
        for (bool isLeft : {true, false}){
            const B foot = isLeft ? B::LeftFoot : B::RightFoot;
            const B toes = isLeft ? B::LeftToes : B::RightToes;
            const Vec3d heel = position(foot);
            const Vec3d toe = rig.has(toes) ? position(toes) : along(foot, FOOT_LENGTH);
            allFeet.push_back({heel.x, heel.z});
            allFeet.push_back({toe.x, toe.z});
            if (!(isLeft ? metrics.left.contact : metrics.right.contact)){
                continue;
            }
            Vec3d length(toe.x - heel.x, 0.0, toe.z - heel.z);
            Vec3d side = length.squaredLength() > 1e-12 ? Vec3d(-length.z, 0.0, length.x).normalized() : Vec3d(1.0, 0.0, 0.0);
            side *= FOOT_HALF_WIDTH;
            for (const Vec3d& p : {heel, toe}){
                support.push_back({p.x + side.x, p.z + side.z});
                support.push_back({p.x - side.x, p.z - side.z});
            }
        }
        if (support.empty()){
            double nearest = std::numeric_limits<double>::infinity();
            for (const Point2& p : allFeet) nearest = std::min(nearest, std::hypot(p.x - com.x, p.z - com.z));
            metrics.balanced = false;
            metrics.balanceMargin = static_cast<float>(-nearest);
        }else{
            bool inside = false;
            std::vector<Point2> hull;
            convexHull(support, hull);
            const double margin = signedHullDistance(com, hull, inside);
            metrics.balanced = inside;
            metrics.balanceMargin = static_cast<float>(margin);
        }

        // ---- joint limits: the limits clampToLimits applies, with LIMIT_HIT_TOLERANCE (a frame
        // clamped to a limit reads back a hair outside it; key poses are clamped exactly anyway)
        for (int b = 0; b < HUMAN_BONE_COUNT; b++){
            const HumanBone bone = boneAt(b);
            if (!rig.has(bone)){
                continue;
            }
            const float* angles = &body.angles[b * DOFS_PER_BONE];
            bool hit[DOFS_PER_BONE] = {};
            for (int k = 0; k < DOFS_PER_BONE; k++){
                const DofInfo& info = dofInfo(dofIndex(bone, k));
                hit[k] = !(angles[k] >= info.minDegrees - LIMIT_HIT_TOLERANCE && angles[k] <= info.maxDegrees + LIMIT_HIT_TOLERANCE);
            }
            const double swing = std::sqrt(static_cast<double>(angles[0]) * angles[0] + static_cast<double>(angles[1]) * angles[1]);
            if (swing > static_cast<double>(MAX_SWING_DEGREES) + LIMIT_HIT_TOLERANCE){
                hit[0] = hit[1] = true;
            }
            for (int k = 0; k < DOFS_PER_BONE; k++){
                if (hit[k]) metrics.limitHits.push_back(b * DOFS_PER_BONE + k);
            }
        }
        return metrics;
    }

}
