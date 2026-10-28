// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AssetFileWrite.h"

#include "io/AtomicFile.h"
#include "io/watch/FileWatchService.h"

#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;

bool assetio::writeFile(const fs::path& target, std::string_view bytes, Durability durability,
                        std::string* error, bool* written) {
    io::AtomicWriteOptions options;
    options.durability = durability == Durability::Document ? io::Durability::Document : io::Durability::Cache;
    options.skipIfUnchanged = true;
    // the editor's own write (S05 R10), as renameFile's moves: a GUID written into a clip or a
    // meta is no external change for the watcher, the hot reload or the MCP pick-up
    ScopedExpectedWrite expected(target);
    const io::WriteResult result = io::writeFileAtomic(target, bytes, options);
    if (result.written()) {
        expected.reported();
    }
    if (written) {
        *written = result.written();
    }
    if (!result.ok() && error) {
        *error = io::describeWriteError(result);
    }
    return result.ok();
}

bool assetio::readFile(const fs::path& path, std::string& bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (in.bad()) {
        return false;
    }
    bytes = buffer.str();
    return true;
}

bool assetio::renameFile(const fs::path& from, const fs::path& to, std::string* error) {
    std::error_code ec;
    if (fs::exists(to, ec)) {
        if (error) *error = "\"" + to.generic_string() + "\" already exists";
        return false;
    }
    if (!to.parent_path().empty()) {
        fs::create_directories(to.parent_path(), ec);
    }
    ec.clear();
    // the editor's own move: the file watcher does not report it back (S05 R10)
    ScopedExpectedWrite expected(to);
    if (!io::renameWithRetry(from, to, ec)) {
        if (error) *error = ec.message();
        return false;
    }
    expected.reportedMoved(from, to);
    return true;
}

bool assetio::isTemporarySiblingName(std::string_view filename) {
    return io::isTemporarySiblingName(filename);
}
