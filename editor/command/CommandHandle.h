// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CommandHistory.h"
#include <map>

namespace jasperengine::editor{

    class CommandHandle{

    private:
        static std::map<size_t, CommandHistory*> historys;

    public:
        // The scene's history, made on first use
        static CommandHistory* get(size_t sceneId);
        // The scene's history, null when it has none yet
        static CommandHistory* find(size_t sceneId);

        // Frees the scene's history and its steps (R23: links to them go too)
        static void remove(size_t sceneId);
    };

}
