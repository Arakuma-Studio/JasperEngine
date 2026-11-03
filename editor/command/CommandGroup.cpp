// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CommandGroup.h"

#include "PlayChangeReview.h"
#include "util/SafeWrite.h"

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
        // every step done again. One failing with a reason (its failure(), or a write the write
        // scope refused in it) takes back the ones done before it, in reverse, and the group
        // fails (the history then doesn't take it); one that just finds nothing to do is passed
        // over, as before
        failureText.clear();
        std::vector<Command*> doneNow;
        for (const std::unique_ptr<Command>& cmd : commands){
            const unsigned long long mark = safewrite::scopedFailureCount();
            if (cmd->execute()){
                doneNow.push_back(cmd.get());
                continue;
            }
            std::string why = cmd->failure();
            if (why.empty()) why = safewrite::scopedFailureSince(mark);
            if (why.empty()) continue;
            failureText = why;
            // the steps done before it taken back; a rollback that couldn't put everything back says so
            for (auto it = doneNow.rbegin(); it != doneNow.rend(); ++it){
                const unsigned long long rollbackMark = safewrite::scopedFailureCount();
                (*it)->undo();
                std::string problem = (*it)->takeUndoProblem();
                if (problem.empty()) problem = safewrite::scopedFailureSince(rollbackMark);
                if (!problem.empty()) failureText += "; and its rollback could not put everything back: " + problem;
            }
            return false;
        }
        return true;
    }

    applied = true;
    std::vector<std::unique_ptr<Command>> ran;
    std::vector<size_t> failed;
    for (size_t i = 0; i < commands.size(); i++){
        if (commands[i]->execute()){
            ran.push_back(std::move(commands[i]));
        }else{
            failures.push_back(commands[i]->typeName());
            failed.push_back(i);
        }
    }
    commands = std::move(ran);
    if (onFailures){
        onFailures(failed);
        onFailures = nullptr;
    }
    return !commands.empty();
}

void editor::CommandGroup::undo(){
    for (auto it = commands.rbegin(); it != commands.rend(); ++it){
        (*it)->undo();
        const std::string problem = (*it)->takeUndoProblem();
        if (!problem.empty()) undoProblem += (undoProblem.empty() ? "" : "; ") + problem;
    }
}

std::string editor::CommandGroup::undoRefusal() const{
    for (const std::unique_ptr<Command>& cmd : commands){
        std::string why = cmd->undoRefusal();
        if (!why.empty()) return why;
    }
    return std::string();
}

std::string editor::CommandGroup::redoRefusal() const{
    for (const std::unique_ptr<Command>& cmd : commands){
        std::string why = cmd->redoRefusal();
        if (!why.empty()) return why;
    }
    return std::string();
}

std::vector<editor::Command::Touched> editor::CommandGroup::touchedPaths(bool undo) const{
    std::vector<Touched> all;
    for (const std::unique_ptr<Command>& cmd : commands){
        std::vector<Touched> some = cmd->touchedPaths(undo);
        all.insert(all.end(), some.begin(), some.end());
    }
    return all;
}

bool editor::CommandGroup::canUndo() const{
    for (const std::unique_ptr<Command>& cmd : commands){
        if (!cmd->canUndo()) return false;
    }
    return true;
}

bool editor::CommandGroup::canRedo() const{
    for (const std::unique_ptr<Command>& cmd : commands){
        if (!cmd->canRedo()) return false;
    }
    return true;
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
        context.collect(*cmd, commandName(cmd.get()));
    }
}
