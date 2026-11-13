// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AtomicFile.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>

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
    #include <signal.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace jasperengine::io {

namespace {

    constexpr std::string_view kTempSuffix = ".je-tmp";

    // path's UTF-8 conversions changed type in C++20 (std::u8string, u8path deprecated)
    std::string atomicToUtf8(const fs::path& path){
        const auto utf8 = path.u8string();
        return std::string(utf8.begin(), utf8.end());
    }

    fs::path fromUtf8(const std::string& text){
#ifdef __cpp_lib_char8_t
        return fs::path(std::u8string(text.begin(), text.end()));
#else
        return fs::u8path(text);
#endif
    }

    std::mutex hooksMutex;
    std::shared_ptr<const AtomicFileFaultInjector> faultInjector;
    std::shared_ptr<const PublishListener> publishListener;
    std::shared_ptr<const PrePublishListener> prePublishListener;
    std::atomic<bool> hasFaultInjector{false};
    std::atomic<bool> hasPublishListener{false};
    std::atomic<bool> hasPrePublishListener{false};

    std::error_code injectedFault(std::string_view step, const fs::path& path){
        if (!hasFaultInjector.load(std::memory_order_acquire))
            return {};
        std::shared_ptr<const AtomicFileFaultInjector> injector;
        {
            std::lock_guard<std::mutex> lock(hooksMutex);
            injector = faultInjector;
        }
        return (injector && *injector) ? (*injector)(step, path) : std::error_code();
    }

    void notifyPublished(const fs::path& target){
        if (!hasPublishListener.load(std::memory_order_acquire))
            return;
        std::shared_ptr<const PublishListener> listener;
        {
            std::lock_guard<std::mutex> lock(hooksMutex);
            listener = publishListener;
        }
        if (!listener || !*listener)
            return;
        std::error_code ec;
        const uintmax_t size = fs::file_size(target, ec);
        const fs::file_time_type mtime = fs::last_write_time(target, ec);
        (*listener)(target, ec ? 0 : size, mtime);
    }

    void notifyPrePublish(const fs::path& target){
        if (!hasPrePublishListener.load(std::memory_order_acquire))
            return;
        std::shared_ptr<const PrePublishListener> listener;
        {
            std::lock_guard<std::mutex> lock(hooksMutex);
            listener = prePublishListener;
        }
        if (listener && *listener)
            (*listener)(target);
    }

    std::error_code lastError(){
#if defined(_WIN32)
        return std::error_code((int)::GetLastError(), std::system_category());
#else
        return std::error_code(errno, std::generic_category());
#endif
    }

    WriteResult failure(WriteError error, std::error_code code, std::string_view step, const fs::path& path){
        WriteResult result;
        result.error = error;
        result.code = code;
        result.message = std::string(step) + " '" + atomicToUtf8(path) + "': " + code.message();
        return result;
    }

    void removeQuietly(const fs::path& path){
        std::error_code ec;
        fs::remove(path, ec);
    }

    // The file a chain of symbolic links ends at (possibly not existing yet), so the temp is
    // written beside it and the links survive the rename
    fs::path resolveTarget(const fs::path& target){
        fs::path current = target;
        for (int depth = 0; depth < 40; depth++){
            std::error_code ec;
            if (!fs::is_symlink(fs::symlink_status(current, ec)) || ec)
                return current;
            const fs::path link = fs::read_symlink(current, ec);
            if (ec)
                return current;
            current = link.is_absolute() ? link : current.parent_path() / link;
        }
        return current;
    }

    bool isRetryableRenameError(const std::error_code& ec){
#if defined(_WIN32)
        if (ec.category() == std::system_category()){
            const int value = ec.value();
            if (value == ERROR_SHARING_VIOLATION || value == ERROR_LOCK_VIOLATION || value == ERROR_ACCESS_DENIED)
                return true;
        }
#endif
        return ec == std::errc::permission_denied || ec == std::errc::device_or_resource_busy;
    }

    bool targetExists(const fs::path& target){
        std::error_code ec;
        return fs::exists(fs::symlink_status(target, ec));
    }

