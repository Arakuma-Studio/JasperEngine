// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pose/HumanoidMap.h"

#include <algorithm>
#include <cctype>

namespace jasperengine::pose {

    namespace {

        using B = HumanBone;

        enum class Part : uint8_t {
            None, Hips, SpineChain, Neck, Head,
            Shoulder, UpperArm, LowerArm, Hand, UpperLeg, LowerLeg, Foot, Toes
        };

        struct NameInfo {
            Part part = Part::None;
            BodySide side = BodySide::Center;
        };

        char lower(char c){
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        bool startsWithNoCase(const std::string& s, size_t pos, const char* prefix){
            for (size_t i = 0; prefix[i] != '\0'; i++){
                if (pos + i >= s.size() || lower(s[pos + i]) != prefix[i]){
                    return false;
                }
            }
            return true;
        }

        bool isSeparator(char c){
            return !std::isalnum(static_cast<unsigned char>(c));
        }

        bool startsWith(const std::string& s, const char* prefix){
            return s.rfind(prefix, 0) == 0;
        }

        bool oneOf(const std::string& s, std::initializer_list<const char*> values){
            for (const char* v : values){
                if (s == v){
                    return true;
                }
            }
            return false;
        }

        // Body part and side of a bone name (section 4.6)
        NameInfo classify(const std::string& boneName){
            NameInfo info;
            const std::vector<std::string> tokens = humanoidNameTokens(boneName);
            if (tokens.empty()){
                return info;
            }

            bool left = false;
            bool right = false;
            std::string key;
            for (const std::string& token : tokens){
                // twist/roll helpers and IK/end bones are never mapped
                if (oneOf(token, {"twist", "roll", "helper", "ik", "pole", "target", "end", "nub"})){
                    return info;
                }
                // fingers (not animated by the pose tool): "LeftHandIndex1" must not become the hand
                if (oneOf(token, {"index", "middle", "ring", "pinky", "little", "thumb", "finger", "fingers"})){
                    return info;
                }
                if (token == "left" || token == "l"){
                    left = true;
                }else if (token == "right" || token == "r"){
                    right = true;
                }else if (!oneOf(token, {"c", "center", "centre"})){
                    key += token;
                }
            }
            // the same inside unsplit lower-case names ("lefthandindex1", "upperarmtwist")
            for (const char* part : {"twist", "helper", "index", "thumb", "pinky", "finger"}){
                if (key.find(part) != std::string::npos){
                    return info;
                }
            }
            // all-lower-case names without separators: "leftarm"
            if (!left && !right){
                if (startsWith(key, "left") && key.size() > 4){
                    left = true;
                    key = key.substr(4);
                }else if (startsWith(key, "right") && key.size() > 5){
                    right = true;
                    key = key.substr(5);
                }
            }
            if (left && right){
                return info;   // contradictory side tokens
            }
            const BodySide side = left ? BodySide::Left : (right ? BodySide::Right : BodySide::Center);

            Part part = Part::None;
            if (side == BodySide::Center){
                if (oneOf(key, {"hips", "pelvis", "hip"})){
                    part = Part::Hips;
                }else if (startsWith(key, "spine") || oneOf(key, {"chest", "upperchest", "torso"})){
                    part = Part::SpineChain;
                }else if (startsWith(key, "neck")){
                    part = Part::Neck;
                }else if (key == "head"){
                    part = Part::Head;
                }
            }else{
                if (oneOf(key, {"shoulder", "clavicle", "collar"})){
                    part = Part::Shoulder;
                }else if (oneOf(key, {"upperarm", "arm", "uparm"})){
                    part = Part::UpperArm;
                }else if (oneOf(key, {"forearm", "lowerarm", "elbow"})){
                    part = Part::LowerArm;
                }else if (oneOf(key, {"hand", "wrist"})){
                    part = Part::Hand;
                }else if (oneOf(key, {"upleg", "upperleg", "thigh"})){
                    part = Part::UpperLeg;
                }else if (oneOf(key, {"leg", "lowerleg", "calf", "shin", "knee"})){
                    part = Part::LowerLeg;
                }else if (oneOf(key, {"foot", "ankle"})){
                    part = Part::Foot;
                }else if (startsWith(key, "toe") || key == "ball"){
                    part = Part::Toes;
                }
            }
            info.part = part;
            info.side = part == Part::None ? BodySide::Center : side;
            return info;
        }

        // depth of every bone (distance to its root); -1 for bones on a parent cycle or with an
        // invalid parent index
        std::vector<int> boneDepths(const Skeleton& skeleton){
            const int count = static_cast<int>(skeleton.bones.size());
            std::vector<int> depth(count, -2);   // -2 = not computed
            for (int i = 0; i < count; i++){
                std::vector<int> path;
                int b = i;
                int found = -1;
                while (true){
                    if (b == -1){
                        found = -1;   // reached a root: depth of the root is 0
                        break;
                    }
                    if (b < 0 || b >= count || depth[b] == -1){
                        found = -3;   // invalid
                        break;
                    }
                    if (depth[b] >= 0){
                        found = b;
                        break;
                    }
                    if (std::find(path.begin(), path.end(), b) != path.end()){
                        found = -3;   // cycle
                        break;
                    }
                    path.push_back(b);
                    b = skeleton.bones[b].parent;
                }
                if (found == -3){
                    for (int p : path){
                        depth[p] = -1;
                    }
                    continue;
                }
                int d = found >= 0 ? depth[found] + 1 : 0;
                for (auto it = path.rbegin(); it != path.rend(); ++it){
                    depth[*it] = d++;
                }
            }
            return depth;
        }

        bool isAncestor(const Skeleton& skeleton, const std::vector<int>& depth, int ancestor, int bone){
            if (bone < 0 || depth[bone] < 0){
                return false;
            }
            int b = skeleton.bones[bone].parent;
            while (b >= 0){
                if (b == ancestor){
                    return true;
                }
                b = skeleton.bones[b].parent;
            }
            return false;
        }

        int lowestCommonAncestor(const Skeleton& skeleton, const std::vector<int>& depth, int a, int b){
            if (a < 0 || b < 0 || depth[a] < 0 || depth[b] < 0){
                return -1;
            }
            while (depth[a] > depth[b]) a = skeleton.bones[a].parent;
            while (depth[b] > depth[a]) b = skeleton.bones[b].parent;
            while (a != b && a >= 0 && b >= 0){
                a = skeleton.bones[a].parent;
                b = skeleton.bones[b].parent;
            }
            return a == b ? a : -1;
        }

        // the candidate nearest the root (then the first in skeleton order); notes ambiguities
        int pickCandidate(const Skeleton& skeleton, const std::vector<int>& depth, std::vector<int> candidates,
                          HumanBone slot, std::vector<std::string>* notes){
            if (candidates.empty()){
                return -1;
            }
            std::stable_sort(candidates.begin(), candidates.end(), [&](int a, int b){ return depth[a] < depth[b]; });
            if (candidates.size() > 1 && notes){
                std::string line = std::string(humanBoneName(slot)) + ": " + std::to_string(candidates.size()) + " candidates (";
                for (size_t i = 0; i < candidates.size(); i++){
                    line += (i ? ", " : "") + skeleton.bones[candidates[i]].name;
                }
                line += "), chose '" + skeleton.bones[candidates[0]].name + "' (nearest the root)";
                notes->push_back(line);
            }
            return candidates[0];
        }

        HumanBone sided(Part part, BodySide side){
            const bool left = side == BodySide::Left;
            switch (part){
                case Part::Shoulder: return left ? B::LeftShoulder : B::RightShoulder;
                case Part::UpperArm: return left ? B::LeftUpperArm : B::RightUpperArm;
                case Part::LowerArm: return left ? B::LeftLowerArm : B::RightLowerArm;
                case Part::Hand:     return left ? B::LeftHand : B::RightHand;
                case Part::UpperLeg: return left ? B::LeftUpperLeg : B::RightUpperLeg;
                case Part::LowerLeg: return left ? B::LeftLowerLeg : B::RightLowerLeg;
                case Part::Foot:     return left ? B::LeftFoot : B::RightFoot;
                case Part::Toes:     return left ? B::LeftToes : B::RightToes;
                default: return B::Count;
            }
        }

    }

