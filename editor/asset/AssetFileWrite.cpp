// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AssetFileWrite.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace jasperengine::editor;

namespace {

    std::atomic<unsigned long> tempCounter{0};

    unsigned long processId() {
#ifdef _WIN32
        return static_cast<unsigned long>(_getpid());
#else
        return static_cast<unsigned long>(getpid());
#endif
    }

    fs::path temporarySibling(const fs::path& target) {
        const std::string name = "." + target.filename().string() + "." + std::to_string(processId()) + "-" +
                                 std::to_string(tempCounter.fetch_add(1)) + ".je-tmp";
        return target.parent_path() / name;
    }

    bool retryable(const std::error_code& ec) {
        return ec == std::errc::permission_denied || ec == std::errc::device_or_resource_busy;
    }

    // Renames with the S03 policy: 5 attempts 200 ms apart while the error is a sharing one
    bool renameWithRetry(const fs::path& from, const fs::path& to, std::error_code& ec) {
        for (int attempt = 0; attempt < 5; attempt++) {
            ec.clear();
            fs::rename(from, to, ec);
            if (!ec || !retryable(ec)) {
                return !ec;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        return false;
    }

    bool writeTemporary(const fs::path& temp, std::string_view bytes, assetio::Durability durability, std::string& error) {
#ifdef _WIN32
        {
            std::ofstream file(temp, std::ios::binary | std::ios::trunc);
            if (!file) {
                error = "cannot create " + temp.string();
                return false;
            }
            file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!file.flush()) {
                error = "cannot write " + temp.string();
                return false;
            }
        }
        (void)durability;
        return true;
#else
        const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            error = "cannot create " + temp.string();
            return false;
        }
        size_t done = 0;
        while (done < bytes.size()) {
            const ssize_t count = ::write(fd, bytes.data() + done, bytes.size() - done);
            if (count <= 0) {
                ::close(fd);
                error = "cannot write " + temp.string();
                return false;
            }
            done += static_cast<size_t>(count);
        }
        if (durability == assetio::Durability::Document && ::fsync(fd) != 0) {
            ::close(fd);
            error = "cannot sync " + temp.string();
            return false;
        }
        return ::close(fd) == 0;
#endif
    }

}

bool assetio::readFile(const fs::path& path, std::string& bytes) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    bytes = buffer.str();
    return true;
}

bool assetio::writeFile(const fs::path& target, std::string_view bytes, Durability durability, std::string* error, bool* written) {
    if (written) *written = false;
    std::string current;
    std::error_code ec;
    if (fs::is_regular_file(target, ec) && readFile(target, current) && current == bytes) {
        return true;
    }
    if (target.has_parent_path()) {
        fs::create_directories(target.parent_path(), ec);
    }
    // A read-only target signals a VCS lock (Git LFS, Perforce): leave it alone
    const fs::file_status status = fs::status(target, ec);
    if (!ec && fs::exists(status) && (status.permissions() & fs::perms::owner_write) == fs::perms::none) {
        if (error) *error = target.string() + " is read-only";
        return false;
    }

    const fs::path temp = temporarySibling(target);
    std::string message;
    if (!writeTemporary(temp, bytes, durability, message)) {
        fs::remove(temp, ec);
        if (error) *error = message;
        return false;
    }
    if (!renameWithRetry(temp, target, ec)) {
        std::error_code ignored;
        fs::remove(temp, ignored);
        if (error) *error = "cannot replace " + target.string() + ": " + ec.message();
        return false;
    }
    if (written) *written = true;
    return true;
}

bool assetio::renameFile(const fs::path& from, const fs::path& to, std::string* error) {
    std::error_code ec;
    if (fs::exists(to, ec)) {
        if (error) *error = to.string() + " already exists";
        return false;
    }
    if (to.has_parent_path()) {
        fs::create_directories(to.parent_path(), ec);
    }
    if (!renameWithRetry(from, to, ec)) {
        if (error) *error = "cannot move " + from.string() + " to " + to.string() + ": " + ec.message();
        return false;
    }
    return true;
}

bool assetio::isTemporarySiblingName(std::string_view filename) {
    const std::string_view suffix = ".je-tmp";
    return filename.size() > suffix.size() + 1 && filename.front() == '.' &&
           filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) == 0;
}
