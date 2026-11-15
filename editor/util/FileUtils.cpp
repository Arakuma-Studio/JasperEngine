// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FileUtils.h"

#ifdef _WIN32

#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

std::filesystem::path FileUtils::windowsModulePath() {
    std::vector<wchar_t> buffer(MAX_PATH);
    while (true) {
        const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0) {
            return {};
        }
        if (size < buffer.size()) {
            return std::filesystem::path(std::wstring(buffer.data(), size));
        }
        buffer.resize(buffer.size() * 2);
    }
}

bool FileUtils::windowsWriteNewFile(const std::filesystem::path& path, const char* data, size_t size) {
    // FILE_FLAG_OPEN_REPARSE_POINT: a reparse point already at the name (a symlink, a
    // junction) is the entry itself, so CREATE_NEW fails with ERROR_FILE_EXISTS instead of
    // following it
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        errno = (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) ? EEXIST
              : code == ERROR_ACCESS_DENIED ? EACCES
              : (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) ? ENOENT
              : code == ERROR_SHARING_VIOLATION ? EBUSY
              : code == ERROR_INVALID_NAME ? EINVAL : EIO;
        return false;
    }
    size_t done = 0;
    bool ok = true;
    while (ok && done < size) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size - done, 1u << 30));
        DWORD wrote = 0;
        ok = WriteFile(file, data + done, chunk, &wrote, nullptr) && wrote > 0;
        if (!ok) {
            const DWORD code = GetLastError();
            errno = (code == ERROR_DISK_FULL || code == ERROR_HANDLE_DISK_FULL) ? ENOSPC : EIO;
        }
        done += wrote;
    }
    if (!CloseHandle(file) && ok) {
        errno = EIO;
        ok = false;
    }
    return ok;
}

} // namespace jasperengine::editor

#endif