    std::vector<std::string> humanoidNameTokens(const std::string& boneName){
        // namespace / path prefix up to the last ':' or '|'
        std::string name = boneName;
        const size_t ns = name.find_last_of(":|");
        if (ns != std::string::npos){
            name = name.substr(ns + 1);
        }

        // rig prefixes (repeatedly, with the separators after them)
        static const char* PREFIXES[] = {"mixamorig", "bip001", "bip01", "def-", "def_", "j_bip_", "b_", "bone_"};
        bool stripped = true;
        while (stripped && !name.empty()){
            stripped = false;
            size_t pos = 0;
            while (pos < name.size() && isSeparator(name[pos])) pos++;
            for (const char* prefix : PREFIXES){
                if (startsWithNoCase(name, pos, prefix)){
                    pos += std::char_traits<char>::length(prefix);
                    if (startsWith(prefix, "mixamorig")){
                        while (pos < name.size() && std::isdigit(static_cast<unsigned char>(name[pos]))) pos++;
                    }
                    stripped = true;
                    break;
                }
            }
            while (pos < name.size() && isSeparator(name[pos])) pos++;
            name = name.substr(pos);
        }

        // split on separators, camelCase and letter/digit boundaries
        std::vector<std::string> tokens;
        std::string current;
        auto flush = [&](){
            if (!current.empty()){
                tokens.push_back(current);
                current.clear();
            }
        };
        for (size_t i = 0; i < name.size(); i++){
            const unsigned char c = static_cast<unsigned char>(name[i]);
            if (isSeparator(name[i])){
                flush();
                continue;
            }
            if (!current.empty()){
                const unsigned char prev = static_cast<unsigned char>(name[i - 1]);
                const bool next_lower = i + 1 < name.size() && std::islower(static_cast<unsigned char>(name[i + 1]));
                const bool boundary =
                    (std::isupper(c) && std::islower(prev)) ||                       // camelCase
                    (std::isupper(c) && std::isupper(prev) && next_lower) ||         // "LUpper" -> L, Upper
                    (std::isdigit(c) && std::isalpha(prev)) || (std::isalpha(c) && std::isdigit(prev));
                if (boundary){
                    flush();
                }
            }
            current += lower(name[i]);
        }
        flush();
        return tokens;
    }

