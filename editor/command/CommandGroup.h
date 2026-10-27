// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Command.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor{

    // Commands undone and redone as one step: an MCP batch, a folded interactive edit or
    // agent call, Apply Play Mode Changes. Named by its label when it has one, else by its
    // first command ("Move Player (+2 more)").
    class CommandGroup : public Command{
    private:
        std::vector<std::unique_ptr<Command>> commands;
        std::string label;
        bool applied;
        std::vector<std::string> failures;
        std::function<void(const std::vector<size_t>&)> onFailures;

        static std::string commandName(const Command* cmd);

    public:
        // applied: the commands already ran (grouping history steps), so execute() redoes
        // them; otherwise the first execute runs each, dropping and reporting the ones that
        // fail, and succeeds when any ran
        explicit CommandGroup(std::vector<std::unique_ptr<Command>> commands, std::string label = {}, bool applied = true);

        bool execute() override;
        void undo() override;

        // false when any command refuses (Command::canUndo/canRedo)
        bool canUndo() const override;
        bool canRedo() const override;

        bool affectsStructure() const override;

        std::string describe() const override;
        const char* typeName() const override { return "CommandGroup"; }
        bool isNoOp() const override;
        size_t memoryBytes() const override;
        bool releaseOffThread() const override;
        void collectKeepCommands(KeepContext& context) const override;

        void setLabel(std::string label) { this->label = std::move(label); }
        const std::string& getLabel() const { return label; }
        const std::vector<std::unique_ptr<Command>>& getCommands() const { return commands; }
        // The names of the commands the first execute dropped because they failed
        const std::vector<std::string>& getFailures() const { return failures; }
        // Called by the first execute with the indices of the commands that failed
        void setOnFailures(std::function<void(const std::vector<size_t>&)> callback) { onFailures = std::move(callback); }
    };

}
