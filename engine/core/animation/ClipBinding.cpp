// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ClipBinding.h"
#include "component/MeshComponent.h"

#include "animation/AnimationClip.h"
#include "component/Transform.h"
#include "lattice/LatticeSolver.h"
#include "registry/EntityRegistry.h"

#include <algorithm>
#include <unordered_set>

using namespace jasperengine;

namespace {

    bool allDigits(const std::string& text, size_t from){
        if (from >= text.size()) return false;
        for (size_t i = from; i < text.size(); i++){
            if (text[i] < '0' || text[i] > '9') return false;
        }
        return true;
    }

    // Calls fn(child) for every Transform child of `parent` in child (Transform array) order,
    // until fn returns false. The array keeps every branch contiguous after its root
    // (EntityRegistry::addEntityChild, findBranchLastIndex), so a parent's children are found in
    // its branch; for NULL_ENTITY the children are the parentless Transforms.
    template<typename Fn>
    void forEachChild(EntityRegistry* registry, Entity parent, Fn fn){
        auto transforms = registry->getComponentArray<Transform>();
        const size_t count = transforms->size();
        if (parent == NULL_ENTITY){
            for (size_t i = 0; i < count; i++){
                if (transforms->getComponentFromIndex(i).parent == NULL_ENTITY){
                    if (!fn(transforms->getEntity(i))) return;
                }
            }
            return;
        }
        if (!registry->isEntityCreated(parent) || !registry->getSignature(parent).test(registry->getComponentId<Transform>())){
            return;
        }
        const size_t start = transforms->getIndex(parent);
        // the branch so far: a list while short, then a set (findBranchLastIndex's way)
        std::vector<Entity> branchList{parent};
        std::unordered_set<Entity> branchSet;
        for (size_t i = start + 1; i < count; i++){
            const Entity owner = transforms->getComponentFromIndex(i).parent;
            const bool inBranch = branchSet.empty() ?
                std::find(branchList.begin(), branchList.end(), owner) != branchList.end() :
                branchSet.count(owner) != 0;
            if (!inBranch) break;
            const Entity entity = transforms->getEntity(i);
            if (owner == parent){
                if (!fn(entity)) return;
            }
            if (branchSet.empty() && branchList.size() < 32){
                branchList.push_back(entity);
            }else{
                if (branchSet.empty()) branchSet.insert(branchList.begin(), branchList.end());
                branchSet.insert(entity);
            }
        }
    }

    Entity parentOf(EntityRegistry* registry, Entity entity){
        Transform* transform = registry->findComponent<Transform>(entity);
        return transform ? transform->parent : NULL_ENTITY;
    }

}

