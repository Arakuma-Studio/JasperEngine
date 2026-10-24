// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>
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

    // Writes newContent only when the file does not already hold it.
    static WriteResult writeFile(const std::filesystem::path& filePath, const std::string& newContent) {
        std::error_code ec;
        if (std::filesystem::exists(filePath, ec) && !ec) {
            std::ifstream ifs(filePath, std::ios::in | std::ios::binary);
            if (ifs) {
                std::string currentContent(
                    (std::istreambuf_iterator<char>(ifs)),
                    std::istreambuf_iterator<char>()
                );
                if (currentContent == newContent) {
                    return WriteResult::Unchanged;
                }
            }
        }

        if (filePath.has_parent_path()) {
            std::filesystem::create_directories(filePath.parent_path(), ec);
            if (ec) {
                return WriteResult::Failed;
            }
        }

        std::ofstream ofs(filePath, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!ofs) {
            return WriteResult::Failed;
        }
        ofs.write(newContent.data(), static_cast<std::streamsize>(newContent.size()));
        return ofs ? WriteResult::Written : WriteResult::Failed;
    }

    // How writeFileReplacing() treats the line endings of the content
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

    // Writes a temporary file next to the target and renames it over the target, so a save
    // that fails (a full disk, a crash halfway) keeps the old file whole. rename() replaces an
    // existing target on Windows too (MoveFileExW with MOVEFILE_REPLACE_EXISTING). As with a
    // plain write, a symlink is written through and a file the user may not write (read-only,
    // a locked file) is refused; an existing file keeps its permissions.
    static bool writeFileReplacing(const std::filesystem::path& filePath, const std::string& content,
                                   LineEndings lineEndings = LineEndings::AsGiven) {
        namespace fs = std::filesystem;

        std::error_code ec;
        fs::path target = filePath;
        if (fs::is_symlink(target, ec)) {
            fs::path resolved = fs::canonical(target, ec);
            if (!ec) {
                target = resolved;
            }
        }

        const fs::file_status status = fs::status(target, ec);
        const bool exists = !ec && fs::exists(status);
        const bool existingFile = exists && fs::is_regular_file(status);
#ifndef _WIN32
        // The directory's permissions would let the rename through
        if (exists && ::access(target.c_str(), W_OK) != 0) {
            return false;
        }
#endif

        const bool crlf = lineEndings == LineEndings::KeepExisting && existingFile && fileUsesCrlf(target);

        // Same directory, so the rename never crosses a file system
        fs::path tempName(".");
        tempName += target.filename();
        tempName += ".tmp";
        const fs::path tempPath = target.parent_path() / tempName;

        bool written = false;
        {
            std::ofstream ofs(tempPath, std::ios::out | std::ios::binary | std::ios::trunc);
            if (ofs) {
                const std::string converted = crlf ? toCrlf(content) : std::string();
                const std::string& data = crlf ? converted : content;
                ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
                ofs.flush();
                ofs.close();
                written = !ofs.fail();
            }
        }

        if (written) {
            if (existingFile) {
                fs::permissions(tempPath, status.permissions(), fs::perm_options::replace, ec);
            }
            // Windows cannot replace a file that a reader, the indexer or a virus scan
            // briefly holds, so the rename is tried again for a while
            for (int attempt = 0; attempt < renameAttempts; attempt++) {
                if (attempt > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(renameRetryMs));
                }
                ec.clear();
                fs::rename(tempPath, target, ec);
                if (!ec) {
                    return true;
                }
            }
        }

        // errno still tells why the write failed
        const int writeErrno = errno;
        fs::remove(tempPath, ec);
        errno = writeErrno;
        return false;
    }

    // writeFileReplacing(): renames tried before it fails, and the wait between them
    static constexpr int renameAttempts = 10;
    static constexpr int renameRetryMs = 50;

    // True when the file holds newContent afterwards, written or already up to date.
    // Use writeFile() when a no-op has to be told apart from a failure.
    static bool writeIfChanged(const std::filesystem::path& filePath, const std::string& newContent) {
        return writeFile(filePath, newContent) != WriteResult::Failed;
    }

    // path's UTF-8 conversions changed shape in C++20: u8string() returns std::u8string
    // and u8path() is deprecated. These keep the call sites on std::string either way.
    static std::filesystem::path pathFromUtf8(const std::string& text) {
#ifdef __cpp_lib_char8_t
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
#else
        return std::filesystem::u8path(text);
#endif
    }

    static std::string pathToUtf8(const std::filesystem::path& path) {
        const auto utf8 = path.u8string();
        return std::string(utf8.begin(), utf8.end());
    }
};

} // namespace jasperengine::editor