    // True when an existing target may not be written (no owner write bit, Windows read-only
    // attribute): VCS lockable files signal "not locked" that way
    bool isReadOnly(const fs::path& target){
#if defined(_WIN32)
        const DWORD attributes = ::GetFileAttributesW(target.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0;
#else
        struct stat st;
        if (::stat(target.c_str(), &st) != 0)
            return false;
        return (st.st_mode & S_IWUSR) == 0;
#endif
    }

    bool holdsBytes(const fs::path& target, std::string_view bytes){
        std::error_code ec;
        const uintmax_t size = fs::file_size(target, ec);
        if (ec || size != bytes.size())
            return false;
        std::ifstream in(target, std::ios::binary);
        if (!in)
            return false;
        std::string current(bytes.size(), '\0');
        in.read(current.data(), (std::streamsize)current.size());
        return (size_t)in.gcount() == bytes.size() && current == bytes;
    }

    // The target's permission bits (and Windows attributes) onto the temp before it replaces it
    void copyModeToTemp(const fs::path& target, const fs::path& temp){
#if defined(_WIN32)
        const DWORD attributes = ::GetFileAttributesW(target.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES){
            const DWORD kept = attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED);
            ::SetFileAttributesW(temp.c_str(), kept ? kept : FILE_ATTRIBUTE_NORMAL);
        }
#else
        struct stat st;
        if (::stat(target.c_str(), &st) == 0)
            ::chmod(temp.c_str(), st.st_mode & 07777);
#endif
    }

