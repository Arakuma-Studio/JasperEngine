// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FileUtils.h"
#include "io/AtomicFile.h"

#include "FileData.h"
#include "Log.h"
#include "ResourcePack.h"
#include "System.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <system_error>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <io.h>
    #include <process.h>
    #include <share.h>
#else
    #include <unistd.h>
#endif

#if defined(__APPLE__)
    #include <TargetConditionals.h>
#endif

using namespace jasperengine;
namespace fs = std::filesystem;

namespace {

    // Absolute paths are read on the platforms whose files a player can see (desktop, the
    // editor); a phone or a browser has no path to give
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    constexpr bool ABSOLUTE_READS = false;
#else
    constexpr bool ABSOLUTE_READS = true;
#endif

    fs::path pathFromUtf8(const std::string& utf8){
#if defined(_WIN32)
        if (utf8.empty()) return fs::path();
        const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (size <= 0) return fs::path(utf8);
        std::wstring wide(static_cast<size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &wide[0], size);
        return fs::path(wide);
#else
        return fs::path(utf8);
#endif
    }

    std::string pathToUtf8(const fs::path& path){
#if defined(_WIN32)
        const std::wstring& wide = path.native();
        if (wide.empty()) return std::string();
        const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        if (size <= 0) return std::string();
        std::string utf8(static_cast<size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), &utf8[0], size, nullptr, nullptr);
        return utf8;
#else
        return path.native();
#endif
    }

    FILE* openFile(const fs::path& path, const char* mode){
#if defined(_WIN32)
        const std::wstring wideMode(mode, mode + std::strlen(mode));
        // shared like fopen (_wfopen_s would lock the file)
        return _wfsopen(path.c_str(), wideMode.c_str(), _SH_DENYNO);
#else
        return std::fopen(path.c_str(), mode);
#endif
    }

    std::string reasonOf(int error){
        switch (error){
            case ENOENT: return "not found";
            case EACCES:
            case EPERM: return "permission denied";
            case EISDIR: return "is a folder";
            case ENOSPC: return "no space left on the device";
            case EROFS: return "read-only file system";
            default: {
#if defined(_WIN32)
                char text[256] = {0};
                strerror_s(text, sizeof(text), error);
                return text;
#else
                return std::strerror(error);
#endif
            }
        }
    }

    std::string reasonOf(const std::error_code& ec){
        if (ec == std::errc::no_such_file_or_directory) return "not found";
        if (ec == std::errc::permission_denied || ec == std::errc::operation_not_permitted) return "permission denied";
        if (ec == std::errc::no_space_on_device) return "no space left on the device";
        if (ec == std::errc::read_only_file_system) return "read-only file system";
        return ec.message();
    }

    bool beginsWith(const std::string& text, const char* prefix){
        return text.compare(0, std::strlen(prefix), prefix) == 0;
    }

    // Android's asset:// and lua:// files are inside the APK (platformFopen reads them): their
    // system path is no file system path, and "/data" there would be the device's folder
    bool onFileSystem(const std::string& normalized){
#if defined(__ANDROID__)
        return beginsWith(normalized, "data://") || normalized.find("://") == std::string::npos;
#else
        (void)normalized;
        return true;
#endif
    }

    bool isAbsolute(const std::string& path){
        if (!path.empty() && (path[0] == '/' || path[0] == '\\')) return true;
        // C:\ or C:/
        return path.size() >= 2 && path[1] == ':' &&
               ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'));
    }

    bool hasDotDot(const std::string& path){
        size_t i = 0;
        while (i <= path.size()){
            size_t end = path.find_first_of("/\\", i);
            if (end == std::string::npos) end = path.size();
            if (path.compare(i, end - i, "..") == 0) return true;
            i = end + 1;
        }
        return false;
    }

    // "a//b\\c/./d" -> "a/b/c/d"; false on ".." or a ':' inside a segment
    bool joinSegments(const std::string& rest, std::string& out, std::string& error, const std::string& shown){
        out.clear();
        size_t i = 0;
        while (i <= rest.size()){
            size_t end = rest.find_first_of("/\\", i);
            if (end == std::string::npos) end = rest.size();
            const std::string segment = rest.substr(i, end - i);
            if (segment == ".."){
                error = "'..' is not allowed in paths: " + shown;
                return false;
            }
            if (segment.find(':') != std::string::npos){
                error = "':' is not allowed in path names: " + shown;
                return false;
            }
            if (!segment.empty() && segment != "."){
                if (!out.empty()) out += '/';
                out += segment;
            }
            i = end + 1;
        }
        return true;
    }

    // Reads every byte of an open file (fread in chunks: an APK asset has no reliable length)
    bool readAll(FILE* file, std::string& out){
        out.clear();
        char buffer[65536];
        while (true){
            const size_t n = std::fread(buffer, 1, sizeof(buffer), file);
            out.append(buffer, n);
            if (n < sizeof(buffer)){
                return !std::ferror(file);
            }
        }
    }

    // through S03's atomic writer (io::writeFileAtomic: a temporary sibling flushed to the disk,
    // then the rename; its retries, fault injection and read-only refusal), so a crash leaves
    // the old file or the new one
    bool atomicWrite(const fs::path& target, const std::string& bytes, std::string& reason){
        std::error_code ec;
        if (fs::is_directory(target, ec)){
            reason = "is a folder";
            return false;
        }
        const io::WriteResult written = io::writeFileAtomic(target, bytes);
        if (!written.ok()){
            reason = written.code ? reasonOf(written.code) : written.message;
            return false;
        }
        return true;
    }

    bool makeParents(const fs::path& target, std::string& reason){
        const fs::path parent = target.parent_path();
        if (parent.empty()) return true;
        std::error_code ec;
        if (fs::is_directory(parent, ec)) return true;
        fs::create_directories(parent, ec);
        if (ec){
            reason = reasonOf(ec);
            return false;
        }
        return true;
    }

}

