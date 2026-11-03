// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pose/HumanExtraBones.h"

#include <algorithm>
#include <cctype>

namespace jasperengine::pose {

    namespace {

        using E = HumanExtraBone;
        using B = HumanBone;

        const char* const FINGER_NAMES[5] = {"thumb", "index", "middle", "ring", "little"};
        const char* const JOINT_NAMES[3] = {"proximal", "intermediate", "distal"};

        struct Table {
            std::array<HumanExtraBoneInfo, HUMAN_EXTRA_BONE_COUNT> bones;
            std::array<std::string, HUMAN_EXTRA_BONE_COUNT> names;
            Table(){
                for (int side = 0; side < 2; side++){
                    for (int finger = 0; finger < 5; finger++){
                        for (int joint = 0; joint < 3; joint++){
                            const int i = side * 15 + finger * 3 + joint;
                            names[(size_t)i] = std::string(side == 0 ? "left_" : "right_") + FINGER_NAMES[finger] + "_" + JOINT_NAMES[joint];
                            const int mirror = (1 - side) * 15 + finger * 3 + joint;
                            bones[(size_t)i] = {nullptr, side == 0 ? B::LeftHand : B::RightHand, extraAt(mirror),
                                                side == 0 ? BodySide::Left : BodySide::Right, finger, joint};
                        }
                    }
                }
                names[(size_t)extraIndex(E::LeftEye)] = "left_eye";
                names[(size_t)extraIndex(E::RightEye)] = "right_eye";
                names[(size_t)extraIndex(E::Jaw)] = "jaw";
                bones[(size_t)extraIndex(E::LeftEye)] = {nullptr, B::Head, E::RightEye, BodySide::Left, -1, -1};
                bones[(size_t)extraIndex(E::RightEye)] = {nullptr, B::Head, E::LeftEye, BodySide::Right, -1, -1};
                bones[(size_t)extraIndex(E::Jaw)] = {nullptr, B::Head, E::Jaw, BodySide::Center, -1, -1};
                for (int i = 0; i < HUMAN_EXTRA_BONE_COUNT; i++) bones[(size_t)i].name = names[(size_t)i].c_str();
            }
        };

        const Table& table(){
            static const Table instance;
            return instance;
        }

        bool oneOf(const std::string& s, std::initializer_list<const char*> values){
            for (const char* v : values){
                if (s == v) return true;
            }
            return false;
        }

        bool isNumber(const std::string& token){
            if (token.empty()) return false;
            for (char c : token){
                if (!std::isdigit(static_cast<unsigned char>(c))) return false;
            }
            return true;
        }

        // the finger a bone name names (-1: none), and whether it is a tip or a metacarpal
        struct FingerName {
            int finger = -1;
            bool skipped = false;
        };

        FingerName fingerOf(const std::string& boneName){
            FingerName out;
            const std::vector<std::string> tokens = humanoidNameTokens(boneName);
            for (const std::string& token : tokens){
                int finger = -1;
                if (oneOf(token, {"thumb", "thb"})) finger = 0;
                else if (oneOf(token, {"index", "pointer"})) finger = 1;
                else if (token == "middle") finger = 2;
                else if (token == "ring") finger = 3;
                else if (oneOf(token, {"little", "pinky"})) finger = 4;
                if (finger >= 0 && out.finger < 0) out.finger = finger;
                if (oneOf(token, {"end", "nub", "tip", "metacarpal", "meta", "carpal", "twist", "helper", "ik", "target"})) out.skipped = true;
            }
            // a trailing ordinal: 4 is the tip of a three-joint finger, 0 its metacarpal
            for (size_t i = tokens.size(); i-- > 0;){
                if (isNumber(tokens[i])){
                    const int value = std::atoi(tokens[i].c_str());
                    if (value == 4 || value == 0) out.skipped = true;
                    break;
                }
                if (!oneOf(tokens[i], {"l", "r", "left", "right"})) break;
            }
            return out;
        }

        BodySide sideOf(const std::string& boneName){
            bool left = false;
            bool right = false;
            for (const std::string& token : humanoidNameTokens(boneName)){
                if (token == "left" || token == "l") left = true;
                else if (token == "right" || token == "r") right = true;
            }
            if (left == right){
                // "lefteye" / "righteye" (lower case, unsplit)
                std::string all;
                for (const std::string& token : humanoidNameTokens(boneName)) all += token;
                if (all.rfind("left", 0) == 0) return BodySide::Left;
                if (all.rfind("right", 0) == 0) return BodySide::Right;
                return BodySide::Center;
            }
            return left ? BodySide::Left : BodySide::Right;
        }