    std::error_code syncPath(const fs::path& path){
#if defined(_WIN32)
        HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return lastError();
        std::error_code ec;
        if (!::FlushFileBuffers(handle))
            ec = lastError();
        ::CloseHandle(handle);
        return ec;
#elif defined(__EMSCRIPTEN__)
        (void)path;
        return {};
#else
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0)
            return lastError();
        std::error_code ec;
    #if defined(__APPLE__)
        if (::fcntl(fd, F_FULLFSYNC) != 0 && ::fsync(fd) != 0)
            ec = lastError();
    #else
        if (::fsync(fd) != 0)
            ec = lastError();
    #endif
        ::close(fd);
        return ec;
#endif
    }

    // The new directory entry reaches the disk too (ext4 and friends order data before a
    // rename only for the file itself)
    void syncParentDirectory(const fs::path& target){
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
        const fs::path parent = target.has_parent_path() ? target.parent_path() : fs::path(".");
        const int fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
        if (fd >= 0){
            ::fsync(fd);
            ::close(fd);
        }
#else
        (void)target;
#endif
    }

    // One rename attempt. A publish replaces through RENAME_SWAP on Apple (the swapped-out old
    // file is then unlinked), MoveFileExW write-through on Windows, rename(2) elsewhere.
    std::error_code renameOnce(const fs::path& from, const fs::path& to, bool publish){
#if defined(_WIN32)
        DWORD flags = MOVEFILE_REPLACE_EXISTING;
        if (publish)
            flags |= MOVEFILE_WRITE_THROUGH;
        if (!::MoveFileExW(from.c_str(), to.c_str(), flags))
            return lastError();
        return {};
#else
    #if defined(__APPLE__)
        // never with a directory: APFS swaps a file with one, which would put the directory
        // (and what is in it) at the temporary name and report the write done; rename(2)
        // refuses it (EISDIR), as everywhere else (WriteFileReplacing.AFailedRenameKeepsTheTarget)
        std::error_code statError;
        if (publish && targetExists(to) && !fs::is_directory(fs::symlink_status(to, statError))){
            if (::renamex_np(from.c_str(), to.c_str(), RENAME_SWAP) == 0){
                ::unlink(from.c_str());
                return {};
            }
            // non-APFS volumes (HFS+, exFAT, SMB) do not swap: a plain rename replaces too
            if (errno != ENOTSUP && errno != EINVAL)
                return lastError();
        }
    #else
        (void)publish;
    #endif
        if (::rename(from.c_str(), to.c_str()) != 0)
            return lastError();
        return {};
#endif
    }

    bool renameRetrying(const fs::path& from, const fs::path& to, const AtomicWriteOptions& options,
            bool publish, std::error_code& error, int& attempts, bool& cancelled){
        const int maxAttempts = std::max(1, options.attempts);
        attempts = 0;
        cancelled = false;
        for (int attempt = 0; attempt < maxAttempts; attempt++){
            if (options.shouldCancel && options.shouldCancel()){
                cancelled = true;
                error = std::make_error_code(std::errc::operation_canceled);
                return false;
            }
            if (attempt > 0 && options.retryDelay.count() > 0)
                std::this_thread::sleep_for(options.retryDelay);
            attempts++;
            error = injectedFault("rename", to);
            if (!error)
                error = renameOnce(from, to, publish);
            if (!error)
                return true;
            // A missing source or a move across volumes cannot clear by waiting
            if (!isRetryableRenameError(error))
                return false;
        }
        return false;
    }

    // The rest of a publish once the temp is complete (and synced when synced is true)
    WriteResult publishPrepared(const fs::path& temp, const fs::path& realTarget, const AtomicWriteOptions& options, bool synced){
        const bool exists = targetExists(realTarget);
        if (exists && isReadOnly(realTarget)){
            removeQuietly(temp);
            return failure(WriteError::ReadOnlyTarget, std::make_error_code(std::errc::permission_denied), "replace", realTarget);
        }
        if (exists)
            copyModeToTemp(realTarget, temp);

        if (!synced && options.durability == Durability::Document){
            std::error_code ec = injectedFault("sync", temp);
            if (!ec)
                ec = syncPath(temp);
            if (ec){
                removeQuietly(temp);
                return failure(WriteError::SyncTemp, ec, "sync", temp);
            }
        }

        notifyPrePublish(realTarget);
        std::error_code ec;
        int attempts = 0;
        bool cancelled = false;
        if (!renameRetrying(temp, realTarget, options, true, ec, attempts, cancelled)){
            removeQuietly(temp);
            WriteResult result = failure(cancelled ? WriteError::Cancelled : WriteError::Replace, ec, "replace", realTarget);
            result.attempts = attempts;
            return result;
        }
        if (options.durability == Durability::Document)
            syncParentDirectory(realTarget);

        notifyPublished(realTarget);
        WriteResult result;
        result.attempts = attempts;
        return result;
    }

    // Writes all bytes to a new temp, in chunks the fault injector can interrupt
    WriteResult writeTemp(const fs::path& temp, std::string_view bytes, bool sync, bool& synced){
        synced = false;
        std::error_code ec = injectedFault("open", temp);
        if (ec)
            return failure(WriteError::OpenTemp, ec, "create", temp);

#if defined(_WIN32)
        HANDLE handle = ::CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return failure(WriteError::OpenTemp, lastError(), "create", temp);
        size_t written = 0;
        while (written < bytes.size()){
            const size_t chunk = std::min(kWriteChunkSize, bytes.size() - written);
            ec = injectedFault("write", temp);
            DWORD done = 0;
            if (!ec && !::WriteFile(handle, bytes.data() + written, (DWORD)chunk, &done, nullptr))
                ec = lastError();
            if (!ec && done == 0)
                ec = std::make_error_code(std::errc::io_error);
            if (ec){
                ::CloseHandle(handle);
                removeQuietly(temp);
                return failure(WriteError::WriteTemp, ec, "write", temp);
            }
            written += done;
        }
        if (sync){
            ec = injectedFault("sync", temp);
            if (!ec && !::FlushFileBuffers(handle))
                ec = lastError();
            if (ec){
                ::CloseHandle(handle);
                removeQuietly(temp);
                return failure(WriteError::SyncTemp, ec, "sync", temp);
            }
            synced = true;
        }
        if (!::CloseHandle(handle)){
            ec = lastError();
            removeQuietly(temp);
            return failure(WriteError::WriteTemp, ec, "write", temp);
        }
#else
        int flags = O_WRONLY | O_CREAT | O_EXCL;
    #ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
    #endif
        const int fd = ::open(temp.c_str(), flags, 0666);
        if (fd < 0)
            return failure(WriteError::OpenTemp, lastError(), "create", temp);
        size_t written = 0;
        while (written < bytes.size()){
            const size_t chunk = std::min(kWriteChunkSize, bytes.size() - written);
            ec = injectedFault("write", temp);
            ssize_t done = 0;
            if (!ec){
                do {
                    done = ::write(fd, bytes.data() + written, chunk);
                } while (done < 0 && errno == EINTR);
                if (done < 0)
                    ec = lastError();
                else if (done == 0)
                    ec = std::make_error_code(std::errc::io_error);
            }
            if (ec){
                ::close(fd);
                removeQuietly(temp);
                return failure(WriteError::WriteTemp, ec, "write", temp);
            }
            written += (size_t)done;
        }
        if (sync){
            ec = injectedFault("sync", temp);
    #if defined(__EMSCRIPTEN__)
            // the browser's file system has no disk to reach; the runtime syncs IDBFS itself
    #elif defined(__APPLE__)
            if (!ec && ::fcntl(fd, F_FULLFSYNC) != 0 && ::fsync(fd) != 0)
                ec = lastError();
    #else
            if (!ec && ::fsync(fd) != 0)
                ec = lastError();
    #endif
            if (ec){
                ::close(fd);
                removeQuietly(temp);
                return failure(WriteError::SyncTemp, ec, "sync", temp);
            }
            synced = true;
        }
        if (::close(fd) != 0){
            ec = lastError();
            removeQuietly(temp);
            return failure(WriteError::WriteTemp, ec, "write", temp);
        }
#endif
        return {};
    }

    std::string timestampNow(){
        const std::time_t now = std::time(nullptr);
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        char buffer[32];
        std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &local);
        return buffer;
    }

    // The target's folder: created when missing, or (createDirectories false) required
    WriteResult prepareDirectory(const fs::path& directory, const AtomicWriteOptions& options){
        std::error_code ec;
        if (!options.createDirectories){
            if (!fs::is_directory(directory, ec))
                return failure(WriteError::MissingDirectory, ec ? ec : std::make_error_code(std::errc::no_such_file_or_directory),
                               "open directory", directory);
            return {};
        }
        fs::create_directories(directory, ec);
        if (ec)
            return failure(WriteError::CreateDirectories, ec, "create directory", directory);
        return {};
    }

}

