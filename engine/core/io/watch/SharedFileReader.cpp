// Copyright (c) 2026 Gamecrafters Guild Limited and contributors (OpenEngine, MIT)
// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// From OpenEngine's Engine/Modules/AssetCore/Source/SharedFileRead.cpp (NOTICE.md).

#include "SharedFileReader.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>

using namespace jasperengine;

namespace fs = std::filesystem;

SharedFileReader::SharedFileReader(const fs::path& path) {
    open(path);
}

SharedFileReader::~SharedFileReader() {
    close();
}

SharedFileReader::SharedFileReader(SharedFileReader&& other) noexcept {
    *this = std::move(other);
}

SharedFileReader& SharedFileReader::operator=(SharedFileReader&& other) noexcept {
    if (this != &other) {
        close();
#ifdef _WIN32
        handle = other.handle;
        other.handle = nullptr;
#else
        fd = other.fd;
        other.fd = -1;
#endif
    }
    return *this;
}

#ifdef _WIN32

namespace {

    HANDLE openShared(const wchar_t* path) {
        return CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    }

}

bool SharedFileReader::open(const fs::path& path) {
    close();
    HANDLE h = openShared(path.c_str());
    if (h == INVALID_HANDLE_VALUE) return false;
    handle = h;
    return true;
}

void SharedFileReader::close() {
    if (handle) {
        CloseHandle(static_cast<HANDLE>(handle));
        handle = nullptr;
    }
}

bool SharedFileReader::isOpen() const {
    return handle != nullptr;
}

int64_t SharedFileReader::size() const {
    if (!handle) return -1;
    LARGE_INTEGER value{};
    if (!GetFileSizeEx(static_cast<HANDLE>(handle), &value)) return -1;
    return static_cast<int64_t>(value.QuadPart);
}

bool SharedFileReader::seekTo(uint64_t offset) {
    if (!handle) return false;
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    return SetFilePointerEx(static_cast<HANDLE>(handle), position, nullptr, FILE_BEGIN) != 0;
}

int64_t SharedFileReader::read(void* dst, uint64_t bytes) {
    if (!handle) return -1;
    uint8_t* out = static_cast<uint8_t*>(dst);
    uint64_t total = 0;
    while (total < bytes) {
        const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(bytes - total, 64ull * 1024ull * 1024ull));
        DWORD got = 0;
        if (!::ReadFile(static_cast<HANDLE>(handle), out + total, chunk, &got, nullptr)) return -1;
        if (got == 0) break;
        total += got;
    }
    return static_cast<int64_t>(total);
}

FILE* SharedFileReader::openStream(const char* utf8Path) {
    if (!utf8Path) return nullptr;
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Path, -1, nullptr, 0);
    if (length <= 0) return nullptr;
    std::wstring wide(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Path, -1, wide.data(), length) <= 0) return nullptr;
    HANDLE h = openShared(wide.c_str());
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_RDONLY | _O_BINARY);
    if (descriptor == -1) {
        CloseHandle(h);
        return nullptr;
    }
    FILE* stream = _fdopen(descriptor, "rb");
    if (!stream) _close(descriptor);   // closes the handle too
    return stream;
}

#else

bool SharedFileReader::open(const fs::path& path) {
    close();
    int descriptor;
    do {
        descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) return false;
    fd = descriptor;
    return true;
}

void SharedFileReader::close() {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

bool SharedFileReader::isOpen() const {
    return fd >= 0;
}

int64_t SharedFileReader::size() const {
    if (fd < 0) return -1;
    struct stat st{};
    if (::fstat(fd, &st) != 0) return -1;
    return static_cast<int64_t>(st.st_size);
}

bool SharedFileReader::seekTo(uint64_t offset) {
    if (fd < 0) return false;
    return ::lseek(fd, static_cast<off_t>(offset), SEEK_SET) != static_cast<off_t>(-1);
}

int64_t SharedFileReader::read(void* dst, uint64_t bytes) {
    if (fd < 0) return -1;
    uint8_t* out = static_cast<uint8_t*>(dst);
    uint64_t total = 0;
    while (total < bytes) {
        const size_t chunk = static_cast<size_t>(std::min<uint64_t>(bytes - total, 64ull * 1024ull * 1024ull));
        const ssize_t got = ::read(fd, out + total, chunk);
        if (got < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (got == 0) break;
        total += static_cast<uint64_t>(got);
    }
    return static_cast<int64_t>(total);
}

FILE* SharedFileReader::openStream(const char* utf8Path) {
    // POSIX opens never block a rename over the file
    return utf8Path ? std::fopen(utf8Path, "rb") : nullptr;
}

#endif

bool SharedFileReader::readAll(const fs::path& path, std::vector<uint8_t>& out) {
    out.clear();
    SharedFileReader reader;
    if (!reader.open(path)) return false;
    const int64_t size = reader.size();
    if (size < 0) return false;
    out.resize(static_cast<size_t>(size));
    if (size == 0) return true;
    const int64_t got = reader.read(out.data(), static_cast<uint64_t>(size));
    if (got < 0) {
        out.clear();
        return false;
    }
    out.resize(static_cast<size_t>(got));
    return true;
}

bool SharedFileReader::readAll(const fs::path& path, std::string& out) {
    std::vector<uint8_t> bytes;
    if (!readAll(path, bytes)) {
        out.clear();
        return false;
    }
    out.assign(bytes.begin(), bytes.end());
    return true;
}