        bool hasToken(const std::string& boneName, std::initializer_list<const char*> values){
            for (const std::string& token : humanoidNameTokens(boneName)){
                if (oneOf(token, values)) return true;
                // "lefteye", "jawbone": unsplit lower-case names
                for (const char* value : values){
                    const std::string v(value);
                    if (token.size() > v.size() && (token.compare(token.size() - v.size(), v.size(), v) == 0 ||
                                                    token.compare(0, v.size(), v) == 0)){
                        const std::string rest = token.compare(0, v.size(), v) == 0 ? token.substr(v.size()) : token.substr(0, token.size() - v.size());
                        if (oneOf(rest, {"left", "right", "l", "r", "bone"})) return true;
                    }
                }
            }
            return false;
        }

        std::vector<int> depths(const Skeleton& skeleton){
            const int count = static_cast<int>(skeleton.bones.size());
            std::vector<int> depth(count, 0);
            for (int i = 0; i < count; i++){
                int guard = 0;
                for (int b = skeleton.bones[i].parent; b >= 0 && b < count && guard < count; b = skeleton.bones[b].parent, guard++){
                    depth[i]++;
                }
            }
            return depth;
        }

        bool descends(const Skeleton& skeleton, int ancestor, int bone){
            const int count = static_cast<int>(skeleton.bones.size());
            if (ancestor < 0 || bone < 0) return false;
            int guard = 0;
            for (int b = skeleton.bones[bone].parent; b >= 0 && b < count && guard < count; b = skeleton.bones[b].parent, guard++){
                if (b == ancestor) return true;
            }
            return false;
        }

        // the slot bones (never used again for an extra bone)
        std::vector<bool> slotBones(const Skeleton& skeleton, const HumanoidMap& humanoid){
            std::vector<bool> used(skeleton.bones.size(), false);
            for (const std::string& name : humanoid.bones){
                const int bone = findSkeletonBone(skeleton, name);
                if (bone >= 0) used[(size_t)bone] = true;
            }
            return used;
        }

        // the candidate nearest the root (then skeleton order); notes the others
        int pick(const Skeleton& skeleton, const std::vector<int>& depth, std::vector<int> candidates, const char* what,
                 std::vector<std::string>* notes){
            if (candidates.empty()) return -1;
            std::stable_sort(candidates.begin(), candidates.end(), [&](int a, int b){ return depth[(size_t)a] < depth[(size_t)b]; });
            if (candidates.size() > 1 && notes){
                std::string line = std::string(what) + ": " + std::to_string(candidates.size()) + " candidates (";
                for (size_t i = 0; i < candidates.size(); i++){
                    line += (i ? ", " : "") + skeleton.bones[(size_t)candidates[i]].name;
                }
                line += "), chose '" + skeleton.bones[(size_t)candidates[0]].name + "' (nearest the root)";
                notes->push_back(line);
            }
            return candidates[0];
        }

    }

    const HumanExtraBoneInfo& humanExtraBoneInfo(HumanExtraBone bone){
        return table().bones[(size_t)extraIndex(bone)];
    }

    const char* humanExtraBoneName(HumanExtraBone bone){
        return bone == HumanExtraBone::Count ? "" : table().bones[(size_t)extraIndex(bone)].name;
    }

    bool humanExtraBoneFromName(const std::string& name, HumanExtraBone& bone){
        for (int i = 0; i < HUMAN_EXTRA_BONE_COUNT; i++){
            if (table().names[(size_t)i] == name){
                bone = extraAt(i);
                return true;
            }
        }
        return false;
    }

