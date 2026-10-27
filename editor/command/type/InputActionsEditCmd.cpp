// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "InputActionsEditCmd.h"

#include "Out.h"
#include "input/InputActions.h"
#include "io/AtomicFile.h"
#include "io/FileData.h"

#include <atomic>
#include <system_error>

using namespace jasperengine;

namespace {
    std::atomic<uint64_t> revision{0};
}

editor::InputActionsEditCmd::InputActionsEditCmd(std::filesystem::path file, std::string before, std::string after, std::string label,
                                                 bool created)
    : file(std::move(file)), before(std::move(before)), after(std::move(after)), label(std::move(label)), created(created) {
}

bool editor::InputActionsEditCmd::write(const std::string& text) const {
    const io::WriteResult result = io::writeFileAtomic(file, text);
    if (!result.ok()) {
        Out::error("Could not write %s: %s", file.generic_string().c_str(), result.message.c_str());
        return false;
    }
    revision++;
    // the game playing with this asset sees the edit at once (its maps keep their state)
    if (InputActions::isLoaded()) {
        std::error_code ec;
        const std::filesystem::path loaded = FileData::getSystemPath(InputActions::getAssetPath());
        if (std::filesystem::equivalent(loaded, file, ec) || loaded == file)
            InputActions::reloadText(text);
    }
    return true;
}

bool editor::InputActionsEditCmd::execute() {
    return write(after);
}

void editor::InputActionsEditCmd::undo() {
    if (created) {
        std::error_code ec;
        std::filesystem::remove(file, ec);
        if (ec) Out::error("Could not remove %s: %s", file.generic_string().c_str(), ec.message().c_str());
        revision++;
        return;
    }
    write(before);
}

uint64_t editor::InputActionsEditCmd::getRevision() {
    return revision.load();
}