WriteResult writeFileAtomic(const fs::path& target, std::string_view bytes, const AtomicWriteOptions& options){
    const fs::path realTarget = resolveTarget(target);

    if (options.skipIfUnchanged && holdsBytes(realTarget, bytes)){
        WriteResult result;
        result.error = WriteError::Unchanged;
        return result;
    }
    if (targetExists(realTarget) && isReadOnly(realTarget))
        return failure(WriteError::ReadOnlyTarget, std::make_error_code(std::errc::permission_denied), "replace", realTarget);

    if (realTarget.has_parent_path()){
        WriteResult prepared = prepareDirectory(realTarget.parent_path(), options);
        if (!prepared.ok())
            return prepared;
    }

    const fs::path temp = makeTemporarySiblingPath(realTarget);
    bool synced = false;
    WriteResult written = writeTemp(temp, bytes, options.durability == Durability::Document, synced);
    if (!written.ok())
        return written;
    return publishPrepared(temp, realTarget, options, synced);
}

WriteResult publishFile(const fs::path& temp, const fs::path& target, const AtomicWriteOptions& options){
    const fs::path realTarget = resolveTarget(target);
    if (!fs::exists(temp)){
        return failure(WriteError::OpenTemp, std::make_error_code(std::errc::no_such_file_or_directory), "open", temp);
    }
    if (realTarget.has_parent_path()){
        WriteResult prepared = prepareDirectory(realTarget.parent_path(), options);
        if (!prepared.ok()){
            removeQuietly(temp);
            return prepared;
        }
    }
    return publishPrepared(temp, realTarget, options, false);
}

AtomicFileWriter::~AtomicFileWriter(){
    discard();
}

void AtomicFileWriter::closeHandle(){
    if (handle == -1)
        return;
#if defined(_WIN32)
    ::CloseHandle(reinterpret_cast<HANDLE>(handle));
#else
    ::close((int)handle);
#endif
    handle = -1;
}

void AtomicFileWriter::discard(){
    closeHandle();
    if (!temp.empty())
        removeQuietly(temp);
    temp.clear();
}

