// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LUA_H
#define LUA_H

#include "GameObject.h"

namespace jasperengine {
    class LuaScript {
    public:
        static void setGameObject(const std::string& global, GameObject* object);
        static GameObject* getGameObject(const std::string& global);
    };

}


#endif //ANDROIDSTUDIO_LUA_H