bool anim::parsePath(const std::string& path, std::vector<PathSegment>& out){
    out.clear();
    if (path.empty()) return true;
    size_t begin = 0;
    while (true){
        const size_t end = path.find('/', begin);
        const std::string text = path.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (text.empty()){
            out.clear();
            return false;
        }
        PathSegment segment;
        const size_t hash = text.rfind('#');
        if (hash == std::string::npos){
            segment.name = text;
        }else if (allDigits(text, hash + 1)){
            // "Name#N": the N-th later sibling of that name
            if (hash == 0 || text.size() - hash - 1 > 9){
                out.clear();
                return false;
            }
            segment.name = text.substr(0, hash);
            segment.index = std::stoi(text.substr(hash + 1));
        }else if (hash + 1 == text.size() || (text[hash + 1] == '-' && allDigits(text, hash + 2))){
            // "Name#" or "Name#-N": a malformed index
            out.clear();
            return false;
        }else{
            // a '#' that is part of the name ("Take #b")
            segment.name = text;
        }
        out.push_back(segment);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return true;
}

std::string anim::formatPath(const std::vector<PathSegment>& segments){
    std::string path;
    for (size_t i = 0; i < segments.size(); i++){
        if (i > 0) path += '/';
        path += formatSegment(segments[i].name, segments[i].index);
    }
    return path;
}

std::string anim::formatSegment(const std::string& name, int index){
    return index > 0 ? name + "#" + std::to_string(index) : name;
}

bool anim::isAddressableName(const std::string& name){
    if (name.empty() || name.find('/') != std::string::npos) return false;
    const size_t hash = name.rfind('#');
    if (hash == std::string::npos) return true;
    // it would read back as an index ("Bone#1") or as a malformed one ("Bone#", "Bone#-1")
    if (hash + 1 == name.size() || allDigits(name, hash + 1)) return false;
    if (name[hash + 1] == '-' && allDigits(name, hash + 2)) return false;
    return true;
}

std::vector<Entity> anim::getChildren(EntityRegistry* registry, Entity parent){
    std::vector<Entity> children;
    if (!registry || parent == NULL_ENTITY) return children;
    forEachChild(registry, parent, [&children](Entity child){
        children.push_back(child);
        return true;
    });
    return children;
}

Entity anim::resolvePath(EntityRegistry* registry, Entity root, const std::string& path, PathResolution* info){
    std::vector<PathSegment> segments;
    if (!parsePath(path, segments)){
        if (info){
            *info = PathResolution();
            info->lastMatched = (registry && root != NULL_ENTITY && registry->isEntityCreated(root)) ? root : NULL_ENTITY;
        }
        return NULL_ENTITY;
    }
    return resolvePath(registry, root, segments, info);
}

Entity anim::resolvePath(EntityRegistry* registry, Entity root, const std::vector<PathSegment>& segments, PathResolution* info){
    PathResolution result;
    if (!registry || root == NULL_ENTITY || !registry->isEntityCreated(root)){
        if (info) *info = result;
        return NULL_ENTITY;
    }
    Entity current = root;
    result.lastMatched = root;
    for (const PathSegment& segment : segments){
        Entity found = NULL_ENTITY;
        int seen = 0;
        forEachChild(registry, current, [&](Entity child){
            if (registry->getEntityName(child) != segment.name) return true;
            if (seen == segment.index) found = child;
            seen++;
            // without "#N" the walk goes on to tell whether the name is shared
            return found == NULL_ENTITY || (segment.index == 0 && seen < 2);
        });
        if (found == NULL_ENTITY){
            if (info) *info = result;
            return NULL_ENTITY;
        }
        if (segment.index == 0 && seen > 1) result.duplicateSibling = true;
        current = found;
        result.lastMatched = found;
        result.resolvedSegments++;
    }
    result.entity = current;
    if (info) *info = result;
    return current;
}

void anim::uniqueNamesUnder(EntityRegistry* registry, Entity root, std::unordered_map<std::string, Entity>& out){
    if (!registry) return;
    auto transforms = registry->getComponentArray<Transform>();
    Transform* rootTransform = transforms->findComponent(root);
    if (!rootTransform) return;
    const size_t first = (size_t)(rootTransform - transforms->findComponentFromIndex(0));
    const size_t last = registry->findBranchLastIndex(root);
    for (size_t i = first + 1; i <= last && i < transforms->size(); i++){
        const Entity entity = transforms->getEntity(i);
        auto inserted = out.emplace(registry->getEntityNameRef(entity), entity);
        if (!inserted.second) inserted.first->second = NULL_ENTITY;
    }
}

bool anim::relativePath(EntityRegistry* registry, Entity root, Entity entity, std::string& out){
    out.clear();
    if (!registry || root == NULL_ENTITY || entity == NULL_ENTITY ||
        !registry->isEntityCreated(root) || !registry->isEntityCreated(entity)){
        return false;
    }
    std::vector<PathSegment> segments;
    Entity current = entity;
    // a parent cycle cannot happen through the registry, the bound only guards bad data
    for (int depth = 0; current != root; depth++){
        if (depth > 1024) return false;
        const Entity parent = parentOf(registry, current);
        if (parent == NULL_ENTITY) return false;
        PathSegment segment;
        segment.name = registry->getEntityName(current);
        if (!isAddressableName(segment.name)) return false;
        forEachChild(registry, parent, [&](Entity child){
            if (child == current) return false;
            if (registry->getEntityName(child) == segment.name) segment.index++;
            return true;
        });
        segments.push_back(segment);
        current = parent;
    }
    std::reverse(segments.begin(), segments.end());
    out = formatPath(segments);
    return true;
}

std::vector<std::pair<std::string, Entity>> anim::descendantPaths(EntityRegistry* registry, Entity root){
    std::vector<std::pair<std::string, Entity>> paths;
    if (!registry || root == NULL_ENTITY || !registry->isEntityCreated(root)) return paths;
    // depth first in child order; a name that can't be addressed hides its branch
    std::vector<std::pair<std::string, Entity>> stack;
    stack.push_back({std::string(), root});
    while (!stack.empty()){
        std::pair<std::string, Entity> current = stack.back();
        stack.pop_back();
        paths.push_back(current);
        std::vector<std::pair<std::string, Entity>> children;
        std::vector<std::pair<std::string, int>> counts;
        forEachChild(registry, current.second, [&](Entity child){
            const std::string name = registry->getEntityName(child);
            int index = 0;
            auto count = std::find_if(counts.begin(), counts.end(),
                                      [&name](const std::pair<std::string, int>& c){ return c.first == name; });
            if (count == counts.end()){
                counts.push_back({name, 1});
            }else{
                index = count->second++;
            }
            if (isAddressableName(name)){
                const std::string segment = formatSegment(name, index);
                children.push_back({current.first.empty() ? segment : current.first + "/" + segment, child});
            }
            return true;
        });
        for (auto it = children.rbegin(); it != children.rend(); ++it){
            stack.push_back(*it);
        }
    }
    return paths;
}

// ---- channel values (complete: what the flush writes, as data) ----

bool anim::readChannelValue(EntityRegistry* registry, Entity entity, ClipChannel channel,
                            const std::vector<int>& points, std::vector<float>& out){
    out.clear();
    if (!registry || entity == NULL_ENTITY || !registry->isEntityCreated(entity)) return false;
    switch (channel){
        case ClipChannel::Position:
        case ClipChannel::Scale: {
            Transform* transform = registry->findComponent<Transform>(entity);
            if (!transform) return false;
            const Vector3& v = channel == ClipChannel::Position ? transform->position : transform->scale;
            out = {v.x, v.y, v.z};
            return true;
        }
        case ClipChannel::Rotation: {
            Transform* transform = registry->findComponent<Transform>(entity);
            if (!transform) return false;
            out.resize(4);
            quaternionToXYZW(transform->rotation, out.data());
            return true;
        }
        case ClipChannel::Morph: {
            MeshComponent* mesh = registry->findComponent<MeshComponent>(entity);
            if (!mesh) return false;
            out.assign(mesh->morphWeights, mesh->morphWeights + MAX_MORPHTARGETS);
            return true;
        }
        case ClipChannel::Lattice: {
            LatticeComponent* latticecomp = registry->findComponent<LatticeComponent>(entity);
            if (!latticecomp) return false;
            const int pointCount = lattice::pointCount(latticecomp->resolutionX, latticecomp->resolutionY, latticecomp->resolutionZ);
            auto offsetOf = [latticecomp](int point){
                return (size_t)point < latticecomp->offsets.size() ? latticecomp->offsets[(size_t)point] : Vector3(0, 0, 0);
            };
            if (points.empty()){
                for (int p = 0; p < pointCount; p++){
                    Vector3 v = offsetOf(p);
                    out.insert(out.end(), {v.x, v.y, v.z});
                }
                return true;
            }
            for (int p : points){
                if (p < 0 || p >= pointCount){
                    out.clear();
                    return false;
                }
                Vector3 v = offsetOf(p);
                out.insert(out.end(), {v.x, v.y, v.z});
            }
            return true;
        }
        case ClipChannel::Property:
        case ClipChannel::Humanoid:
        case ClipChannel::HumanoidPosition:
            // not entity channels: a humanoid track is realised on a rig (HumanoidClip.h)
            return false;
    }
    return false;
}

bool anim::writeChannelValue(EntityRegistry* registry, Entity entity, ClipChannel channel,
                             const std::vector<int>& points, const std::vector<float>& value){
    if (!registry || entity == NULL_ENTITY || !registry->isEntityCreated(entity)) return false;
    switch (channel){
        case ClipChannel::Position:
        case ClipChannel::Scale: {
            Transform* transform = registry->findComponent<Transform>(entity);
            if (!transform || value.size() < 3) return false;
            Vector3 v(value[0], value[1], value[2]);
            if (channel == ClipChannel::Position) transform->position = v;
            else transform->scale = v;
            transform->needUpdate = true;
            return true;
        }
        case ClipChannel::Rotation: {
            Transform* transform = registry->findComponent<Transform>(entity);
            if (!transform || value.size() < 4) return false;
            transform->rotation = quaternionFromXYZW(value.data());
            transform->needUpdate = true;
            return true;
        }
        case ClipChannel::Morph: {
            MeshComponent* mesh = registry->findComponent<MeshComponent>(entity);
            if (!mesh) return false;
            for (size_t i = 0; i < value.size() && i < (size_t)MAX_MORPHTARGETS; i++){
                mesh->morphWeights[i] = value[i];
            }
            return true;
        }
        case ClipChannel::Lattice: {
            LatticeComponent* latticecomp = registry->findComponent<LatticeComponent>(entity);
            if (!latticecomp) return false;
            const int pointCount = lattice::pointCount(latticecomp->resolutionX, latticecomp->resolutionY, latticecomp->resolutionZ);
            const size_t count = points.empty() ? (size_t)pointCount : points.size();
            if (value.size() < 3 * count) return false;
            for (size_t n = 0; n < count; n++){
                int p = points.empty() ? (int)n : points[n];
                if (p < 0 || p >= pointCount) return false;
            }
            if (latticecomp->offsets.size() < (size_t)pointCount){
                latticecomp->offsets.resize((size_t)pointCount, Vector3(0, 0, 0));
            }
            bool changed = false;
            for (size_t n = 0; n < count; n++){
                int p = points.empty() ? (int)n : points[n];
                Vector3 v(value[3 * n], value[3 * n + 1], value[3 * n + 2]);
                if (latticecomp->offsets[(size_t)p] != v){
                    latticecomp->offsets[(size_t)p] = v;
                    changed = true;
                }
            }
            if (changed) latticecomp->revision++;
            return true;
        }
        case ClipChannel::Property:
        case ClipChannel::Humanoid:
        case ClipChannel::HumanoidPosition:
            // not entity channels: a humanoid track is realised on a rig (HumanoidClip.h)
            return false;
    }
    return false;
}