    HumanExtraMap autoDetectExtraBones(const Skeleton& skeleton, const HumanoidMap& humanoid, std::vector<std::string>* notes){
        HumanExtraMap out;
        const int count = static_cast<int>(skeleton.bones.size());
        if (count == 0) return out;
        const std::vector<int> depth = depths(skeleton);
        const std::vector<bool> used = slotBones(skeleton, humanoid);

        // fingers, per hand
        for (int side = 0; side < 2; side++){
            const int hand = findSkeletonBone(skeleton, humanoid.bones[(size_t)boneIndex(side == 0 ? B::LeftHand : B::RightHand)]);
            if (hand < 0) continue;
            for (int finger = 0; finger < 5; finger++){
                std::vector<int> candidates;
                for (int i = 0; i < count; i++){
                    if (used[(size_t)i] || !descends(skeleton, hand, i)) continue;
                    const FingerName name = fingerOf(skeleton.bones[(size_t)i].name);
                    if (name.finger == finger && !name.skipped) candidates.push_back(i);
                }
                if (candidates.empty()) continue;
                // the chain of the candidate nearest the root: it and its candidate descendants by depth
                const std::string what = std::string(side == 0 ? "left_" : "right_") + FINGER_NAMES[finger];
                std::vector<int> roots;
                for (int c : candidates){
                    bool hasAncestor = false;
                    for (int o : candidates) hasAncestor = hasAncestor || (o != c && descends(skeleton, o, c));
                    if (!hasAncestor) roots.push_back(c);
                }
                const int first = pick(skeleton, depth, roots, (what + " chain").c_str(), notes);
                std::vector<int> chain = {first};
                for (int c : candidates){
                    if (descends(skeleton, first, c)) chain.push_back(c);
                }
                std::stable_sort(chain.begin(), chain.end(), [&](int a, int b){ return depth[(size_t)a] < depth[(size_t)b]; });
                for (size_t j = 1; j < chain.size(); j++){
                    if (depth[(size_t)chain[j]] == depth[(size_t)chain[j - 1]] && notes){
                        notes->push_back(what + ": '" + skeleton.bones[(size_t)chain[j - 1]].name + "' and '" +
                                         skeleton.bones[(size_t)chain[j]].name + "' are at the same depth: the first is used");
                    }
                }
                // one bone per depth (the first in skeleton order)
                std::vector<int> joints;
                for (int c : chain){
                    if (joints.empty() || depth[(size_t)c] != depth[(size_t)joints.back()]) joints.push_back(c);
                }
                if (joints.size() > 3 && notes){
                    notes->push_back(what + ": " + std::to_string(joints.size()) + " joints, the first three are used");
                }
                for (int joint = 0; joint < 3 && joint < (int)joints.size(); joint++){
                    out.bones[(size_t)(side * 15 + finger * 3 + joint)] = skeleton.bones[(size_t)joints[(size_t)joint]].name;
                }
            }
        }

        // eyes and jaw, under the head
        const int head = findSkeletonBone(skeleton, humanoid.bones[(size_t)boneIndex(B::Head)]);
        if (head >= 0){
            std::vector<int> leftEyes, rightEyes, jaws;
            for (int i = 0; i < count; i++){
                if (used[(size_t)i] || !descends(skeleton, head, i)) continue;
                const std::string& name = skeleton.bones[(size_t)i].name;
                if (hasToken(name, {"end", "nub", "tip", "lid", "eyelid", "brow", "eyebrow", "lash", "eyelash", "target"})) continue;
                if (hasToken(name, {"eye", "eyeball"})){
                    const BodySide side = sideOf(name);
                    if (side == BodySide::Left) leftEyes.push_back(i);
                    else if (side == BodySide::Right) rightEyes.push_back(i);
                }else if (hasToken(name, {"jaw"})){
                    jaws.push_back(i);
                }
            }
            const int leftEye = pick(skeleton, depth, leftEyes, "left_eye", notes);
            const int rightEye = pick(skeleton, depth, rightEyes, "right_eye", notes);
            const int jaw = pick(skeleton, depth, jaws, "jaw", notes);
            if (leftEye >= 0) out.bones[(size_t)extraIndex(E::LeftEye)] = skeleton.bones[(size_t)leftEye].name;
            if (rightEye >= 0) out.bones[(size_t)extraIndex(E::RightEye)] = skeleton.bones[(size_t)rightEye].name;
            if (jaw >= 0) out.bones[(size_t)extraIndex(E::Jaw)] = skeleton.bones[(size_t)jaw].name;
        }

        // names must be unique to be mapped (a duplicate name can't be told apart)
        for (int x = 0; x < HUMAN_EXTRA_BONE_COUNT; x++){
            std::string& name = out.bones[(size_t)x];
            if (name.empty()) continue;
            int matches = 0;
            for (const SkeletonBone& bone : skeleton.bones) matches += bone.name == name ? 1 : 0;
            if (matches != 1){
                if (notes) notes->push_back(std::string(humanExtraBoneName(extraAt(x))) + ": bone name '" + name + "' is not unique, left unmapped");
                name.clear();
            }
        }
        return out;
    }

