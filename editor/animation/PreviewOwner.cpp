// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PreviewOwner.h"

#include "component/Transform.h"
#include "registry/EntityRegistry.h"

#include <algorithm>
#include "profiler/GpuMemory.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace{

    struct Entry{
        PreviewOwner::Owner owner;
        PreviewOwner::Stop stop;
        const void* who = nullptr;
        // the registry the owner acquired with (stopFor without one); valid while the scene is
        // open: a closed scene's owners are released (releaseScene)
        EntityRegistry* registry = nullptr;
        PreviewOwner::Writes writes;
    };

    struct Registry{
        PreviewOwner::Token nextToken = 1;
        std::vector<Entry> entries;
    };

    // never destroyed: owners living until static exit release into it
    Registry& ownerRegistry(){
        static Registry* instance = new Registry();
        return *instance;
    }

    // `ancestor` is `entity` or one of its Transform parents
    bool isAncestorOrSelf(EntityRegistry* registry, Entity ancestor, Entity entity){
        if (ancestor == entity) return true;
        if (!registry || ancestor == NULL_ENTITY || entity == NULL_ENTITY || !registry->isEntityCreated(entity)) return false;
        Entity current = entity;
        // a hierarchy is never deeper than the entity count; the bound only guards a broken cycle
        for (int depth = 0; depth < 100000; depth++){
            const Transform* transform = registry->findComponent<Transform>(current);
            if (!transform || transform->parent == NULL_ENTITY) return false;
            current = transform->parent;
            if (current == ancestor) return true;
            if (!registry->isEntityCreated(current)) return false;
        }
        return false;
    }
}

bool PreviewOwner::overlaps(EntityRegistry* registry, Entity a, Entity b){
    return isAncestorOrSelf(registry, a, b) || isAncestorOrSelf(registry, b, a);
}

PreviewOwner::Token PreviewOwner::acquire(uint32_t sceneId, Entity root, EntityRegistry* registry, const std::string& name, Stop stop){
    Registry& reg = ownerRegistry();
    // the owners in the way leave the registry first, then stop (in the order they came): a
    // stop that releases its own token finds it gone, one that releases others finds them
    std::vector<Entry> stopped;
    for (auto it = reg.entries.begin(); it != reg.entries.end();){
        if (it->owner.sceneId == sceneId && overlaps(registry, it->owner.root, root)){
            stopped.push_back(std::move(*it));
            it = reg.entries.erase(it);
        }else{
            ++it;
        }
    }
    for (Entry& entry : stopped){
        if (entry.stop) entry.stop();
    }

    Entry entry;
    entry.owner.token = reg.nextToken++;
    entry.owner.sceneId = sceneId;
    entry.owner.root = root;
    entry.owner.name = name;
    entry.stop = std::move(stop);
    entry.registry = registry;
    reg.entries.push_back(std::move(entry));
    return reg.entries.back().owner.token;
}

void PreviewOwner::release(Token token){
    if (token == 0) return;
    std::vector<Entry>& entries = ownerRegistry().entries;
    entries.erase(std::remove_if(entries.begin(), entries.end(), [token](const Entry& entry){ return entry.owner.token == token; }),
                  entries.end());
}

bool PreviewOwner::owns(Token token){
    if (token == 0) return false;
    const std::vector<Entry>& entries = ownerRegistry().entries;
    return std::any_of(entries.begin(), entries.end(), [token](const Entry& entry){ return entry.owner.token == token; });
}

PreviewOwner::Owner PreviewOwner::ownerOf(uint32_t sceneId, Entity root){
    for (const Entry& entry : ownerRegistry().entries){
        if (entry.owner.sceneId == sceneId && entry.owner.root == root) return entry.owner;
    }
    return Owner();
}

std::vector<PreviewOwner::Owner> PreviewOwner::owners(){
    std::vector<Owner> out;
    for (const Entry& entry : ownerRegistry().entries) out.push_back(entry.owner);
    return out;
}

void PreviewOwner::releaseScene(uint32_t sceneId){
    std::vector<Entry>& entries = ownerRegistry().entries;
    entries.erase(std::remove_if(entries.begin(), entries.end(), [sceneId](const Entry& entry){ return entry.owner.sceneId == sceneId; }),
                  entries.end());
}

PreviewOwner::Token PreviewOwner::acquireFor(const void* who, uint32_t sceneId, Entity root, EntityRegistry* registry,
                                             const std::string& name, Stop stop){
    if (!who) return acquire(sceneId, root, registry, name, std::move(stop));
    for (Entry& entry : ownerRegistry().entries){
        if (entry.who != who) continue;
        if (entry.owner.sceneId == sceneId && entry.owner.root == root){
            // still the owner of that root: nobody overlapping acquired it since
            entry.stop = std::move(stop);
            return entry.owner.token;
        }
        release(entry.owner.token);
        break;
    }
    const Token token = acquire(sceneId, root, registry, name, std::move(stop));
    for (Entry& entry : ownerRegistry().entries){
        if (entry.owner.token == token) entry.who = who;
    }
    return token;
}

void PreviewOwner::releaseFor(const void* who){
    if (!who) return;
    std::vector<Entry>& entries = ownerRegistry().entries;
    entries.erase(std::remove_if(entries.begin(), entries.end(), [who](const Entry& entry){ return entry.who == who; }), entries.end());
}

PreviewOwner::Token PreviewOwner::tokenOf(const void* who){
    if (!who) return 0;
    for (const Entry& entry : ownerRegistry().entries){
        if (entry.who == who) return entry.owner.token;
    }
    return 0;
}

void PreviewOwner::stopFor(uint32_t sceneId, const std::vector<Entity>& roots, EntityRegistry* registry, const void* except){
    Registry& reg = ownerRegistry();
    std::vector<Entry> stopped;
    for (auto it = reg.entries.begin(); it != reg.entries.end();){
        bool hit = (sceneId == 0 || it->owner.sceneId == sceneId) && !(except && it->who == except);
        if (hit && !roots.empty()){
            EntityRegistry* hierarchy = registry ? registry : it->registry;
            hit = std::any_of(roots.begin(), roots.end(), [&](Entity root){ return overlaps(hierarchy, it->owner.root, root); });
        }
        if (hit){
            stopped.push_back(std::move(*it));
            it = reg.entries.erase(it);
        }else{
            ++it;
        }
    }
    for (Entry& entry : stopped){
        if (entry.stop) entry.stop();
    }
}

void PreviewOwner::setWrites(Token token, Writes writes){
    for (Entry& entry : ownerRegistry().entries){
        if (entry.owner.token == token) entry.writes = std::move(writes);
    }
}

bool PreviewOwner::writes(uint32_t sceneId, Entity entity){
    if (entity == NULL_ENTITY) return false;
    // (the predicates of a copy: one never acquires, but a release while asking stays safe)
    std::vector<std::pair<Entity, Writes>> asked;
    for (const Entry& entry : ownerRegistry().entries){
        if (entry.owner.sceneId == sceneId) asked.emplace_back(entry.owner.root, entry.writes);
    }
    for (const auto& owner : asked){
        if (owner.second ? owner.second(entity) : owner.first == entity) return true;
    }
    return false;
}
