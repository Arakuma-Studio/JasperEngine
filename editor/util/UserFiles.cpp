// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UserFiles.h"

#include "DocumentFile.h"
#include "io/AtomicFile.h"
#include "io/ScopedFileLock.h"

#include <fstream>
#include <iterator>
#include <system_error>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    std::filesystem::path lockFor(const std::filesystem::path& file, const std::filesystem::path& lockFile) {
        if (!lockFile.empty()) return lockFile;
        std::filesystem::path lock = file;
        lock += ".lock";
        return lock;
    }

}

bool userfiles::read(const std::filesystem::path& file, std::string& text) {
    text.clear();
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

bool userfiles::write(const std::filesystem::path& file, const std::string& text, std::string* error,
                      const std::filesystem::path& lockFile) {
    std::error_code ec;
    if (!file.parent_path().empty()) std::filesystem::create_directories(file.parent_path(), ec);
    io::ScopedFileLock lock(lockFor(file, lockFile));
    if (!lock.isLocked() && lock.getStatus() != io::ScopedFileLock::Status::Unsupported) {
        if (error) *error = lock.failureReason();
        return false;
    }
    io::AtomicWriteOptions options;
    options.durability = io::Durability::Document;
    options.skipIfUnchanged = true;
    const io::WriteResult result = io::writeFileAtomic(file, text, options);
    if (!result.ok()) {
        if (error) *error = DocumentFiles::saveFailureMessage(file, result);
        return false;
    }
    return true;
}

bool userfiles::remove(const std::filesystem::path& file, std::string* error, const std::filesystem::path& lockFile) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return true;
    io::ScopedFileLock lock(lockFor(file, lockFile));
    std::filesystem::remove(file, ec);
    if (ec) {
        if (error) *error = "Could not delete " + file.generic_string() + ": " + ec.message();
        return false;
    }
    return true;
}