    int findSkeletonBone(const Skeleton& skeleton, const std::string& name){
        if (name.empty()){
            return -1;
        }
        for (size_t i = 0; i < skeleton.bones.size(); i++){
            if (skeleton.bones[i].name == name){
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    HumanBone effectiveParentSlot(const HumanoidMap& map, HumanBone bone){
        HumanBone p = humanBoneInfo(bone).parent;
        while (p != HumanBone::Count && map.bones[boneIndex(p)].empty()){
            p = humanBoneInfo(p).parent;
        }
        return p;
    }

    HumanoidMap autoDetectHumanoid(const Skeleton& skeleton, std::vector<std::string>* notes){
        HumanoidMap map;
        const int count = static_cast<int>(skeleton.bones.size());
        const std::vector<int> depth = boneDepths(skeleton);

        std::vector<NameInfo> infos(count);
        for (int i = 0; i < count; i++){
            if (depth[i] >= 0){
                infos[i] = classify(skeleton.bones[i].name);
            }
        }
        auto candidatesOf = [&](Part part, BodySide side){
            std::vector<int> result;
            for (int i = 0; i < count; i++){
                if (infos[i].part == part && infos[i].side == side){
                    result.push_back(i);
                }
            }
            return result;
        };
        auto setSlot = [&](HumanBone slot, int bone){
            if (bone >= 0){
                map.bones[boneIndex(slot)] = skeleton.bones[bone].name;
            }
        };

        // limbs
        int slotBone[HUMAN_BONE_COUNT];
        std::fill(std::begin(slotBone), std::end(slotBone), -1);
        for (Part part : {Part::Shoulder, Part::UpperArm, Part::LowerArm, Part::Hand,
                          Part::UpperLeg, Part::LowerLeg, Part::Foot, Part::Toes}){
            for (BodySide side : {BodySide::Left, BodySide::Right}){
                const HumanBone slot = sided(part, side);
                const int bone = pickCandidate(skeleton, depth, candidatesOf(part, side), slot, notes);
                slotBone[boneIndex(slot)] = bone;
                setSlot(slot, bone);
            }
        }

        // hips: by name, else the lowest common ancestor of both upper legs
        int hips = pickCandidate(skeleton, depth, candidatesOf(Part::Hips, BodySide::Center), B::Hips, notes);
        if (hips < 0){
            hips = lowestCommonAncestor(skeleton, depth, slotBone[boneIndex(B::LeftUpperLeg)], slotBone[boneIndex(B::RightUpperLeg)]);
            if (hips >= 0 && notes){
                notes->push_back("hips: no hips/pelvis name, mapped '" + skeleton.bones[hips].name +
                                 "' (lowest common ancestor of the upper legs)");
            }
        }
        setSlot(B::Hips, hips);

        auto under = [&](std::vector<int> candidates, int ancestor){
            if (ancestor < 0){
                return candidates;
            }
            std::vector<int> result;
            for (int c : candidates){
                if (isAncestor(skeleton, depth, ancestor, c)){
                    result.push_back(c);
                }
            }
            return result;
        };

        // neck and head
        const int neck = pickCandidate(skeleton, depth, under(candidatesOf(Part::Neck, BodySide::Center), hips), B::Neck, notes);
        setSlot(B::Neck, neck);
        const int head = pickCandidate(skeleton, depth, under(candidatesOf(Part::Head, BodySide::Center), neck >= 0 ? neck : hips),
                                       B::Head, notes);
        setSlot(B::Head, head);

        // spine chain: centre spine-named bones between the hips and the neck (or head)
        std::vector<int> chain;
        const int chainEnd = neck >= 0 ? neck : head;
        bool walked = false;
        if (chainEnd >= 0 && hips >= 0 && isAncestor(skeleton, depth, hips, chainEnd)){
            for (int b = skeleton.bones[chainEnd].parent; b >= 0 && b != hips; b = skeleton.bones[b].parent){
                if (infos[b].part == Part::SpineChain){
                    chain.push_back(b);
                }
            }
            std::reverse(chain.begin(), chain.end());
            walked = true;
        }
        if (!walked){
            chain = under(candidatesOf(Part::SpineChain, BodySide::Center), hips);
            std::stable_sort(chain.begin(), chain.end(), [&](int a, int b){ return depth[a] < depth[b]; });
        }
        if (!chain.empty()){
            const size_t n = chain.size();
            setSlot(B::Spine, chain[0]);
            if (n == 2){
                setSlot(B::Chest, chain[1]);
            }else if (n >= 3){
                setSlot(B::Chest, chain[n == 3 ? 1 : n / 2]);
                setSlot(B::UpperChest, chain[n - 1]);
            }
            if (n > 3 && notes){
                notes->push_back("spine: " + std::to_string(n) + " spine bones, chest = '" + skeleton.bones[chain[n / 2]].name +
                                 "', upper_chest = '" + skeleton.bones[chain[n - 1]].name + "'");
            }
        }

        return map;
    }

    bool validateHumanoid(const Skeleton& skeleton, const HumanoidMap& map, std::string* error){
        std::vector<std::string> problems;
        const int count = static_cast<int>(skeleton.bones.size());

        for (int i = 0; i < count; i++){
            const int parent = skeleton.bones[i].parent;
            if (parent < -1 || parent >= count || parent == i){
                problems.push_back("bone '" + skeleton.bones[i].name + "' has an invalid parent index");
            }
        }
        const std::vector<int> depth = boneDepths(skeleton);
        for (int i = 0; i < count; i++){
            if (depth[i] < 0){
                problems.push_back("bone '" + skeleton.bones[i].name + "' is on a parent cycle");
                break;
            }
        }

        std::string missing;
        for (int s = 0; s < HUMAN_BONE_COUNT; s++){
            if (humanBoneInfo(boneAt(s)).required && map.bones[s].empty()){
                missing += (missing.empty() ? "" : ", ") + std::string(humanBoneName(boneAt(s)));
            }
        }
        if (!missing.empty()){
            problems.push_back("missing required slots: " + missing);
        }

        int slotBone[HUMAN_BONE_COUNT];
        for (int s = 0; s < HUMAN_BONE_COUNT; s++){
            slotBone[s] = -1;
            const std::string& name = map.bones[s];
            if (name.empty()){
                continue;
            }
            int found = -1;
            int matches = 0;
            for (int i = 0; i < count; i++){
                if (skeleton.bones[i].name == name){
                    if (found < 0) found = i;
                    matches++;
                }
            }
            if (matches == 0){
                problems.push_back(std::string(humanBoneName(boneAt(s))) + ": no bone named '" + name + "'");
            }else if (matches > 1){
                problems.push_back(std::string(humanBoneName(boneAt(s))) + ": bone name '" + name + "' is not unique");
            }else{
                slotBone[s] = found;
            }
            for (int o = 0; o < s; o++){
                if (map.bones[o] == name){
                    problems.push_back("bone '" + name + "' is mapped to both " + humanBoneName(boneAt(o)) + " and " + humanBoneName(boneAt(s)));
                }
            }
        }

        bool cyclic = false;
        for (int i = 0; i < count; i++){
            cyclic = cyclic || depth[i] < 0;
        }
        if (!cyclic){
            for (int s = 1; s < HUMAN_BONE_COUNT; s++){
                if (slotBone[s] < 0){
                    continue;
                }
                const HumanBone p = effectiveParentSlot(map, boneAt(s));
                if (p == HumanBone::Count || slotBone[boneIndex(p)] < 0){
                    continue;   // reported above (hips missing or not found)
                }
                const int parentBone = slotBone[boneIndex(p)];
                bool reached = false;
                std::string between;
                for (int b = skeleton.bones[slotBone[s]].parent; b >= 0; b = skeleton.bones[b].parent){
                    if (b == parentBone){
                        reached = true;
                        break;
                    }
                    for (int o = 0; o < HUMAN_BONE_COUNT; o++){
                        if (slotBone[o] == b && between.empty()){
                            between = humanBoneName(boneAt(o));
                        }
                    }
                }
                if (!reached){
                    problems.push_back(std::string(humanBoneName(boneAt(s))) + " ('" + map.bones[s] + "') is not a descendant of " +
                                       humanBoneName(p) + " ('" + map.bones[boneIndex(p)] + "')");
                }else if (!between.empty()){
                    problems.push_back(std::string(humanBoneName(boneAt(s))) + ": slot " + between + " lies between it and its parent slot " +
                                       humanBoneName(p));
                }
            }
        }

        if (error){
            error->clear();
            for (size_t i = 0; i < problems.size(); i++){
                *error += (i ? "; " : "") + problems[i];
            }
        }
        return problems.empty();
    }

}
