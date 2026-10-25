// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef THREADPOOLMANAGERLUA_H
#define THREADPOOLMANAGERLUA_H

#include "Export.h"

#include <cstddef>

namespace jasperengine {

    // The Lua ThreadPoolManager, kept one release for scripts written for the old pool:
    // getQueueSize() reports the job system's Background lane; initialize and shutdown only
    // warn (once) and do nothing, since the pool is the engine's.
    class JASPERENGINE_API ThreadPoolManagerLua {
    public:
        static ThreadPoolManagerLua* getInstance();
        static void initialize(size_t maxThreads);
        static void shutdown();
        size_t getQueueSize() const;
    };

}

#endif // THREADPOOLMANAGERLUA_H