bool FileUtils::resolve(const std::string& path, bool forWrite, std::string& normalized, std::string& systemPath, std::string& error){
    if (path.empty()){
        error = "empty path";
        return false;
    }
    const size_t scheme = path.find("://");
    if (scheme == std::string::npos){
        if (isAbsolute(path)){
            if (forWrite){
                error = "absolute paths are not allowed for writes (use data://): " + path;
                return false;
            }
            if (!ABSOLUTE_READS){
                error = "absolute paths are not allowed on this platform (use data://, asset:// or lua://): " + path;
                return false;
            }
            if (hasDotDot(path)){
                error = "'..' is not allowed in paths: " + path;
                return false;
            }
            normalized = path;
            systemPath = path;
            return true;
        }
        std::string joined;
        if (!joinSegments(path, joined, error, path)) return false;
        normalized = "data://" + joined;
    } else {
        const std::string prefix = path.substr(0, scheme + 3);
        if (prefix != "data://" && prefix != "asset://" && prefix != "lua://"){
            error = "unknown path scheme (use data://, asset:// or lua://): " + path;
            return false;
        }
        std::string joined;
        if (!joinSegments(path.substr(scheme + 3), joined, error, path)) return false;
        normalized = prefix + joined;
        if (forWrite && prefix != "data://"){
            error = "read-only path: " + normalized;
            return false;
        }
    }
    // the web build restores data:// from IndexedDB after it starts: a file written before that
    // would be replaced by the restore
    if (forWrite && !System::instance().isUserDataLoaded()){
        error = "the saved files are not restored yet (System.isUserDataLoaded() is false); write later: " + normalized;
        return false;
    }
    systemPath = FileData::getSystemPath(normalized);
    return true;
}

bool FileUtils::readText(const std::string& path, std::string& out, std::string& error){
    std::string normalized, systemPath;
    if (!resolve(path, false, normalized, systemPath, error)) return false;

    // a packed export keeps assets and Lua files in resources.pak
    if (!beginsWith(normalized, "data://") && ResourcePack::contains(normalized)){
        std::vector<unsigned char> bytes;
        if (!ResourcePack::read(normalized, bytes)){
            error = "cannot read the resource pack entry: " + normalized;
            return false;
        }
        out.assign(bytes.begin(), bytes.end());
        return true;
    }

    std::error_code ec;
    if (onFileSystem(normalized) && fs::is_directory(pathFromUtf8(systemPath), ec)){
        error = "is a folder: " + normalized;
        return false;
    }
    errno = 0;
    FILE* file = System::instance().platformFopen(systemPath.c_str(), "rb");
    if (!file){
        error = reasonOf(errno ? errno : ENOENT) + ": " + normalized;
        return false;
    }
    const bool ok = readAll(file, out);
    const int readError = errno;
    std::fclose(file);
    if (!ok){
        error = "cannot read " + normalized + ": " + reasonOf(readError ? readError : EIO);
        return false;
    }
    return true;
}

bool FileUtils::writeText(const std::string& path, const std::string& text, std::string& error){
    std::string normalized, systemPath;
    if (!resolve(path, true, normalized, systemPath, error)) return false;
    if (normalized == "data://"){
        error = "not a file path: " + path;
        return false;
    }
    const fs::path target = pathFromUtf8(systemPath);
    std::string reason;
    if (!makeParents(target, reason) || !atomicWrite(target, text, reason)){
        error = reason + ": " + normalized;
        return false;
    }
    System::instance().syncFileSystem();
    return true;
}

bool FileUtils::appendText(const std::string& path, const std::string& text, std::string& error){
    std::string normalized, systemPath;
    if (!resolve(path, true, normalized, systemPath, error)) return false;
    if (normalized == "data://"){
        error = "not a file path: " + path;
        return false;
    }
    const fs::path target = pathFromUtf8(systemPath);
    std::string reason;
    if (!makeParents(target, reason)){
        error = reason + ": " + normalized;
        return false;
    }
    std::error_code ec;
    if (fs::is_directory(target, ec)){
        error = "is a folder: " + normalized;
        return false;
    }
    FILE* file = openFile(target, "ab");
    if (!file){
        error = reasonOf(errno) + ": " + normalized;
        return false;
    }
    bool ok = text.empty() || std::fwrite(text.data(), 1, text.size(), file) == text.size();
    int writeError = ok ? 0 : errno;
    if (std::fclose(file) != 0 && ok){
        ok = false;
        writeError = errno;
    }
    if (!ok){
        error = "cannot write " + normalized + ": " + reasonOf(writeError ? writeError : EIO);
        return false;
    }
    System::instance().syncFileSystem();
    return true;
}

