// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Dialogue Runner of a GameObject (docs/specs/dialogue-graphs.md R13, R27): Lua
// `DialogueRunner(scene, entity)` and C++ scripts. It wraps the GameObject's component and adds one
// when it has none; the runner itself (dialogue::Runner) is made on first use, loads the
// component's dialogue and is seeded from its Seed (0: the entity id). Main thread only (ECS).

#ifndef DIALOGUE_RUNNER_OBJECT_H
#define DIALOGUE_RUNNER_OBJECT_H

#include "object/EntityHandle.h"
#include "component/DialogueRunnerComponent.h"
#include "dialogue/DialogueRunner.h"
#include "localization/LocalizationArgs.h"

#include <string>
#include <vector>

namespace jasperengine{

    class JASPERENGINE_API DialogueRunner: public EntityHandle{
    public:
        DialogueRunner(Scene* scene, Entity entity);
        virtual ~DialogueRunner();

        DialogueRunner(const DialogueRunner& rhs);
        DialogueRunner& operator=(const DialogueRunner& rhs);
        DialogueRunner(DialogueRunner&& rhs) noexcept;
        DialogueRunner& operator=(DialogueRunner&& rhs) noexcept;

        // `entry` "" = the component's Start Entry, else `start`
        bool start(const std::string& entry = std::string(), const LocalizationArgs& args = {});
        bool advance();
        bool choose(int index);
        bool resume();
        void stop();
        void setArg(const std::string& name, const LocalizationValue& value);

        // the .dialogue played; a new one stops a running conversation
        void setDialogue(const std::string& path);
        std::string getDialogue() const;
        void setStartEntry(const std::string& entry);
        std::string getStartEntry() const;
        void setAutoStart(bool autoStart);
        bool isAutoStart() const;
        void setSeed(int seed);
        int getSeed() const;

        std::string getState() const;
        bool isRunning() const;
        std::string getCurrentNode() const;
        // the last line shown; null before any
        const dialogue::Line* getCurrentLine() const;

        FunctionSubscribe<void(DialogueRunner)>& getOnStart();
        FunctionSubscribe<void(DialogueRunner, const dialogue::Line&)>& getOnLine();
        FunctionSubscribe<void(DialogueRunner, const std::string&)>& getOnEnd();
        FunctionSubscribe<void(DialogueRunner, const std::string&, const dialogue::Value&)>& getOnVariableChanged();

        // the component's runner, made and loaded on first use (null when the dialogue does not load)
        dialogue::Runner* getRunner() const;
    };

    namespace dialogue {
        // GameObjects whose scripts were just awoken (a scene's start, an instantiate; null = the
        // whole scene): their Auto Start runners start at the end of this frame's update
        JASPERENGINE_API void scheduleAutoStart(Scene* scene, const std::vector<Entity>* entities);
        // at Stop: handlers, pending starts, the program cache (Engine::clearAllSubscriptions)
        JASPERENGINE_API void clearRuntime();
        // the runner of a component, made on first use (and its mirrors kept in step)
        JASPERENGINE_API Runner* runnerOf(Scene* scene, Entity entity);
    }

}

#endif //DIALOGUE_RUNNER_OBJECT_H
