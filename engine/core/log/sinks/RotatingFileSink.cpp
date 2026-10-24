// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "log/LogSinks.h"
#include "log/Backtrace.h"
#include "io/AtomicFile.h"

#include <cmath>
#include <filesystem>
#include <cstdio>
#include <string>

#if defined(_WIN32)
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <cerrno>
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

using namespace jasperengine;

namespace je_file_sink {}
using namespace je_file_sink;

namespace je_file_sink {

#if defined(_WIN32)
    std::wstring widen(const std::string& text) {
        if (text.empty()) return std::wstring();
        const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring wide(static_cast<size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &wide[0], length);
        return wide;
    }
#endif

    bool fileExists(const std::string& path) {
#if defined(_WIN32)
        return GetFileAttributesW(widen(path).c_str()) != INVALID_FILE_ATTRIBUTES;
#else
        struct stat info{};
        return ::stat(path.c_str(), &info) == 0;
#endif
    }

    void removeFile(const std::string& path) {
#if defined(_WIN32)
        DeleteFileW(widen(path).c_str());
#else
        ::unlink(path.c_str());
#endif
    }

    // with the retries of io/AtomicFile (a reader holding the file open on Windows)
    bool osRename(const std::string& from, const std::string& to) {
        std::error_code error;
        return io::renameWithRetry(std::filesystem::path(std::u8string(from.begin(), from.end())),
                                   std::filesystem::path(std::u8string(to.begin(), to.end())), error);
    }

}

RotatingFileSink::RotatingFileSink(Options options): options(std::move(options)) {
    showSource.store(this->options.showSource, std::memory_order_relaxed);
    if (!this->options.rename) this->options.rename = osRename;
    if (this->options.maxFiles < 1) this->options.maxFiles = 1;
    pending.reserve(64 * 1024);
    if (!openFile()) return;
    // a file past the size from earlier sessions moves aside first
    if (fileBytes > this->options.maxBytes) rotate();
    if (!this->options.banner.empty()) {
        writeOut(this->options.banner.data(), this->options.banner.size());
    }
}

RotatingFileSink::~RotatingFileSink() {
    flush();
    closeFile();
}

bool RotatingFileSink::isOpen() const {
    return handle != -1;
}

bool RotatingFileSink::openFile() {
#if defined(_WIN32)
    HANDLE file = CreateFileW(widen(options.path).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    fileBytes = static_cast<size_t>(size.QuadPart);
    handle = reinterpret_cast<intptr_t>(file);
#else
    const int fd = ::open(options.path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    struct stat info{};
    fileBytes = ::fstat(fd, &info) == 0 ? static_cast<size_t>(info.st_size) : 0;
    handle = fd;
#endif
    return true;
}

void RotatingFileSink::closeFile() {
    if (handle == -1) return;
#if defined(_WIN32)
    CloseHandle(reinterpret_cast<HANDLE>(handle));
#else
    ::close(static_cast<int>(handle));
#endif
    handle = -1;
}

void RotatingFileSink::writeOut(const char* data, size_t size) {
    if (handle == -1 || size == 0) return;
#if defined(_WIN32)
    while (size > 0) {
        DWORD written = 0;
        if (!WriteFile(reinterpret_cast<HANDLE>(handle), data, static_cast<DWORD>(size), &written, nullptr) || written == 0) break;
        data += written;
        size -= written;
        fileBytes += written;
    }
#else
    while (size > 0) {
        const ssize_t written = ::write(static_cast<int>(handle), data, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) break;
        data += written;
        size -= static_cast<size_t>(written);
        fileBytes += static_cast<size_t>(written);
    }
#endif
}

void RotatingFileSink::rotate() {
    if (rotationOff) return;
    closeFile();
    const std::string& path = options.path;
    bool ok = true;
    const std::string last = path + "." + std::to_string(options.maxFiles);
    if (fileExists(last)) removeFile(last);
    for (int i = options.maxFiles - 1; i >= 1 && ok; i--) {
        const std::string from = path + "." + std::to_string(i);
        if (fileExists(from)) ok = options.rename(from, path + "." + std::to_string(i + 1));
    }
    if (ok) ok = options.rename(path, path + ".1");
    // reopened either way: a new file after a rotation, the same one (appending) after a failure
    if (!ok) rotationOff = true;
    openFile();
    if (!ok) {
        const char notice[] = "--- log rotation failed: rotation is off, this file keeps growing ---\n";
        writeOut(notice, sizeof(notice) - 1);
    }
}

void RotatingFileSink::write(const LogRecord& record) {
    const int64_t second = static_cast<int64_t>(std::floor(record.wallTime));
    if (second != stampSecond) {
        stampSecond = second;
        logFormatTimestamp(record.wallTime, stamp);
    }
    const size_t lineStart = pending.size();
    pending += stamp;
    pending += " [";
    pending += logFileLabel(record);
    pending += "] ";
    pending.append(record.text.data(), record.text.size());
    if (logShowsCategory(*record.category)) {
        pending += " [";
        pending += record.category->name;
        pending += ']';
    }
    if (record.hasSource() && showSource.load(std::memory_order_relaxed)) {
        pending += " (";
        pending.append(record.file.data(), record.file.size());
        pending += ':';
        pending += std::to_string(record.line);
        pending += ')';
    }
    pending += '\n';
    for (uint32_t i = 0; i < record.frameCount; i++) {
        scratch.clear();
        Backtrace::describe(record.frames[i], scratch);
        pending += "  #";
        pending += std::to_string(i);
        pending += ' ';
        pending += scratch;
        pending += '\n';
    }
    // past the size with this line: what came before goes to the full file, the line to a new one
    if (!rotationOff && fileBytes + pending.size() > options.maxBytes && fileBytes + lineStart > 0) {
        writeOut(pending.data(), lineStart);
        rotate();
        pending.erase(0, lineStart);
    }
}

void RotatingFileSink::flush() {
    // one write per batch: "drained" means the lines are with the OS
    writeOut(pending.data(), pending.size());
    pending.clear();
}
