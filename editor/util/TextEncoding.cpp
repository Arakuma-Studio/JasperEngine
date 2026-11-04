// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TextEncoding.h"

#include "util/StringUtils.h"

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

#ifdef _WIN32
    std::wstring widenUtf8(const std::string& text) {
        const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (size <= 0) return std::wstring();
        std::wstring wide(static_cast<size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
        return wide;
    }

    // A console or a pipe to write to: a program started from a shell, an MCP client, a CI job.
    // Started from Explorer, a GUI program has neither.
    bool hasErrorStream() {
        const HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
        if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return false;
        const DWORD type = GetFileType(handle);
        return type == FILE_TYPE_CHAR || type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK;
    }

    bool isConsole(DWORD which) {
        DWORD mode = 0;
        const HANDLE handle = GetStdHandle(which);
        return handle != nullptr && handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode);
    }

    UINT shellCodePage = 0;
#endif

}

std::optional<std::string> TextEncoding::codePageProblem(unsigned activeCodePage) {
    if (activeCodePage == 65001) return std::nullopt;
    return std::string("JasperEngine Editor needs Windows 10 version 1903 or later: this Windows ignores the "
                       "editor's UTF-8 setting (code page ") + std::to_string(activeCodePage) +
           "), so names in other languages would corrupt projects. Update Windows to use the editor "
           "(games made with it still run here).";
}

bool TextEncoding::checkWindowsCodePage() {
#ifdef _WIN32
    const std::optional<std::string> problem = codePageProblem(GetACP());
    if (!problem) return true;
    if (hasErrorStream()) {
        std::fprintf(stderr, "%s\n", problem->c_str());
        std::fflush(stderr);
    } else {
        MessageBoxW(nullptr, widenUtf8(*problem).c_str(), L"JasperEngine Editor", MB_OK | MB_ICONERROR);
    }
    return false;
#else
    return true;
#endif
}

void TextEncoding::useUtf8Console() {
#ifdef _WIN32
    if (shellCodePage != 0 || !(isConsole(STD_OUTPUT_HANDLE) || isConsole(STD_ERROR_HANDLE))) return;
    const UINT current = GetConsoleOutputCP();
    if (current == CP_UTF8 || !SetConsoleOutputCP(CP_UTF8)) return;
    shellCodePage = current;
    std::atexit([]() { SetConsoleOutputCP(shellCodePage); });
#endif
}

std::string TextEncoding::consoleToUtf8(std::string_view line) {
    if (StringUtils::isValidUtf8(line)) return std::string(line);
#ifdef _WIN32
    // the system's console code page: under the UTF-8 manifest GetOEMCP() is UTF-8 itself
    static const UINT codePage = []() {
        DWORD value = 0;
        const int got = GetLocaleInfoEx(LOCALE_NAME_SYSTEM_DEFAULT, LOCALE_IDEFAULTCODEPAGE | LOCALE_RETURN_NUMBER,
                                        reinterpret_cast<LPWSTR>(&value), sizeof(value) / sizeof(WCHAR));
        return got > 0 && value != 0 ? static_cast<UINT>(value) : 437u;
    }();
    const int length = static_cast<int>(line.size());
    const int wideSize = MultiByteToWideChar(codePage, 0, line.data(), length, nullptr, 0);
    if (wideSize > 0) {
        std::wstring wide(static_cast<size_t>(wideSize), L'\0');
        MultiByteToWideChar(codePage, 0, line.data(), length, wide.data(), wideSize);
        const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideSize, nullptr, 0, nullptr, nullptr);
        if (size > 0) {
            std::string utf8(static_cast<size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideSize, utf8.data(), size, nullptr, nullptr);
            return utf8;
        }
    }
#endif
    return StringUtils::repairUtf8(line);
}
