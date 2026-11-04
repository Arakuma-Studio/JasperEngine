// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GraphEditCmd.h"

#include "Out.h"
#include "io/AtomicFile.h"

#include <utility>
#include <vector>

using namespace jasperengine;

namespace {

    std::vector<std::pair<uint64_t, editor::GraphEditCmd::ChangeListener>>& graphChangeListeners() {
        static std::vector<std::pair<uint64_t, editor::GraphEditCmd::ChangeListener>> listeners;
        return listeners;
    }

    void notifyGraphFileChanged(const std::filesystem::path& file) {
        // a copy: a listener may remove itself
        const auto listeners = graphChangeListeners();
        for (const auto& [id, listener] : listeners) listener(file);
    }

}

uint64_t editor::GraphEditCmd::addChangeListener(ChangeListener listener) {
    static uint64_t nextId = 1;
    graphChangeListeners().emplace_back(nextId, std::move(listener));
    return nextId++;
}

void editor::GraphEditCmd::removeChangeListener(uint64_t id) {
    auto& listeners = graphChangeListeners();
    for (auto it = listeners.begin(); it != listeners.end(); ++it) {
        if (it->first != id) continue;
        listeners.erase(it);
        return;
    }
}

editor::GraphEditCmd::GraphEditCmd(std::filesystem::path file, std::string before, std::string after, std::string label)
    : file(std::move(file)), before(std::move(before)), after(std::move(after)), label(std::move(label)) {
}

bool editor::GraphEditCmd::write(const std::string& text) const {
    const io::WriteResult result = io::writeFileAtomic(file, text);
    if (!result.ok()) {
        Out::error("Could not write %s: %s", file.generic_string().c_str(), result.message.c_str());
        return false;
    }
    return true;
}

bool editor::GraphEditCmd::execute() {
    if (!write(after)) return false;
    notifyGraphFileChanged(file);
    return true;
}

void editor::GraphEditCmd::undo() {
    if (write(before)) notifyGraphFileChanged(file);
}
