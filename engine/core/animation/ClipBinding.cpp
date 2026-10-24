// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ClipBinding.h"

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
