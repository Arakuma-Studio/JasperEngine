// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SaveSlotCmd.h"

#include "Out.h"
#include "io/AtomicFile.h"

#include <fstream>

using namespace jasperengine;
namespace fs = std::filesystem;

namespace {

    bool readSlotFile(const fs::path& path, std::string& out){
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return true;
    }

}

editor::SaveSlotCmd::SaveSlotCmd(Action action, std::string slot, uint64_t generation, save::CorruptMode mode)
    : action(action), slot(std::move(slot)), generation(generation), mode(mode){
}

std::string editor::SaveSlotCmd::label() const{
    switch (action){
        case Action::Delete: return "Delete Save Slot '" + slot + "'";
        case Action::Restore: return "Restore Save Slot '" + slot + "' Generation " + std::to_string(generation);
        case Action::Corrupt: return "Corrupt Save Slot '" + slot + "'";
        case Action::ClearAll: return "Clear All Saves";
    }
    return "Change Save Slot";
}

bool editor::SaveSlotCmd::snapshot(const std::vector<std::string>& names){
    save::SaveGameStore& store = save::SaveGameStore::instance();
    slots.clear();
    for (const std::string& name : names){
        // what is queued for the slot lands first: the bytes kept are what the game wrote
        store.flush(name);
        SlotState& state = slots[name];
        for (const fs::path& path : store.slotFiles(name)){
            std::string bytes;
            if (!readSlotFile(path, bytes)){
                error = "Couldn't read " + path.filename().string() + " to keep it for undo";
                return false;
            }
            state.files[path.filename().string()] = std::move(bytes);
        }
    }
    return true;
}

bool editor::SaveSlotCmd::execute(){
    save::SaveGameStore& store = save::SaveGameStore::instance();
    error.clear();
    std::vector<std::string> names;
    if (action == Action::ClearAll){
        for (const save::SlotInfo& info : store.list()) names.push_back(info.slot);
    } else {
        names.push_back(slot);
    }
    if (!snapshot(names)) return false;

    save::SaveError result;
    if (action == Action::Delete || action == Action::ClearAll){
        for (const std::string& name : names){
            result = store.remove(name);
            if (!result.ok()) break;
        }
    } else if (action == Action::Restore){
        result = store.restore(slot, generation, nullptr);
        if (result.ok()) result = store.flush(slot);
    } else {
        std::string file;
        result = store.corrupt(slot, mode, file);
    }
    for (const std::string& name : names) slots[name].newestAfter = store.newestGeneration(name);
    if (!result.ok()){
        error = result.message;
        Out::error(result.message);
        return false;
    }
    return true;
}

std::string editor::SaveSlotCmd::undoRefusal() const{
    save::SaveGameStore& store = save::SaveGameStore::instance();
    for (const auto& entry : slots){
        if (store.newestGeneration(entry.first) != entry.second.newestAfter)
            return "Can't undo \"" + label() + "\": the game saved that slot since.";
    }
    return Command::undoRefusal();
}

void editor::SaveSlotCmd::undo(){
    save::SaveGameStore& store = save::SaveGameStore::instance();
    const fs::path folder = store.folder();
    for (const auto& entry : slots){
        store.flush(entry.first);
        std::error_code ec;
        for (const fs::path& path : store.slotFiles(entry.first)){
            if (!entry.second.files.count(path.filename().string())) fs::remove(path, ec);
        }
        for (const auto& file : entry.second.files){
            const io::WriteResult written = io::writeFileAtomic(folder / file.first, file.second);
            if (!written.written() && written.error != io::WriteError::Unchanged){
                undoProblem = "Couldn't put back " + file.first + ": " + io::describeWriteError(written);
                Out::error(undoProblem);
            }
        }
        // the image in memory is of the files undo replaced
        store.unload(entry.first);
    }
}

size_t editor::SaveSlotCmd::memoryBytes() const{
    size_t total = sizeof(*this);
    for (const auto& entry : slots)
        for (const auto& file : entry.second.files) total += file.first.size() + file.second.size();
    return total;
}
