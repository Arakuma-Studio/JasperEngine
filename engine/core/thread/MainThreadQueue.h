// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MAINTHREADQUEUE_H
#define MAINTHREADQUEUE_H

#include "Export.h"

#include <cstddef>
#include <cstdint>
#include <functional>

namespace jasperengine {

    // The one way work done on another thread (an HTTP transfer, a native SDK callback) reaches
    // the game: a task posted here runs on the main thread, in Engine::runFrame, right before
    // Engine::onUpdate (inside the update block: not while the app or the editor's Play is
    // paused, or the MCP clock holds the simulation; time scale 0 still runs it). Tasks never
    // touch Lua off the main thread because they only run there.
    //
    // Shared by the http and native-bridge features (and anything else that needs it):
    //   MainThreadQueue::post([result = std::move(result)]() { deliver(result); });
    class JASPERENGINE_API MainThreadQueue {
    public:
        // Any thread; never blocks on Lua. Tasks run in the order they were posted.
        static void post(std::function<void()> task);

        // Engine::runFrame only (the main thread): runs the tasks posted before this call, in
        // order. A task posting another task runs that one in the next drain (next frame). A
        // task that throws is logged and the others still run.
        static void drain();

        // Tasks waiting for the next drain
        static size_t pending();

        // Play stop / engine shutdown: drops the queued tasks without running them
        static void clear();

        // Incremented by every clear(): a poster that keeps "a task of mine is queued" state
        // compares it to know the task it remembers was dropped
        static uint64_t epoch();

        // The game's main thread: the thread that made the engine (Engine::systemInit), until
        // a drain runs on another one (the editor's main thread)
        static bool isMainThread();
        static void setMainThread();
    };

}

#endif // MAINTHREADQUEUE_H
