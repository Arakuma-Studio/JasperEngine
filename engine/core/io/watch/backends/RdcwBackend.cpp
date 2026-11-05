// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Windows: one recursive ReadDirectoryChangesW per root, overlapped, 64 KiB (S05 R17). The
// loop, the OLD/NEW_NAME pairing and the record validation follow OpenEngine's
// FileWatcher::WatchWindows / ProcessWindowsFileChanges (MIT, NOTICE.md).

#include "Backends.h"

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

using namespace jasperengine;

namespace fs = std::filesystem;

namespace {

    // the network share limit for one ReadDirectoryChangesW buffer
    constexpr DWORD BUFFER_SIZE = 64 * 1024;
    constexpr DWORD NOTIFY_FILTER = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
                                    FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_CREATION;
    // an OLD_NAME left at the end of a buffer waits this long for its NEW_NAME
    constexpr DWORD RENAME_HOLD_MS = 200;

    // \\?\ form, so paths over MAX_PATH open
    std::wstring longPathForm(const fs::path& path) {
        std::wstring text = path.lexically_normal().wstring();
        for (wchar_t& c : text) {
            if (c == L'/') c = L'\\';
        }
        if (text.rfind(L"\\\\?\\", 0) == 0) return text;
        if (text.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + text.substr(2);
        return L"\\\\?\\" + text;
    }

    // An 8.3 short name in a record (a component with '~') resolved to the long one
    fs::path longName(const fs::path& path) {
        const std::wstring text = path.wstring();
        if (text.find(L'~') == std::wstring::npos) return path;
        const std::wstring query = longPathForm(path);
        const DWORD needed = GetLongPathNameW(query.c_str(), nullptr, 0);
        if (needed == 0) return path;   // gone (a removal): the short name is all there is
        std::wstring out(needed, L'\0');
        const DWORD written = GetLongPathNameW(query.c_str(), out.data(), needed);
        if (written == 0 || written >= needed) return path;
        out.resize(written);
        if (out.rfind(L"\\\\?\\UNC\\", 0) == 0) out = L"\\\\" + out.substr(8);
        else if (out.rfind(L"\\\\?\\", 0) == 0) out = out.substr(4);
        return fs::path(out).lexically_normal();
    }

    bool isDirectory(const fs::path& path) {
        const DWORD attributes = GetFileAttributesW(longPathForm(path).c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

}

namespace jasperengine::watch {

    class RdcwBackend : public FileWatcher {
    public:
        ~RdcwBackend() override { stop(); }

        bool start(const fs::path& rootIn, WatchSink& sinkIn, std::string& error) override;
        void stop() override;
        bool flush(std::chrono::milliseconds timeout) override;
        bool flushIsDrain() const override { return true; }
        const char* name() const override { return "ReadDirectoryChangesW"; }
        bool isNative() const override { return true; }
        size_t watchCount() const override { return running ? 1 : 0; }
        void injectOverflowForTesting() override;

    private:
        fs::path root;
        WatchSink* sink = nullptr;
        HANDLE directory = INVALID_HANDLE_VALUE;
        HANDLE stopEvent = nullptr;
        OVERLAPPED overlapped{};
        std::vector<BYTE> buffer;
        bool readPending = false;
        std::thread thread;
        std::atomic<bool> running{false};

        // the completion handling (the thread and flush())
        std::mutex readMutex;
        fs::path pendingOldName;
        bool hasPendingOldName = false;
        ULONGLONG pendingOldNameAt = 0;

        bool issueReadLocked();
        // Handles a completed read (if there is one) and issues the next; false: none completed
        bool completeLocked(std::vector<RawEvent>& events);
        void parse(const BYTE* data, DWORD size, std::vector<RawEvent>& events);
        void flushOldNameLocked(bool all, std::vector<RawEvent>& events);
        void deliver(std::vector<RawEvent>&& events);
        void run();
    };

}

using namespace jasperengine::watch;

bool RdcwBackend::start(const fs::path& rootIn, WatchSink& sinkIn, std::string& error) {
    stop();
    root = rootIn.lexically_normal();
    sink = &sinkIn;
    directory = CreateFileW(longPathForm(root).c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (directory == INVALID_HANDLE_VALUE) {
        error = "the project folder cannot be opened for watching (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    overlapped = OVERLAPPED{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    buffer.assign(BUFFER_SIZE, 0);
    if (!overlapped.hEvent || !stopEvent) {
        error = "CreateEvent failed (error " + std::to_string(GetLastError()) + ")";
        stop();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(readMutex);
        if (!issueReadLocked()) {
            error = "ReadDirectoryChangesW failed (error " + std::to_string(GetLastError()) + ")";
            readPending = false;
        }
    }
    if (!readPending) {
        stop();
        return false;
    }
    running = true;
    thread = std::thread([this]() { run(); });
    return true;
}

void RdcwBackend::stop() {
    if (thread.joinable()) {
        running = false;
        SetEvent(stopEvent);
        thread.join();
    }
    running = false;
    std::lock_guard<std::mutex> lock(readMutex);
    if (directory != INVALID_HANDLE_VALUE) {
        if (readPending) {
            CancelIoEx(directory, &overlapped);
            DWORD bytes = 0;
            GetOverlappedResult(directory, &overlapped, &bytes, TRUE);   // the cancelled read must end before its buffer goes
        }
        CloseHandle(directory);
        directory = INVALID_HANDLE_VALUE;
    }
    readPending = false;
    if (overlapped.hEvent) CloseHandle(overlapped.hEvent);
    overlapped = OVERLAPPED{};
    if (stopEvent) CloseHandle(stopEvent);
    stopEvent = nullptr;
    hasPendingOldName = false;
}

bool RdcwBackend::issueReadLocked() {
    HANDLE event = overlapped.hEvent;
    overlapped = OVERLAPPED{};
    overlapped.hEvent = event;
    ResetEvent(event);
    const BOOL ok = ReadDirectoryChangesW(directory, buffer.data(), BUFFER_SIZE, TRUE, NOTIFY_FILTER, nullptr, &overlapped, nullptr);
    readPending = ok || GetLastError() == ERROR_IO_PENDING;
    return readPending;
}

bool RdcwBackend::completeLocked(std::vector<RawEvent>& events) {
    if (!readPending) return false;
    DWORD bytes = 0;
    if (!GetOverlappedResult(directory, &overlapped, &bytes, FALSE)) {
        const DWORD error = GetLastError();
        if (error == ERROR_IO_INCOMPLETE) return false;
        readPending = false;
        if (error == ERROR_OPERATION_ABORTED) return false;
        if (error == ERROR_NOTIFY_ENUM_DIR) {
            // the buffer overflowed: what was lost is found by a rescan (S05 R6)
            events.push_back(RawEvent{RawEvent::Type::Rescan, root, {}, true});
        } else if (error == ERROR_ACCESS_DENIED || error == ERROR_FILE_NOT_FOUND || error == ERROR_NETNAME_DELETED) {
            sink->onBackendFailed("the project folder was moved or deleted");
            return false;
        }
    } else if (bytes == 0) {
        // a zero-byte completion: too many changes to report, same answer
        events.push_back(RawEvent{RawEvent::Type::Rescan, root, {}, true});
    } else {
        parse(buffer.data(), bytes, events);
    }
    if (!issueReadLocked()) sink->onBackendFailed("ReadDirectoryChangesW failed (error " + std::to_string(GetLastError()) + ")");
    return true;
}

void RdcwBackend::parse(const BYTE* data, DWORD size, std::vector<RawEvent>& events) {
    const BYTE* const end = data + size;
    const BYTE* ptr = data;
    constexpr size_t headerSize = offsetof(FILE_NOTIFY_INFORMATION, FileName);

    while (ptr + headerSize <= end) {
        const FILE_NOTIFY_INFORMATION* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(ptr);
        const DWORD nameBytes = info->FileNameLength;
        if ((nameBytes % sizeof(wchar_t)) != 0) break;   // malformed: ignore the rest
        const size_t recordSize = headerSize + static_cast<size_t>(nameBytes);
        if (ptr + recordSize > end) break;

        const std::wstring relative(info->FileName, nameBytes / sizeof(wchar_t));
        const fs::path path = longName((root / fs::path(relative)).lexically_normal());
        const bool excluded = isExcludedPath(root, path);
        const bool probe = !probePath.empty() && path == probePath;

        if (probe) {
            if (info->Action != FILE_ACTION_REMOVED && info->Action != FILE_ACTION_RENAMED_OLD_NAME) sink->onProbe();
        } else if (info->Action == FILE_ACTION_RENAMED_OLD_NAME) {
            // paired with the NEW_NAME that follows; an excluded old name still pairs (a
            // temp file renamed over its target)
            flushOldNameLocked(true, events);
            if (!excluded) {
                pendingOldName = path;
                hasPendingOldName = true;
                pendingOldNameAt = GetTickCount64();
            }
        } else if (info->Action == FILE_ACTION_RENAMED_NEW_NAME) {
            if (!excluded) {
                const bool folder = isDirectory(path);
                if (hasPendingOldName) events.push_back(RawEvent{RawEvent::Type::Moved, path, pendingOldName, folder});
                else events.push_back(RawEvent{RawEvent::Type::Created, path, {}, folder});
            } else if (hasPendingOldName) {
                // renamed to an excluded name (a .trash folder): it went
                events.push_back(RawEvent{RawEvent::Type::Removed, pendingOldName, {}, false});
            }
            hasPendingOldName = false;
        } else if (!excluded) {
            flushOldNameLocked(true, events);
            switch (info->Action) {
                case FILE_ACTION_ADDED:
                    events.push_back(RawEvent{RawEvent::Type::Created, path, {}, isDirectory(path)});
                    break;
                case FILE_ACTION_REMOVED:
                    events.push_back(RawEvent{RawEvent::Type::Removed, path, {}, false});
                    break;
                default:
                    // a folder's own MODIFIED is its entries changing: those come per file
                    if (!isDirectory(path)) events.push_back(RawEvent{RawEvent::Type::Modified, path, {}, false});
                    break;
            }
        }

        const DWORD next = info->NextEntryOffset;
        if (next == 0) break;
        if ((next % 4) != 0 || next < recordSize || ptr + next > end) break;
        ptr += next;
    }
}

void RdcwBackend::flushOldNameLocked(bool all, std::vector<RawEvent>& events) {
    if (!hasPendingOldName) return;
    if (!all && GetTickCount64() - pendingOldNameAt < RENAME_HOLD_MS) return;
    // no NEW_NAME: it left the watched tree
    events.push_back(RawEvent{RawEvent::Type::Removed, pendingOldName, {}, false});
    hasPendingOldName = false;
}

void RdcwBackend::deliver(std::vector<RawEvent>&& events) {
    if (events.empty() || suppressForTesting) return;
    sink->onRawEvents(std::move(events));
}

void RdcwBackend::run() {
    HANDLE handles[2] = {overlapped.hEvent, stopEvent};
    while (running) {
        DWORD timeout = INFINITE;   // idle: no wakeup (S05 R21)
        {
            std::lock_guard<std::mutex> lock(readMutex);
            if (hasPendingOldName) timeout = RENAME_HOLD_MS;
        }
        const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, timeout);
        if (!running || wait == WAIT_OBJECT_0 + 1) break;
        if (wait == WAIT_FAILED) break;
        std::vector<RawEvent> events;
        {
            std::lock_guard<std::mutex> lock(readMutex);
            if (wait == WAIT_OBJECT_0) completeLocked(events);
            flushOldNameLocked(false, events);
            if (!readPending) {
                deliver(std::move(events));
                break;
            }
        }
        deliver(std::move(events));
    }
}

bool RdcwBackend::flush(std::chrono::milliseconds timeout) {
    if (!lockWithin(readMutex, timeout)) return false;
    std::unique_lock<std::mutex> lock(readMutex, std::adopt_lock);
    if (directory == INVALID_HANDLE_VALUE) return false;
    // drain the completions that are there (zero timeout), re-issuing after each (S05 R7)
    std::vector<RawEvent> events;
    for (int i = 0; i < 64 && WaitForSingleObject(overlapped.hEvent, 0) == WAIT_OBJECT_0; i++) {
        if (!completeLocked(events)) break;
    }
    flushOldNameLocked(true, events);
    deliver(std::move(events));
    return true;
}

void RdcwBackend::injectOverflowForTesting() {
    std::lock_guard<std::mutex> lock(readMutex);
    // what completeLocked does with a zero-byte completion
    std::vector<RawEvent> events{RawEvent{RawEvent::Type::Rescan, root, {}, true}};
    sink->onRawEvents(std::move(events));
}

std::unique_ptr<FileWatcher> jasperengine::watch::createRdcwBackend() {
    return std::make_unique<RdcwBackend>();
}

#endif
