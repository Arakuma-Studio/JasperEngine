// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ANIMLUA_H
#define ANIMLUA_H

// The Lua side of the anime API (engine/core/script/binding/HtmlUIAnimeLua.cpp): what tests
// and tools read about it. Lua callbacks, function values and table targets are registry
// refs released by AnimEngine::reset() (play stop, Engine::systemShutdown).

#include "Export.h"

#include <cstddef>

namespace jasperengine {

    class JASPERENGINE_API AnimLua {
    public:
        // registry refs anime holds (callbacks, function values, easing/modifier functions, table targets)
        static size_t getLiveLuaRefs();
        // After cancelling/detaching private preview roots, release their registry refs
        // before dropping the module cache and collecting Lua. Unrelated refs stay live.
        static void releasePreviewRefs(int contextKey);
    };

}

#endif //ANIMLUA_H
