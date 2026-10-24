// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RemoveFilesCmd.h"

#include <fstream>
#include <iterator>

using namespace jasperengine;

namespace fs = std::filesystem;

editor::RemoveFilesCmd::RemoveFilesCmd(std::vector<fs::path> files, std::function<void(const std::vector<fs::path>&)> afterChange)
    : files(std::move(files))
    , afterChange(std::move(afterChange)) {
}

bool editor::RemoveFilesCmd::execute() {
    // read everything first: a failure removes nothing
    contents.clear();
    for (const fs::path& file : files) {
        std::error_code ec;
        if (!fs::is_regular_file(file, ec)) return false;
        std::ifstream in(file, std::ios::binary);
        if (!in) return false;
        contents.emplace_back(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    for (const fs::path& file : files) {
        std::error_code ec;
        fs::remove(file, ec);
    }
    if (afterChange) afterChange(files);
    return true;
}

void editor::RemoveFilesCmd::undo() {
    for (size_t i = 0; i < files.size() && i < contents.size(); i++) {
        std::error_code ec;
        fs::create_directories(files[i].parent_path(), ec);
        std::ofstream out(files[i], std::ios::binary | std::ios::trunc);
        out << contents[i];
    }
    if (afterChange) afterChange(files);
}