bool FileUtils::exists(const std::string& path){
    std::string normalized, systemPath, error;
    if (!resolve(path, false, normalized, systemPath, error)) return false;
    if (!beginsWith(normalized, "data://") && ResourcePack::contains(normalized)) return true;
    std::error_code ec;
    if (onFileSystem(normalized)) return fs::exists(pathFromUtf8(systemPath), ec);
    // an APK asset is no file of the file system
    FILE* file = System::instance().platformFopen(systemPath.c_str(), "rb");
    if (!file) return false;
    std::fclose(file);
    return true;
}

bool FileUtils::remove(const std::string& path, std::string& error){
    std::string normalized, systemPath;
    if (!resolve(path, true, normalized, systemPath, error)) return false;
    const fs::path target = pathFromUtf8(systemPath);
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(target, ec);
    if (status.type() == fs::file_type::not_found) return true;
    if (status.type() == fs::file_type::directory){
        error = "is a folder (File.remove removes files): " + normalized;
        return false;
    }
    if (!fs::remove(target, ec) && ec){
        error = reasonOf(ec) + ": " + normalized;
        return false;
    }
    System::instance().syncFileSystem();
    return true;
}

bool FileUtils::makeDirs(const std::string& path, std::string& error){
    std::string normalized, systemPath;
    if (!resolve(path, true, normalized, systemPath, error)) return false;
    const fs::path target = pathFromUtf8(systemPath);
    std::error_code ec;
    if (fs::is_directory(target, ec)) return true;
    if (fs::exists(target, ec)){
        error = "a file is there: " + normalized;
        return false;
    }
    fs::create_directories(target, ec);
    if (ec){
        error = reasonOf(ec) + ": " + normalized;
        return false;
    }
    System::instance().syncFileSystem();
    return true;
}

bool FileUtils::list(const std::string& dir, std::vector<std::string>& names, std::string& error){
    std::string normalized, systemPath;
    if (!resolve(dir, false, normalized, systemPath, error)) return false;
    if (!beginsWith(normalized, "data://")){
        error = "only data:// folders can be listed: " + normalized;
        return false;
    }
    names.clear();
    const fs::path folder = pathFromUtf8(systemPath);
    std::error_code ec;
    if (!fs::is_directory(folder, ec)){
        error = (fs::exists(folder, ec) ? "not a folder: " : "not found: ") + normalized;
        return false;
    }
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)){
        names.push_back(pathToUtf8(it->path().filename()));
    }
    if (ec){
        error = reasonOf(ec) + ": " + normalized;
        return false;
    }
    std::sort(names.begin(), names.end());
    return true;
}

bool FileUtils::writeFileAtomic(const std::string& systemPath, const std::string& bytes, std::string& error){
    const fs::path target = pathFromUtf8(systemPath);
    std::string reason;
    if (!makeParents(target, reason) || !atomicWrite(target, bytes, reason)){
        error = reason + ": " + systemPath;
        return false;
    }
    return true;
}

bool FileUtils::renameFile(const std::string& fromSystemPath, const std::string& toSystemPath, std::string& error){
    std::error_code ec;
    fs::rename(pathFromUtf8(fromSystemPath), pathFromUtf8(toSystemPath), ec);
    if (ec){
        error = reasonOf(ec) + ": " + fromSystemPath;
        return false;
    }
    return true;
}

bool FileUtils::prepareUserDataFolder(const std::string& systemDir){
    const fs::path dir = pathFromUtf8(systemDir);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!fs::is_directory(dir, ec)){
        Log::error("Cannot create the user data folder %s; saving next to the game instead", systemDir.c_str());
        return false;
    }
    // older builds saved in the working directory (the install folder): copied once, on the
    // first run with this folder (a marker says it was tried), so a reset or a deleted file
    // later never brings those old values back
    const fs::path marker = dir / ".jasperengine-migrated";
    if (fs::exists(marker, ec)) return true;
    const fs::path old = fs::path(".") / "UserSettings.xml";
    const fs::path current = dir / "UserSettings.xml";
    if (!fs::exists(current, ec) && fs::is_regular_file(old, ec) && !fs::equivalent(old, current, ec)){
        fs::copy_file(old, current, ec);
        if (ec)
            Log::error("Cannot copy ./UserSettings.xml to %s: %s", systemDir.c_str(), ec.message().c_str());
        else
            Log::print("UserSettings: copied ./UserSettings.xml of an older build to %s", systemDir.c_str());
    }
    if (FILE* file = openFile(marker, "wb")){
        std::fputs("./UserSettings.xml of older builds was looked for once; see FileUtils::prepareUserDataFolder\n", file);
        std::fclose(file);
    }
    return true;
}