bool AtomicFileWriter::open(const fs::path& target, const AtomicWriteOptions& options){
    discard();
    status = {};
    writeOptions = options;
    realTarget = resolveTarget(target);
    if (targetExists(realTarget) && isReadOnly(realTarget)){
        status = failure(WriteError::ReadOnlyTarget, std::make_error_code(std::errc::permission_denied), "replace", realTarget);
        return false;
    }
    if (realTarget.has_parent_path()){
        WriteResult prepared = prepareDirectory(realTarget.parent_path(), options);
        if (!prepared.ok()){
            status = prepared;
            return false;
        }
    }
    const fs::path path = makeTemporarySiblingPath(realTarget);
    std::error_code ec = injectedFault("open", path);
    if (ec){
        status = failure(WriteError::OpenTemp, ec, "create", path);
        return false;
    }
#if defined(_WIN32)
    // FILE_SHARE_READ: a working file is read back while this handle writes it
    HANDLE created = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (created == INVALID_HANDLE_VALUE){
        status = failure(WriteError::OpenTemp, lastError(), "create", path);
        return false;
    }
    handle = reinterpret_cast<intptr_t>(created);
#else
    int flags = O_WRONLY | O_CREAT | O_EXCL;
    #ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
    #endif
    const int fd = ::open(path.c_str(), flags, 0666);
    if (fd < 0){
        status = failure(WriteError::OpenTemp, lastError(), "create", path);
        return false;
    }
    handle = fd;
#endif
    temp = path;
    return true;
}

bool AtomicFileWriter::write(const void* data, size_t size){
    if (!status.ok() || handle == -1)
        return false;
    const char* bytes = static_cast<const char*>(data);
    size_t written = 0;
    while (written < size){
        const size_t chunk = std::min(kWriteChunkSize, size - written);
        std::error_code ec = injectedFault("write", temp);
#if defined(_WIN32)
        DWORD done = 0;
        if (!ec && !::WriteFile(reinterpret_cast<HANDLE>(handle), bytes + written, (DWORD)chunk, &done, nullptr))
            ec = lastError();
        if (!ec && done == 0)
            ec = std::make_error_code(std::errc::io_error);
#else
        ssize_t done = 0;
        if (!ec){
            do {
                done = ::write((int)handle, bytes + written, chunk);
            } while (done < 0 && errno == EINTR);
            if (done < 0)
                ec = lastError();
            else if (done == 0)
                ec = std::make_error_code(std::errc::io_error);
        }
#endif
        if (ec){
            status = failure(WriteError::WriteTemp, ec, "write", temp);
            discard();
            return false;
        }
        written += (size_t)done;
    }
    return true;
}

WriteResult AtomicFileWriter::commit(){
    if (handle == -1){
        if (status.ok())
            status = failure(WriteError::OpenTemp, std::make_error_code(std::errc::bad_file_descriptor), "open", realTarget);
        discard();
        return status;
    }
    bool synced = false;
    if (writeOptions.durability == Durability::Document){
        std::error_code ec = injectedFault("sync", temp);
#if defined(_WIN32)
        if (!ec && !::FlushFileBuffers(reinterpret_cast<HANDLE>(handle)))
            ec = lastError();
#elif defined(__APPLE__)
        if (!ec && ::fcntl((int)handle, F_FULLFSYNC) != 0 && ::fsync((int)handle) != 0)
            ec = lastError();
#elif !defined(__EMSCRIPTEN__)
        if (!ec && ::fsync((int)handle) != 0)
            ec = lastError();
#endif
        if (ec){
            status = failure(WriteError::SyncTemp, ec, "sync", temp);
            discard();
            return status;
        }
        synced = true;
    }
#if defined(_WIN32)
    const bool closed = ::CloseHandle(reinterpret_cast<HANDLE>(handle)) != 0;
#else
    const bool closed = ::close((int)handle) == 0;
#endif
    handle = -1;
    if (!closed){
        status = failure(WriteError::WriteTemp, lastError(), "write", temp);
        discard();
        return status;
    }
    // Published, or removed by publishPrepared when it fails
    fs::path published;
    published.swap(temp);
    status = publishPrepared(published, realTarget, writeOptions, synced);
    return status;
}

bool renameWithRetry(const fs::path& from, const fs::path& to, const AtomicWriteOptions& options, std::error_code& error){
    int attempts = 0;
    bool cancelled = false;
    return renameRetrying(from, to, options, false, error, attempts, cancelled);
}

