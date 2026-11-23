// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SaveSlotCmd.h"

#include "Out.h"
#include "io/AtomicFile.h"

#include <algorithm>
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

editor::SaveSlotCmd::SaveSlotCmd(Action action, std::string slot, uint64_t generation, savegame::CorruptMode mode)
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
    savegame::SaveGameStore& store = savegame::SaveGameStore::instance();
    slots.clear();
    for (const std::string& name : names){
        // what is queued for the slot lands first: the bytes kept are what the game wrote. A
        // write still on its way past the limit would land after the step: refused, nothing changed
        const savegame::SaveError flushed = store.flush(name);
        if (!flushed.ok() && store.pendingGeneration(name) != 0){
            error = "Couldn't change save slot \"" + name + "\": the game is still writing it. Try again.";
            return false;
        }
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

bool editor::SaveSlotCmd::changedFiles(const std::vector<std::string>& names) const{
    savegame::SaveGameStore& store = savegame::SaveGameStore::instance();
    for (const std::string& name : names){
        auto it = slots.find(name);
        if (it == slots.end()) continue;
        if (store.pendingGeneration(name) != 0) return true;
        std::vector<std::string> now;
        for (const fs::path& path : store.slotFiles(name)) now.push_back(path.filename().string());
        if (now.size() != it->second.files.size()) return true;
        for (const std::string& file : now)
            if (!it->second.files.count(file)) return true;
    }
    return false;
}

bool editor::SaveSlotCmd::execute(){
    savegame::SaveGameStore& store = savegame::SaveGameStore::instance();
    error.clear();
    partial.clear();
    std::vector<std::string> names;
    if (action == Action::ClearAll){
        for (const savegame::SlotInfo& info : store.list()) names.push_back(info.slot);
    } else {
        names.push_back(slot);
    }
    if (!snapshot(names)){
        Out::error(error);
        return false;
    }

    savegame::SaveError result;
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
    // what undo checks: the newest generation on disk, or the one a write still on its way publishes
    for (const std::string& name : names)
        slots[name].newestAfter = std::max(store.newestGeneration(name), store.pendingGeneration(name));
    if (!result.ok()){
        error = result.message;
        Out::error(result.message);
        if (changedFiles(names)){
            partial = result.message;
            return true;
        }
        return false;
    }
    return true;
}

std::string editor::SaveSlotCmd::undoRefusal() const{
    savegame::SaveGameStore& store = savegame::SaveGameStore::instance();
    for (const auto& entry : slots){
        const uint64_t newest = std::max(store.newestGeneration(entry.first), store.pendingGeneration(entry.first));
        if (newest != entry.second.newestAfter)
            return "Can't undo \"" + label() + "\": the game saved that slot since.";
    }
    return Command::undoRefusal();
}

void editor::SaveSlotCmd::undo(){
    savegame::SaveGameStore& store = savegame::SaveGameStore::instance();
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
