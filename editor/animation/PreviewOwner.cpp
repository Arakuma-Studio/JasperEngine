// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PreviewOwner.h"

#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;

// TODO(T3a): the owner registry. The stubs grant every request (a new valid token) and stop
// nobody.

namespace{
    PreviewOwner::Token nextToken = 1;
    std::set<PreviewOwner::Token> liveTokens;
}

PreviewOwner::Token PreviewOwner::acquire(uint32_t sceneId, Entity root, EntityRegistry* registry, const std::string& name, Stop stop){
    (void)sceneId;
    (void)root;
    (void)registry;
    (void)name;
    (void)stop;
    Token token = nextToken++;
    liveTokens.insert(token);
    return token;
}

void PreviewOwner::release(Token token){
    liveTokens.erase(token);
}

bool PreviewOwner::owns(Token token){
    return token != 0 && liveTokens.count(token) > 0;
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
