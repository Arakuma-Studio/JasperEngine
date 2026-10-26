// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RemoveFilesCmd.h"

#include "Out.h"
#include "util/FileUtils.h"

#include <fstream>
#include <iterator>

using namespace jasperengine;

namespace fs = std::filesystem;

namespace {

// the removed file back, written the atomic way (S03)
bool writeWhole(const fs::path& path, const std::string& content) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    return FileUtils::writeFileReplacing(path, content);
}

}

editor::RemoveFilesCmd::RemoveFilesCmd(std::vector<fs::path> files, std::function<void(const std::vector<fs::path>&)> afterChange)
    : files(std::move(files))
    , afterChange(std::move(afterChange)) {
}

bool editor::RemoveFilesCmd::execute() {
    // read everything first: a missing or unreadable file removes nothing
    contents.clear();
    for (const fs::path& file : files) {
        std::error_code ec;
        if (!fs::is_regular_file(file, ec)) return false;
        std::ifstream in(file, std::ios::binary);
        if (!in) return false;
        contents.emplace_back(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (in.bad()) return false;
    }
    for (size_t i = 0; i < files.size(); i++) {
        std::error_code ec;
        if (!fs::remove(files[i], ec) || ec) {
            // all or nothing: the files already removed come back
            for (size_t j = 0; j < i; j++) writeWhole(files[j], contents[j]);
            if (afterChange && i > 0) afterChange(files);
            return false;
        }
    }
    if (afterChange) afterChange(files);
    return true;
}

void editor::RemoveFilesCmd::undo() {
    bool failed = false;
    for (size_t i = 0; i < files.size() && i < contents.size(); i++) {
        if (!writeWhole(files[i], contents[i])) failed = true;
    }
    if (failed) {
        // Command::undo cannot fail; the history step stays (a redo removes what came back)
        Out::error("Restoring a removed file failed (permissions or a full disk)");
    }
    if (afterChange) afterChange(files);
}