bool renameWithRetry(const fs::path& from, const fs::path& to, std::error_code& error){
    return renameWithRetry(from, to, AtomicWriteOptions{}, error);
}

fs::path makeTemporarySiblingPath(const fs::path& target){
    static std::atomic<uint64_t> counter{0};
    const uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
    fs::path name = fromUtf8(".");
    name += target.filename();
    name += fromUtf8("." + std::to_string(currentProcessId()) + "-" + std::to_string(n) + std::string(kTempSuffix));
    return target.parent_path() / name;
}

int64_t temporarySiblingWriter(std::string_view filename){
    if (filename.size() <= kTempSuffix.size() + 1 || filename.front() != '.')
        return -1;
    if (filename.substr(filename.size() - kTempSuffix.size()) != kTempSuffix)
        return -1;
    // ".<name>.<pid>-<counter>" without the suffix: the last dot starts "<pid>-<counter>"
    const std::string_view stem = filename.substr(0, filename.size() - kTempSuffix.size());
    const size_t dot = stem.rfind('.');
    if (dot == std::string_view::npos || dot < 2)
        return -1;
    const std::string_view id = stem.substr(dot + 1);
    const size_t dash = id.find('-');
    if (dash == std::string_view::npos || dash == 0 || dash + 1 >= id.size())
        return -1;
    int64_t pid = 0;
    for (size_t i = 0; i < dash; i++){
        if (id[i] < '0' || id[i] > '9')
            return -1;
        pid = pid * 10 + (id[i] - '0');
        if (pid > (int64_t(1) << 40))
            return -1;
    }
    for (size_t i = dash + 1; i < id.size(); i++){
        if (id[i] < '0' || id[i] > '9')
            return -1;
    }
    return pid;
}

bool isTemporarySiblingName(std::string_view filename){
    return temporarySiblingWriter(filename) >= 0;
}

fs::path temporarySiblingTarget(const fs::path& temp){
    const std::string name = atomicToUtf8(temp.filename());
    if (!isTemporarySiblingName(name))
        return {};
    const std::string stem = name.substr(0, name.size() - kTempSuffix.size());
    const size_t dot = stem.rfind('.');
    return temp.parent_path() / fromUtf8(stem.substr(1, dot - 1));
}

const char* writeErrorName(WriteError error){
    switch (error){
        case WriteError::None: return "none";
        case WriteError::Unchanged: return "unchanged";
        case WriteError::CreateDirectories: return "create_directory";
        case WriteError::MissingDirectory: return "missing_directory";
        case WriteError::OpenTemp: return "open_temp";
        case WriteError::WriteTemp: return "write_temp";
        case WriteError::SyncTemp: return "sync_temp";
        case WriteError::ReadOnlyTarget: return "read_only_target";
        case WriteError::Replace: return "replace";
        case WriteError::Cancelled: return "cancelled";
    }
    return "unknown";
}

std::string describeWriteError(const WriteResult& result){
    if (result.ok())
        return {};
    if (result.error == WriteError::ReadOnlyTarget)
        return "the file is read-only (is it locked by version control?)";
    if (result.error == WriteError::Cancelled)
        return "the write was cancelled";
    if (result.error == WriteError::MissingDirectory)
        return "its folder does not exist";
    if (result.code == std::errc::no_space_on_device)
        return "the disk is full";
    switch (result.error){
        case WriteError::CreateDirectories: return "could not create its folder (" + result.code.message() + ")";
        case WriteError::OpenTemp: return "could not create a file next to it (" + result.code.message() + ")";
        case WriteError::WriteTemp: return "could not write the data (" + result.code.message() + ")";
        case WriteError::SyncTemp: return "could not flush the data to the disk (" + result.code.message() + ")";
        case WriteError::Replace: return "could not replace the file, another program may be using it (" + result.code.message() + ")";
        default: return result.code.message();
    }
}

int64_t currentProcessId(){
#if defined(_WIN32)
    return (int64_t)::GetCurrentProcessId();
#else
    return (int64_t)::getpid();
#endif
}

