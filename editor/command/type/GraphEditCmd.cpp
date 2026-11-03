// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GraphEditCmd.h"

#include "Out.h"
#include "io/AtomicFile.h"

using namespace jasperengine;

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
    return write(after);
}

void editor::GraphEditCmd::undo() {
    write(before);
}
