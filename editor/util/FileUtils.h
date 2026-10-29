// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "io/AtomicFile.h"

#include "io/watch/FileWatchService.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#elif defined(__APPLE__)
    #include <mach-o/dyld.h>
#elif defined(__FreeBSD__)
    #include <sys/types.h>
    #include <sys/sysctl.h>
#endif
#ifndef _WIN32
    #include <unistd.h>
#endif

namespace jasperengine::editor {

class FileUtils {
public:
    static std::filesystem::path getExecutableDir() {
        return getExecutablePath().parent_path();
    }

    // The running editor's executable, absolute (the scene merge driver git runs, S29 R22)
    static std::filesystem::path getExecutablePath() {
        namespace fs = std::filesystem;

        fs::path executablePath;

#ifdef _WIN32
        std::vector<char> buffer(MAX_PATH);
        while (true) {
            const DWORD size = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (size == 0) {
                return {};
            }
            if (size < buffer.size()) {
                executablePath = std::string(buffer.data(), size);
                break;
            }
            buffer.resize(buffer.size() * 2);
        }
#elif defined(__APPLE__)
        std::vector<char> buffer(1024);
        while (true) {
            uint32_t size = static_cast<uint32_t>(buffer.size());
            if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
                executablePath = std::string(buffer.data());
                break;
            }
            buffer.resize(size);
        }
#elif defined(__FreeBSD__)
        const int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
        size_t size = 0;
        if (::sysctl(mib, 4, nullptr, &size, nullptr, 0) != 0 || size == 0) {
            return {};
        }

        std::vector<char> buffer(size);
        if (::sysctl(mib, 4, buffer.data(), &size, nullptr, 0) != 0 || size == 0) {
            return {};
        }

        const size_t pathLength = buffer[size - 1] == '\0' ? size - 1 : size;
        if (pathLength == 0) {
            return {};
        }
        executablePath = std::string(buffer.data(), pathLength);
#elif defined(__linux__)
        std::error_code readLinkError;
        executablePath = fs::read_symlink("/proc/self/exe", readLinkError);
        if (readLinkError) {
            return {};
        }
#else
        return {};
#endif

        std::error_code ec;
        fs::path normalizedPath = fs::weakly_canonical(executablePath, ec);
        if (ec) {
            normalizedPath = executablePath;
        }

        return normalizedPath;
    }

    // Inside JasperEngine.app the SDK sits in Contents/Resources: codesign treats
    // everything in Contents/MacOS as code and rejects plain files there.
    static std::filesystem::path getEngineDir() {
        namespace fs = std::filesystem;

        const fs::path exeDir = getExecutableDir();
#if defined(__APPLE__)
        std::error_code ec;
        const fs::path bundled = exeDir.parent_path() / "Resources" / "engine";
        if (fs::exists(bundled, ec)) {
            return bundled;
        }
#endif
        return exeDir / "engine";
    }

    // An AppImage mounts at a fresh path every launch, so anything written into
    // a file the project keeps must omit it or that file churns on every start.
    static bool isEngineDirEphemeral() {
        if (std::getenv("APPIMAGE") != nullptr || std::getenv("APPDIR") != nullptr) {
            return true;
        }

        // Fallback when the environment is cleared: both runtimes use $TMPDIR.
        const std::string engineDir = getEngineDir().generic_string();
        return engineDir.find("/.mount_") != std::string::npos ||
               engineDir.find("/appimage_extracted_") != std::string::npos;
    }

    enum class WriteResult { Unchanged, Written, Failed };

    // Writes newContent only when the file does not already hold it (its mtime stays then).
    // Atomic (io::writeFileAtomic, Cache durability): a crash leaves the old file or the new
    // one, never a truncation. Generated output; user documents use DocumentFiles::write.
    static WriteResult writeFile(const std::filesystem::path& filePath, const std::string& newContent) {
        io::AtomicWriteOptions options;
        options.durability = io::Durability::Cache;
        options.skipIfUnchanged = true;
        const io::WriteResult result = io::writeFileAtomic(filePath, newContent, options);
        if (result.error == io::WriteError::Unchanged) {
            return WriteResult::Unchanged;
        }
        return result.ok() ? WriteResult::Written : WriteResult::Failed;
    }

    // std::filesystem::rename with the publish retry policy (a file another program holds open
    // on Windows, an indexer): io::renameWithRetry, throwing like the original on failure
    // The rename is the editor's own: the file watcher does not report it back (S05 R10).
    static void renameWithRetry(const std::filesystem::path& from, const std::filesystem::path& to) {
        std::error_code ec;
        ScopedExpectedWrite write(to);
        if (!io::renameWithRetry(from, to, ec)) {
            throw std::filesystem::filesystem_error("rename", from, to, ec);
        }
        write.reportedMoved(from, to);
    }

