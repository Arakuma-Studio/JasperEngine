// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ECS_ENTITYREF_H
#define ECS_ENTITYREF_H

#include <cstdint>
#include "Entity.h"

namespace jasperengine {

    // An entity as deferred and asynchronous code holds it: the id and the registry's
    // creation serial of the entity at that time (EntityRegistry::makeRef). isAlive(ref) is
    // false once that entity is gone, even when the id was given to a new entity since
    // (system ids, the editor's recreateEntity). Scenes, Lua, prefabs, the editor and MCP keep
    // using the raw Entity.
    struct EntityRef {
        Entity id = NULL_ENTITY;
        uint32_t serial = 0;

        bool isNull() const { return id == NULL_ENTITY; }

        friend bool operator==(const EntityRef& a, const EntityRef& b){ return a.id == b.id && a.serial == b.serial; }
        friend bool operator!=(const EntityRef& a, const EntityRef& b){ return !(a == b); }
    };

}

#endif // ECS_ENTITYREF_H
