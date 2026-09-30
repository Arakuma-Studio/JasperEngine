// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Command.h"
#include <vector>
#include <cstddef>
#include <functional>

namespace jasperengine::editor{

    class CommandHistory{

    private:
        std::vector<Command*> list;
        size_t index = 0; // real index is (index-1)
        size_t sceneId = 0;

        // Play mode: the scene is restored from a snapshot on Stop, so commands made while
        // playing can't outlive the session, and edit-time commands can't be undone during it
        bool inPlaySession = false;
        size_t playFloor = 0;               // first command of the play session
        std::vector<Command*> editRedo;     // edit-time redo tail, set aside while playing

        size_t undoFloor() const { return inPlaySession ? playFloor : 0; }

    public:
        // Called after any command apply/undo/redo with the scene id and affectsStructure().
        static std::function<void(size_t, bool)> onSceneModified;

        explicit CommandHistory(size_t sceneId = 0);
        virtual ~CommandHistory();

        void addCommand(Command* cmd);
        void addCommandNoMerge(Command* cmd);

        void undo();
        void redo();

        bool canUndo() const;
        bool canRedo() const;

        void clear();

        void beginPlaySession();
        // rolledBack: the scene was restored to its pre-play state, so the play-time commands
        // are dropped and the edit redo tail comes back; otherwise the play-time commands stay
        void endPlaySession(bool rolledBack);
    };

}