    bool validateExtraBones(const Skeleton& skeleton, const HumanoidMap& humanoid, const HumanExtraMap& extra, std::string* error){
        std::vector<std::string> problems;
        const int count = static_cast<int>(skeleton.bones.size());
        const std::vector<bool> used = slotBones(skeleton, humanoid);
        for (int x = 0; x < HUMAN_EXTRA_BONE_COUNT; x++){
            const std::string& name = extra.bones[(size_t)x];
            if (name.empty()) continue;
            const char* what = humanExtraBoneName(extraAt(x));
            int found = -1;
            int matches = 0;
            for (int i = 0; i < count; i++){
                if (skeleton.bones[(size_t)i].name == name){
                    if (found < 0) found = i;
                    matches++;
                }
            }
            if (matches == 0){
                problems.push_back(std::string(what) + ": no bone named '" + name + "'");
                continue;
            }
            if (matches > 1){
                problems.push_back(std::string(what) + ": bone name '" + name + "' is not unique");
                continue;
            }
            if (used[(size_t)found]){
                problems.push_back(std::string(what) + ": '" + name + "' is a body slot's bone");
            }
            for (int o = 0; o < x; o++){
                if (extra.bones[(size_t)o] == name){
                    problems.push_back("bone '" + name + "' is mapped to both " + humanExtraBoneName(extraAt(o)) + " and " + what);
                }
            }
            const HumanExtraBoneInfo& info = humanExtraBoneInfo(extraAt(x));
            const int parent = findSkeletonBone(skeleton, humanoid.bones[(size_t)boneIndex(info.parent)]);
            if (parent < 0 || !descends(skeleton, parent, found)){
                problems.push_back(std::string(what) + ": '" + name + "' is not under the " + humanBoneName(info.parent) +
                                   (parent < 0 ? " (unmapped)" : " ('" + humanoid.bones[(size_t)boneIndex(info.parent)] + "')"));
            }
        }
        if (!problems.empty() && error){
            std::string text;
            for (size_t i = 0; i < problems.size(); i++) text += (i ? "; " : "") + problems[i];
            *error = text;
        }
        return problems.empty();
    }

    bool resolveExtraBones(const Skeleton& skeleton, const HumanoidMap& humanoid,
                           const std::array<std::optional<std::string>, HUMAN_EXTRA_BONE_COUNT>& overrides,
                           HumanExtraMap& out, std::string* error, std::vector<std::string>* notes){
        out = autoDetectExtraBones(skeleton, humanoid, notes);
        for (int x = 0; x < HUMAN_EXTRA_BONE_COUNT; x++){
            const std::optional<std::string>& value = overrides[(size_t)x];
            if (!value.has_value()) continue;
            const char* what = humanExtraBoneName(extraAt(x));
            if (value->empty()){
                out.bones[(size_t)x].clear();
                if (notes) notes->push_back(std::string(what) + ": unmapped by the sidecar");
                continue;
            }
            if (findSkeletonBone(skeleton, *value) < 0){
                if (error) *error = std::string("humanoid sidecar: extra_bones.") + what + ": the skeleton has no bone '" + *value + "'";
                return false;
            }
            for (int o = 0; o < HUMAN_EXTRA_BONE_COUNT; o++){
                if (o != x && out.bones[(size_t)o] == *value && !overrides[(size_t)o].has_value()){
                    out.bones[(size_t)o].clear();
                    if (notes) notes->push_back(std::string(humanExtraBoneName(extraAt(o))) + ": unmapped, its bone '" + *value + "' is forced to " + what);
                }
            }
            out.bones[(size_t)x] = *value;
            if (notes) notes->push_back(std::string(what) + ": '" + *value + "' (sidecar)");
        }
        std::string problems;
        if (!validateExtraBones(skeleton, humanoid, out, &problems)){
            if (error) *error = "humanoid sidecar: extra_bones: " + problems;
            return false;
        }
        return true;
    }


    bool resolveExtraBonesOrAuto(const Skeleton& skeleton, const HumanoidMap& humanoid,
                                 const std::array<std::optional<std::string>, HUMAN_EXTRA_BONE_COUNT>& overrides,
                                 HumanExtraMap& out, std::string* error, std::vector<std::string>* notes){
        if (error) error->clear();
        std::string problem;
        std::vector<std::string> applied;
        if (resolveExtraBones(skeleton, humanoid, overrides, out, &problem, notes ? &applied : nullptr)){
            if (notes) notes->insert(notes->end(), applied.begin(), applied.end());
            return true;
        }
        out = autoDetectExtraBones(skeleton, humanoid, notes);
        if (notes) notes->push_back(problem + " (fingers, eyes and jaw auto-detected instead)");
        if (error) *error = problem;
        return false;
    }

}
