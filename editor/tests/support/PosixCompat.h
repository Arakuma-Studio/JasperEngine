// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The POSIX calls the editor tests make for their scratch names and environment (getpid, setenv,
// unsetenv, usleep): <unistd.h>, or the same names over the CRT on Windows, which has no
// <unistd.h>

#ifdef _WIN32

#include <process.h>    // getpid
#include <cstdlib>
#include <cstring>
#include <string>

#include <chrono>
#include <thread>

inline int setenv(const char* name, const char* value, int overwrite) {
    if (!overwrite && std::getenv(name)) return 0;
    return _putenv_s(name, value) == 0 ? 0 : -1;
}

// a path's c_str() is wide on Windows (the name is ASCII)
inline int setenv(const char* name, const wchar_t* value, int overwrite) {
    const std::wstring wide(name, name + std::strlen(name));
    if (!overwrite && _wgetenv(wide.c_str())) return 0;
    return _wputenv_s(wide.c_str(), value) == 0 ? 0 : -1;
}

// (the CRT removes a variable set to "")
inline int unsetenv(const char* name) {
    return _putenv_s(name, "") == 0 ? 0 : -1;
}

inline int usleep(unsigned int microseconds) {
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
    return 0;
}

#else

#include <unistd.h>

#endif