bool isProcessRunning(int64_t pid){
    if (pid <= 0)
        return false;
    if (pid == currentProcessId())
        return true;
#if defined(__EMSCRIPTEN__)
    return false;
#elif defined(_WIN32)
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!process)
        return ::GetLastError() == ERROR_ACCESS_DENIED;
    DWORD exitCode = 0;
    const bool running = ::GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE;
    ::CloseHandle(process);
    return running;
#else
    if (::kill((pid_t)pid, 0) == 0)
        return true;
    // another user's process, or one a sandbox hides: it exists
    return errno == EPERM;
#endif
}

OrphanSweep quarantineOrphanedTemporaryFiles(const std::vector<fs::path>& candidates, const fs::path& quarantineRoot, const fs::path& relativeTo){
    OrphanSweep sweep;
    const std::string stamp = timestampNow();
    for (const fs::path& candidate : candidates){
        const int64_t writer = temporarySiblingWriter(atomicToUtf8(candidate.filename()));
        if (writer < 0 || isProcessRunning(writer))
            continue;
        std::error_code ec;
        if (!fs::is_regular_file(fs::symlink_status(candidate, ec)) || ec)
            continue;
        const uintmax_t size = fs::file_size(candidate, ec);
        if (ec)
            continue;
        if (size == 0){
            if (fs::remove(candidate, ec))
                sweep.removed.push_back(candidate);
            continue;
        }

        const fs::path target = temporarySiblingTarget(candidate);
        fs::path relative = target.filename();
        if (!relativeTo.empty()){
            const fs::path rel = target.lexically_relative(relativeTo);
            if (!rel.empty() && *rel.begin() != "..")
                relative = rel;
        }
        fs::path destination = quarantineRoot / relative;
        destination += "." + stamp;
        for (int n = 1; fs::exists(destination, ec) && n < 1000; n++){
            destination = quarantineRoot / relative;
            destination += "." + stamp + "-" + std::to_string(n);
        }
        fs::create_directories(destination.parent_path(), ec);
        if (ec)
            continue;
        if (!renameWithRetry(candidate, destination, ec)){
            // another volume: copy it out, then drop the temp
            ec.clear();
            fs::copy_file(candidate, destination, fs::copy_options::none, ec);
            if (ec)
                continue;
            fs::remove(candidate, ec);
        }
        // the age that pruneQuarantine() measures starts now, not at the crash
        fs::last_write_time(destination, fs::file_time_type::clock::now(), ec);
        sweep.quarantined.push_back(destination);
    }
    return sweep;
}

size_t pruneQuarantine(const fs::path& quarantineRoot, std::chrono::hours maxAge){
    size_t removed = 0;
    std::error_code ec;
    if (!fs::is_directory(quarantineRoot, ec))
        return 0;
    const auto cutoff = fs::file_time_type::clock::now() - maxAge;
    std::vector<fs::path> expired;
    for (fs::recursive_directory_iterator it(quarantineRoot, ec), end; !ec && it != end; it.increment(ec)){
        std::error_code entryEc;
        if (!it->is_regular_file(entryEc))
            continue;
        const auto mtime = it->last_write_time(entryEc);
        if (!entryEc && mtime < cutoff)
            expired.push_back(it->path());
    }
    for (const fs::path& path : expired){
        std::error_code removeEc;
        if (fs::remove(path, removeEc))
            removed++;
    }
    return removed;
}

void setAtomicFileFaultInjector(AtomicFileFaultInjector injector){
    std::lock_guard<std::mutex> lock(hooksMutex);
    faultInjector = injector ? std::make_shared<const AtomicFileFaultInjector>(std::move(injector)) : nullptr;
    hasFaultInjector.store(faultInjector != nullptr, std::memory_order_release);
}

void setPublishListener(PublishListener listener){
    std::lock_guard<std::mutex> lock(hooksMutex);
    publishListener = listener ? std::make_shared<const PublishListener>(std::move(listener)) : nullptr;
    hasPublishListener.store(publishListener != nullptr, std::memory_order_release);
}

void setPrePublishListener(PrePublishListener listener){
    std::lock_guard<std::mutex> lock(hooksMutex);
    prePublishListener = listener ? std::make_shared<const PrePublishListener>(std::move(listener)) : nullptr;
    hasPrePublishListener.store(prePublishListener != nullptr, std::memory_order_release);
}

}
