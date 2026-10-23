// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandNames.h"
#include "command/EntityEdit.h"
#include "PropertyCmd.h"
#include <algorithm>
#include <vector>
#include <memory>
#include <utility>

namespace jasperengine::editor {

    class MultiPropertyCmd : public Command, public EntityEdit {
    private:
        std::vector<std::unique_ptr<Command>> commands;

        const EntityEdit* firstEdit() const {
            for (const auto& cmd : commands) {
                if (const EntityEdit* edit = dynamic_cast<const EntityEdit*>(cmd.get())) return edit;
            }
            return nullptr;
        }

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

        bool empty() const { return commands.empty(); }
        const std::vector<std::unique_ptr<Command>>& getCommands() const { return commands; }

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

        // "Set Intensity (Sun)" for one property (on any number of GameObjects), "Set
        // Properties (3 Objects)" for several
        std::string describe() const override {
            if (commands.size() == 1) {
                return commands.front()->describe();
            }
            if (commands.empty()) {
                return "Set Properties";
            }
            std::string label;
            bool sameLabel = true;
            for (const auto& cmd : commands) {
                const EntityEdit* edit = dynamic_cast<const EntityEdit*>(cmd.get());
                const std::string childLabel = edit ? edit->editedLabel() : std::string();
                if (label.empty()) {
                    label = childLabel;
                }
                if (childLabel.empty() || childLabel != label) {
                    sameLabel = false;
                }
            }
            const std::vector<Entity> entities = editedEntities();
            if (!firstEdit()) {
                // not GameObject edits (an MCP asset move): named by the first
                return commands.front()->describe() + " (+" + std::to_string(commands.size() - 1) + " more)";
            }
            const std::string verb = sameLabel ? "Set " + label : std::string("Set Properties");
            if (entities.empty()) {
                return verb;
            }
            const EntityEdit* edit = firstEdit();
            return verb + " (" + CommandNames::entities(edit->editedProject(), edit->editedSceneId(), entities) + ")";
        }

        const char* typeName() const override { return "MultiPropertyCmd"; }

        bool isNoOp() const override {
            for (const auto& cmd : commands) {
                if (!cmd->isNoOp()) return false;
            }
            return true;
        }

        size_t memoryBytes() const override {
            size_t bytes = sizeof(*this) + commands.capacity() * sizeof(std::unique_ptr<Command>);
            for (const auto& cmd : commands) {
                bytes += cmd->memoryBytes();
            }
            return bytes;
        }

        // Every GameObject a child edits, in first-edit order
        std::vector<Entity> editedEntities() const override {
            std::vector<Entity> entities;
            for (const auto& cmd : commands) {
                const EntityEdit* edit = dynamic_cast<const EntityEdit*>(cmd.get());
                if (!edit) continue;
                for (Entity entity : edit->editedEntities()) {
                    if (std::find(entities.begin(), entities.end(), entity) == entities.end()) {
                        entities.push_back(entity);
                    }
                }
            }
            return entities;
        }

        std::string editedLabel() const override {
            const EntityEdit* edit = firstEdit();
            return commands.size() == 1 && edit ? edit->editedLabel() : std::string("Properties");
        }

        Project* editedProject() const override {
            const EntityEdit* edit = firstEdit();
            return edit ? edit->editedProject() : nullptr;
        }

        uint32_t editedSceneId() const override {
            const EntityEdit* edit = firstEdit();
            return edit ? edit->editedSceneId() : 0;
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
