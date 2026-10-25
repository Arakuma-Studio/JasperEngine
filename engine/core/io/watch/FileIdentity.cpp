// Copyright (c) 2026 Gamecrafters Guild Limited and contributors (OpenEngine, MIT)
// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// From OpenEngine's Engine/Modules/FileWatcher/Source/FileIdentity.cpp (NOTICE.md).

#include "FileIdentity.h"

#include <cstdio>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

using namespace jasperengine;

std::string FileIdentity::toString() const {
    if (!valid)
        return {};
#ifdef _WIN32
    char buf[64]{};
    std::snprintf(buf, sizeof(buf), "%08X-%016llX", static_cast<unsigned>(volume), static_cast<unsigned long long>(index));
    return std::string(buf);
#else
    return std::to_string(volume) + "-" + std::to_string(index);
#endif
}

FileIdentity jasperengine::readFileIdentity(const std::filesystem::path& path) {
    FileIdentity identity{};
#ifdef _WIN32
    // FILE_FLAG_BACKUP_SEMANTICS opens directories too
    HANDLE h = CreateFileW(path.wstring().c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return identity;

    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = GetFileInformationByHandle(h, &info) != 0;
    CloseHandle(h);
    if (!ok)
        return identity;

    identity.volume = info.dwVolumeSerialNumber;
    identity.index = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | static_cast<uint64_t>(info.nFileIndexLow);
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return identity;
    identity.volume = static_cast<uint64_t>(st.st_dev);
    identity.index = static_cast<uint64_t>(st.st_ino);
#endif
    identity.valid = true;
    return identity;
}
