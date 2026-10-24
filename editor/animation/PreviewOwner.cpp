// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PreviewOwner.h"

using namespace jasperengine;
using namespace jasperengine::editor;

// TODO(T3a): the owner registry. The stubs grant every request and stop nobody.

PreviewOwner::Token PreviewOwner::acquire(uint32_t sceneId, Entity root, EntityRegistry* registry, const std::string& name, Stop stop){
    (void)sceneId;
    (void)root;
    (void)registry;
    (void)name;
    (void)stop;
    return 0;
}

void PreviewOwner::release(Token token){
    (void)token;
}

bool PreviewOwner::owns(Token token){
    (void)token;
    return false;
}

PreviewOwner::Owner PreviewOwner::ownerOf(uint32_t sceneId, Entity root){
    (void)sceneId;
    (void)root;
    return Owner();
}

std::vector<PreviewOwner::Owner> PreviewOwner::owners(){
    return {};
}

void PreviewOwner::releaseScene(uint32_t sceneId){
    (void)sceneId;
}
