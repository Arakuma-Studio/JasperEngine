// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CommandGroup.h"

using namespace jasperengine;

editor::CommandGroup::CommandGroup(std::vector<std::unique_ptr<Command>> commands, std::string label, bool applied):
    commands(std::move(commands)), label(std::move(label)), applied(applied){
    setNoMerge();
}

std::string editor::CommandGroup::commandName(const Command* cmd){
    // a step of a history has its captured name; a fresh command only its description
    return cmd->name().empty() ? cmd->describe() : cmd->name();
}

bool editor::CommandGroup::execute(){
    if (applied){
        for (const std::unique_ptr<Command>& cmd : commands){
            cmd->execute();
        }
        return true;
    }

    applied = true;
    std::vector<std::unique_ptr<Command>> ran;
    for (std::unique_ptr<Command>& cmd : commands){
        if (cmd->execute()){
            ran.push_back(std::move(cmd));
        }else{
            failures.push_back(commandName(cmd.get()));
        }
    }
    commands = std::move(ran);
    return !commands.empty();
}

void editor::CommandGroup::undo(){
    for (auto it = commands.rbegin(); it != commands.rend(); ++it){
        (*it)->undo();
    }
}

bool editor::CommandGroup::affectsStructure() const{
    for (const std::unique_ptr<Command>& cmd : commands){
        if (cmd->affectsStructure()) return true;
    }
    return false;
}

std::string editor::CommandGroup::describe() const{
    if (!label.empty()){
        return label;
    }
    if (commands.empty()){
        return "Edit";
    }
    std::string name = commandName(commands.front().get());
    if (commands.size() > 1){
        name += " (+" + std::to_string(commands.size() - 1) + " more)";
    }
    return name;
}

bool editor::CommandGroup::isNoOp() const{
    for (const std::unique_ptr<Command>& cmd : commands){
        if (!cmd->isNoOp()) return false;
    }
    return true;
}

size_t editor::CommandGroup::memoryBytes() const{
    size_t bytes = sizeof(*this) + commands.capacity() * sizeof(std::unique_ptr<Command>);
    for (const std::unique_ptr<Command>& cmd : commands){
        bytes += cmd->memoryBytes();
    }
    return bytes;
}

bool editor::CommandGroup::releaseOffThread() const{
    for (const std::unique_ptr<Command>& cmd : commands){
        if (!cmd->releaseOffThread()) return false;
    }
    return true;
}

void editor::CommandGroup::collectKeepCommands(KeepContext& context) const{
    for (const std::unique_ptr<Command>& cmd : commands){
        cmd->collectKeepCommands(context);
    }
}
