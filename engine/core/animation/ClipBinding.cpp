// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ClipBinding.h"

#include "animation/AnimationClip.h"
#include "lattice/LatticeSolver.h"
#include "registry/EntityRegistry.h"

using namespace jasperengine;

bool anim::parsePath(const std::string& path, std::vector<PathSegment>& out){
    // TODO(T1b)
    (void)path;
    out.clear();
    return false;
}

std::string anim::formatPath(const std::vector<PathSegment>& segments){
    // TODO(T1b)
    (void)segments;
    return std::string();
}

std::string anim::formatSegment(const std::string& name, int index){
    // TODO(T1b)
    (void)index;
    return name;
}

bool anim::isAddressableName(const std::string& name){
    // TODO(T1b)
    (void)name;
    return false;
}

std::vector<Entity> anim::getChildren(EntityRegistry* registry, Entity parent){
    // TODO(T1b)
    (void)registry;
    (void)parent;
    return {};
}

Entity anim::resolvePath(EntityRegistry* registry, Entity root, const std::string& path, PathResolution* info){
    // TODO(T1b)
    (void)registry;
    (void)root;
    (void)path;
    if (info) *info = PathResolution();
    return NULL_ENTITY;
}

Entity anim::resolvePath(EntityRegistry* registry, Entity root, const std::vector<PathSegment>& segments, PathResolution* info){
    // TODO(T1b)
    (void)registry;
    (void)root;
    (void)segments;
    if (info) *info = PathResolution();
    return NULL_ENTITY;
}

bool anim::relativePath(EntityRegistry* registry, Entity root, Entity entity, std::string& out){
    // TODO(T1b)
    (void)registry;
    (void)root;
    (void)entity;
    out.clear();
    return false;
}

bool anim::hasDuplicateSiblingName(EntityRegistry* registry, Entity entity){
    // TODO(T1b)
    (void)registry;
    (void)entity;
    return false;
}

std::vector<std::pair<std::string, Entity>> anim::descendantPaths(EntityRegistry* registry, Entity root){
    // TODO(T1b)
    (void)registry;
    (void)root;
    return {};
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
            return false;
    }
    return false;
}
