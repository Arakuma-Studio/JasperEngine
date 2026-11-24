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
#include <string_view>
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

std::wstring FileUtils::wideFromUtf8(std::string_view text) {
    // the code point of the sequence at i and its length; 0 when the byte at i is to be escaped
    auto sequenceAt = [&text](size_t i, char32_t& point) -> size_t {
        const unsigned char b0 = static_cast<unsigned char>(text[i]);
        size_t length = 0;
        char32_t least = 0;
        if (b0 < 0x80) {
            point = b0;
            return 1;
        } else if (b0 >= 0xC2 && b0 <= 0xDF) {
            length = 2; point = b0 & 0x1F; least = 0x80;
        } else if (b0 >= 0xE0 && b0 <= 0xEF) {
            length = 3; point = b0 & 0x0F; least = 0x800;
        } else if (b0 >= 0xF0 && b0 <= 0xF4) {
            length = 4; point = b0 & 0x07; least = 0x10000;
        } else {
            return 0;
        }
        if (i + length > text.size()) return 0;
        for (size_t k = 1; k < length; k++) {
            const unsigned char b = static_cast<unsigned char>(text[i + k]);
            if ((b & 0xC0) != 0x80) return 0;
            point = (point << 6) | (b & 0x3F);
        }
        // (an escape's three-byte form stays bytes: utf8FromWide gives the byte back, not it)
        if (point < least || point > 0x10FFFF || (point >= 0xDC80 && point <= 0xDCFF)) return 0;
        return length;
    };
    std::wstring out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        char32_t point = 0;
        size_t length = sequenceAt(i, point);
        if (length > 0 && point >= 0xD800 && point <= 0xDBFF && i + length < text.size()) {
            // a high surrogate is decoded only when no low one follows: they would make a pair,
            // which utf8FromWide writes as one four-byte character
            char32_t next = 0;
            const size_t nextLength = sequenceAt(i + length, next);
            if (nextLength == 0 || (next >= 0xDC00 && next <= 0xDFFF)) length = 0;
        }
        if (length == 0) {
            out.push_back(static_cast<wchar_t>(0xDC00 + static_cast<unsigned char>(text[i])));
            i++;
            continue;
        }
        if (point >= 0x10000) {
            out.push_back(static_cast<wchar_t>(0xD800 + ((point - 0x10000) >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + ((point - 0x10000) & 0x3FF)));
        } else {
            out.push_back(static_cast<wchar_t>(point));
        }
        i += length;
    }
    return out;
}

std::string FileUtils::utf8FromWide(std::wstring_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++) {
        char32_t point = static_cast<char32_t>(text[i]);
        if (point >= 0xD800 && point <= 0xDBFF && i + 1 < text.size() && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
            point = 0x10000 + ((point - 0xD800) << 10) + (static_cast<char32_t>(text[i + 1]) - 0xDC00);
            i++;
        } else if (point >= 0xDC80 && point <= 0xDCFF) {
            out.push_back(static_cast<char>(point - 0xDC00));
            continue;
        }
        if (point < 0x80) {
            out.push_back(static_cast<char>(point));
        } else if (point < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (point >> 6)));
            out.push_back(static_cast<char>(0x80 | (point & 0x3F)));
        } else if (point < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (point >> 12)));
            out.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (point & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (point >> 18)));
            out.push_back(static_cast<char>(0x80 | ((point >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (point & 0x3F)));
        }
    }
    return out;
}

} // namespace jasperengine::editor

#endif
