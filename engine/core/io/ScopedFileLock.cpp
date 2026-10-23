// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ScopedFileLock.h"

#include <cerrno>
#include <fstream>
#include <iterator>
#include <system_error>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <fcntl.h>
    #include <sys/file.h>
    #include <unistd.h>
#endif

namespace fs = std::filesystem;

using namespace jasperengine::io;

namespace {

#if defined(_WIN32)
    // Past any owner text: readers of the file are never blocked by the locked range
    constexpr DWORD kLockOffsetLow = 1u << 30;
#endif

    std::string pathText(const fs::path& path){
        const auto utf8 = path.u8string();
        return std::string(utf8.begin(), utf8.end());
    }

}

ScopedFileLock::ScopedFileLock(const fs::path& lockFile, Mode mode, bool wait): path(lockFile) {
    std::error_code ec;
    if (lockFile.has_parent_path())
        fs::create_directories(lockFile.parent_path(), ec);

#if defined(__EMSCRIPTEN__)
    (void)mode;
    (void)wait;
    status = Status::Locked;
#elif defined(_WIN32)
    HANDLE h = ::CreateFileW(lockFile.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE){
        const DWORD error = ::GetLastError();
        status = (error == ERROR_WRITE_PROTECT || error == ERROR_ACCESS_DENIED) ? Status::Unsupported : Status::Failed;
        reason = "could not open '" + pathText(lockFile) + "': " + std::system_category().message((int)error);
        return;
    }
    OVERLAPPED overlapped{};
    overlapped.Offset = kLockOffsetLow;
    const DWORD flags = (mode == Mode::Exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0) | (wait ? 0 : LOCKFILE_FAIL_IMMEDIATELY);
    if (!::LockFileEx(h, flags, 0, 1, 0, &overlapped)){
        const DWORD error = ::GetLastError();
        if (error == ERROR_LOCK_VIOLATION || error == ERROR_IO_PENDING)
            status = Status::Busy;
        else if (error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_FUNCTION)
            status = Status::Unsupported;
        else
            status = Status::Failed;
        reason = "could not lock '" + pathText(lockFile) + "': " + std::system_category().message((int)error);
        ::CloseHandle(h);
        return;
    }
    handle = h;
    status = Status::Locked;
#else
    int flags = O_CREAT | O_RDWR;
    #ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
    #endif
    fd = ::open(lockFile.c_str(), flags, 0644);
    if (fd < 0){
        const int error = errno;
        status = (error == EROFS || error == EACCES || error == ENOTSUP) ? Status::Unsupported : Status::Failed;
        reason = "could not open '" + pathText(lockFile) + "': " + std::generic_category().message(error);
        return;
    }
    const int operation = (mode == Mode::Exclusive ? LOCK_EX : LOCK_SH) | (wait ? 0 : LOCK_NB);
    int result;
    do {
        result = ::flock(fd, operation);
    } while (result != 0 && errno == EINTR);
    if (result != 0){
        const int error = errno;
        if (error == EWOULDBLOCK)
            status = Status::Busy;
        else if (error == ENOLCK || error == ENOTSUP || error == EOPNOTSUPP)
            status = Status::Unsupported;
        else
            status = Status::Failed;
        reason = "could not lock '" + pathText(lockFile) + "': " + std::generic_category().message(error);
        ::close(fd);
        fd = -1;
        return;
    }
    status = Status::Locked;
#endif
}

ScopedFileLock::~ScopedFileLock(){
    release();
}

void ScopedFileLock::release(){
#if defined(_WIN32)
    if (handle){
        OVERLAPPED overlapped{};
        overlapped.Offset = kLockOffsetLow;
        ::UnlockFileEx((HANDLE)handle, 0, 1, 0, &overlapped);
        ::CloseHandle((HANDLE)handle);
        handle = nullptr;
    }
#elif !defined(__EMSCRIPTEN__)
    if (fd >= 0){
        ::close(fd);
        fd = -1;
    }
#endif
    if (status == Status::Locked)
        status = Status::Failed;
}

bool ScopedFileLock::writeOwnerInfo(const std::string& text){
    if (!isLocked())
        return false;
#if defined(__EMSCRIPTEN__)
    // no lock file is opened in the browser: nobody else could read it
    (void)text;
    return false;
#elif defined(_WIN32)
    HANDLE h = (HANDLE)handle;
    LARGE_INTEGER zero{};
    if (!::SetFilePointerEx(h, zero, nullptr, FILE_BEGIN) || !::SetEndOfFile(h))
        return false;
    DWORD written = 0;
    return ::WriteFile(h, text.data(), (DWORD)text.size(), &written, nullptr) && written == text.size();
#else
    if (::ftruncate(fd, 0) != 0)
        return false;
    size_t done = 0;
    while (done < text.size()){
        const ssize_t n = ::pwrite(fd, text.data() + done, text.size() - done, (off_t)done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        done += (size_t)n;
    }
    return true;
#endif
}

std::string ScopedFileLock::readOwnerInfo(const fs::path& lockFile){
    std::ifstream in(lockFile, std::ios::binary);
    if (!in)
        return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
