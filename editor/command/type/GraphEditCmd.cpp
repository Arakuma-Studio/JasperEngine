// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GraphEditCmd.h"

#include "Out.h"
#include "io/AtomicFile.h"
#include "util/SafeWrite.h"

#include <algorithm>
#include <utility>
#include <vector>

using namespace jasperengine;

namespace {

    std::vector<std::pair<uint64_t, editor::GraphEditCmd::ChangeListener>>& graphChangeListeners() {
        static std::vector<std::pair<uint64_t, editor::GraphEditCmd::ChangeListener>> listeners;
        return listeners;
    }

    void notifyGraphFileChanged(const std::filesystem::path& file) {
        // by id: a listener may remove itself or another, and what that one captures may be gone
        std::vector<uint64_t> ids;
        for (const auto& item : graphChangeListeners()) ids.push_back(item.first);
        for (const uint64_t id : ids) {
            const auto& listeners = graphChangeListeners();
            const auto it = std::find_if(listeners.begin(), listeners.end(), [id](const auto& item) { return item.first == id; });
            if (it == listeners.end()) continue;
            const editor::GraphEditCmd::ChangeListener listener = it->second;
            listener(file);
        }
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

editor::GraphEditCmd::GraphEditCmd(std::filesystem::path file, std::string before, std::string after, std::string label, bool createsFile)
    : file(std::move(file)), before(std::move(before)), after(std::move(after)), label(std::move(label)), creates(createsFile) {
}

bool editor::GraphEditCmd::write(const std::string& text) const {
    // beneath the write root, as WriteFilesCmd writes: a link planted at the file or a folder on
    // its way does not lead the write out of the project
    const std::filesystem::path root = rootFor(file);
    std::string error;
    if (!root.empty()) {
        const std::string refused = safewrite::scopesRefusal(file);
        if (refused.empty() && safewrite::writeFileResolvedBeneath(root, file, text, error)) return true;
        Out::error("Could not write %s: %s", file.generic_string().c_str(), (refused.empty() ? error : refused).c_str());
        return false;
    }
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
    if (creates) {
        std::error_code ec;
        std::filesystem::remove(file, ec);
        if (ec) {
            Out::error("Could not remove %s: %s", file.generic_string().c_str(), ec.message().c_str());
            return;
        }
        notifyGraphFileChanged(file);
        return;
    }
    if (write(before)) notifyGraphFileChanged(file);
}