    // What the editor made at `path` (a file, or a folder and what it holds) or removed from
    // it: its own writes, which the file watcher does not report back (S05 R10)
    static void reportWritten(const std::filesystem::path& path) {
        ScopedExpectedWrite write(path, true);
        write.reported(path);
        std::error_code ec;
        if (!std::filesystem::is_directory(path, ec)) return;
        for (std::filesystem::recursive_directory_iterator it(path, std::filesystem::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            write.reported(it->path());
        }
    }
    static void reportRemoved(const std::filesystem::path& path) {
        ScopedExpectedWrite write(path, true);
        write.reportedRemoved(path);
    }

    // Copies a file over another the atomic way: into a temporary sibling, then published
    // (io::publishFile), so the target is the old file or the whole new one, never a part
    static bool copyFileAtomic(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& ec) {
        const std::filesystem::path temp = io::makeTemporarySiblingPath(to);
        std::filesystem::copy_file(from, temp, std::filesystem::copy_options::none, ec);
        if (ec) {
            std::error_code ignored;
            std::filesystem::remove(temp, ignored);
            return false;
        }
        const io::WriteResult published = io::publishFile(temp, to);
        ec = published.code;
        return published.ok();
    }

    // How a document write treats the line endings of the content
    enum class LineEndings {
        AsGiven,        // the bytes as they are (binary files, content that is already final)
        KeepExisting    // text the editor makes with \n: a file there already written with
                        // \r\n (an existing Windows checkout) gets \r\n again, a new one \n
    };

    // True when the first line of the file ends with \r\n
    static bool fileUsesCrlf(const std::filesystem::path& filePath) {
        std::ifstream ifs(filePath, std::ios::in | std::ios::binary);
        char previous = 0;
        char c = 0;
        while (ifs.get(c)) {
            if (c == '\n') {
                return previous == '\r';
            }
            previous = c;
        }
        return false;
    }

    // Every line ending as \r\n (an \r\n already there stays one)
    static std::string toCrlf(const std::string& text) {
        std::string out;
        out.reserve(text.size() + text.size() / 16);
        for (size_t i = 0; i < text.size(); i++) {
            if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) {
                out += '\r';
            }
            out += text[i];
        }
        return out;
    }

    // The bytes a document's text is written as: with \r\n when the file it replaces (through
    // a symlink too) has them and lineEndings is KeepExisting, else the text as it is. Hash
    // what this returns (SceneProject::diskSha1): it is what lands on disk.
    static std::string withLineEndings(const std::filesystem::path& filePath, const std::string& text,
                                       LineEndings lineEndings = LineEndings::KeepExisting) {
        if (lineEndings != LineEndings::KeepExisting) {
            return text;
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(filePath, ec) || !fileUsesCrlf(filePath)) {
            return text;
        }
        return toCrlf(text);
    }

    // A document written the atomic way (io::writeFileAtomic, Document durability: fsync, mode
    // and symlink kept, a read-only file refused, retried publish), with its line endings
    // (withLineEndings). A failed write leaves the old file whole. DocumentFiles::write is the
    // same with the reason of a failure.
    static bool writeFileReplacing(const std::filesystem::path& filePath, const std::string& content,
                                   LineEndings lineEndings = LineEndings::AsGiven) {
        io::AtomicWriteOptions options;
        options.durability = io::Durability::Document;
        return io::writeFileAtomic(filePath, withLineEndings(filePath, content, lineEndings), options).ok();
    }

    // True when the file holds newContent afterwards, written or already up to date.
    // Use writeFile() when a no-op has to be told apart from a failure.
    static bool writeIfChanged(const std::filesystem::path& filePath, const std::string& newContent) {
        return writeFile(filePath, newContent) != WriteResult::Failed;
    }

    // path's UTF-8 conversions in C++20: u8string() returns std::u8string and u8path() is
    // deprecated. These keep the call sites on std::string.
    static std::filesystem::path pathFromUtf8(std::string_view text) {
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    }

    static std::string pathToUtf8(const std::filesystem::path& path) {
        const std::u8string utf8 = path.u8string();
        return std::string(utf8.begin(), utf8.end());
    }

    // generic_string() in UTF-8: '/' separators on every platform
    static std::string pathToGenericUtf8(const std::filesystem::path& path) {
        const std::u8string utf8 = path.generic_u8string();
        return std::string(utf8.begin(), utf8.end());
    }
};

} // namespace jasperengine::editor
