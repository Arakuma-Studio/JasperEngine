// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "PropertyCmd.h"
#include <vector>
#include <memory>
#include <utility>

namespace jasperengine::editor {

    class MultiPropertyCmd : public Command {
    private:
        std::vector<std::unique_ptr<Command>> commands;

    public:
        template<typename T>
        void addPropertyCmd(Project* project, uint32_t sceneId, Entity entity, ComponentType type, 
                           std::string propertyName, T newValue, std::function<void()> onValueChanged = nullptr) {
            commands.push_back(std::make_unique<PropertyCmd<T>>(
                project, sceneId, entity, type, propertyName, newValue, onValueChanged));
        }

        void addCommand(std::unique_ptr<Command> command) {
            commands.push_back(std::move(command));
        }

        bool execute() override {
            for (size_t i = 0; i < commands.size(); ++i) {
                if (!commands[i]->execute()){
                    while (i > 0) {
                        commands[--i]->undo();
                    }
                    return false;
                }
            }
            return true;
        }

        void undo() override {
            for (auto it = commands.rbegin(); it != commands.rend(); ++it) {
                (*it)->undo();
            }
        }

        // Structural if any child is: a batch may mix an AddComponentCmd with properties.
        bool affectsStructure() const override {
            for (const auto& cmd : commands) {
                if (cmd->affectsStructure()) return true;
            }
            return false;
        }

        bool mergeWith(Command* otherCommand) override {
            MultiPropertyCmd* otherMultiCmd = dynamic_cast<MultiPropertyCmd*>(otherCommand);
            if (otherMultiCmd) {
                bool anyMerged = false;
                std::vector<size_t> unmatched;
                
                for (size_t i = 0; i < otherMultiCmd->commands.size(); i++) {
                    bool merged = false;
                    
                    for (size_t j = 0; j < commands.size(); j++) {
                        if (commands[j]->mergeWith(otherMultiCmd->commands[i].get())) {
                            merged = true;
                            anyMerged = true;
                            break;
                        }
                    }

                    if (!merged) {
                        unmatched.push_back(i);
                    }

                    // A rejected merge must leave the older batch intact for its own undo.
                    if (anyMerged) {
                        for (size_t index : unmatched) {
                            commands.push_back(std::move(otherMultiCmd->commands[index]));
                        }
                        unmatched.clear();
                    }
                }
                
                return anyMerged;
            }
            
            return false;
        }

    };

}
