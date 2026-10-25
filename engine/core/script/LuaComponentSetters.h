// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LUACOMPONENTSETTERS_H
#define LUACOMPONENTSETTERS_H

#include "EntityRegistry.h"

namespace jasperengine {

    namespace lua_detail {
        template<typename M> struct MemberOf;
        template<typename C, typename V> struct MemberOf<V C::*> {
            using Class = C;
            using Value = V;
        };
    }

    // The Lua setter of a component field: writes it and stamps the component changed
    // (Changed<T>), as the object wrappers' setters do.
    //   .addProperty("width", &RectTransformComponent::width, &setTrackedMember<&RectTransformComponent::width>)
    template<auto Member>
    void setTrackedMember(typename lua_detail::MemberOf<decltype(Member)>::Class* self,
                          const typename lua_detail::MemberOf<decltype(Member)>::Value& value){
        self->*Member = value;
        EntityRegistry::markChangedComponent(self);
    }

}

#endif // LUACOMPONENTSETTERS_H
