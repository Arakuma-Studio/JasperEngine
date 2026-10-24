// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "io/AtomicFile.h"

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

namespace jasperengine::editor {

class FileUtils {
public:
    static std::filesystem::path getExecutableDir() {
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

        return normalizedPath.parent_path();
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
    static void renameWithRetry(const std::filesystem::path& from, const std::filesystem::path& to) {
        std::error_code ec;
        if (!io::renameWithRetry(from, to, ec)) {
            throw std::filesystem::filesystem_error("rename", from, to, ec);
        }
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

    // with '/' separators on every platform
    static std::string genericPathToUtf8(const std::filesystem::path& path) {
        const std::u8string utf8 = path.generic_u8string();
        return std::string(utf8.begin(), utf8.end());
    }
};

} // namespace jasperengine::editor
