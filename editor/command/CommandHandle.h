// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CommandHistory.h"
#include <map>

namespace jasperengine::editor{

    class CommandHandle{

    private:
        static std::map<size_t, CommandHistory*> historys;
        static bool redirecting;
        static size_t redirectFrom;
        static size_t redirectTo;

    public:
        // The scene's history, made on first use
        static CommandHistory* get(size_t sceneId);
        // The scene's history, null when it has none yet
        static CommandHistory* find(size_t sceneId);

        // Frees the scene's history and its steps (R23: links to them go too)
        static void remove(size_t sceneId);

        // While one lives, a command pushed on `from` goes to the history of `to`. The Inspector's
        // rows push on the selected scene's history whichever scene they edit: the rows of a
        // .material, on a mesh of a scene of its own, must stay out of the open scene's undo.
        class Redirect{
        public:
            Redirect(size_t from, size_t to);
            ~Redirect();
            Redirect(const Redirect&) = delete;
            Redirect& operator=(const Redirect&) = delete;
        };
    };

}
