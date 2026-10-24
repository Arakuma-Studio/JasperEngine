// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SCOPEDFILELOCK_H
#define SCOPEDFILELOCK_H

#include "Export.h"

#include <filesystem>
#include <string>

namespace jasperengine::io {

    // A cross-process advisory lock on a sidecar file, held for the object's lifetime
    // (flock on POSIX, LockFileEx on Windows). The operating system releases it when the
    // process dies, so a crashed owner never leaves a stale lock behind.
    //
    // The lock file may carry a short text about its owner (writeOwnerInfo); other processes
    // can read it while the lock is held (readOwnerInfo). On Windows the lock covers one byte
    // far past the end of the file for that reason.
    //
    // In the browser (one process per origin) no lock is taken and isLocked() is always true.
    class JASPERENGINE_API ScopedFileLock {
    public:
        enum class Mode { Exclusive, Shared };

        enum class Status {
            Locked,
            // Another process holds it (only with wait == false)
            Busy,
            // The file system has no locks (ENOLCK on network shares, ENOTSUP, a read-only
            // volume): callers may warn and continue unlocked
            Unsupported,
            Failed
        };

        explicit ScopedFileLock(const std::filesystem::path& lockFile, Mode mode = Mode::Exclusive, bool wait = true);
        ~ScopedFileLock();

        ScopedFileLock(const ScopedFileLock&) = delete;
        ScopedFileLock& operator=(const ScopedFileLock&) = delete;

        bool isLocked() const { return status == Status::Locked; }
        Status getStatus() const { return status; }
        // Why acquisition failed, naming the step and the system's error; empty while locked
        const std::string& failureReason() const { return reason; }
        const std::filesystem::path& getPath() const { return path; }

        // Replaces the lock file's text (owner pid, host...). Only while locked.
        bool writeOwnerInfo(const std::string& text);
        // The text a (possibly other) owner wrote, empty when there is none
        static std::string readOwnerInfo(const std::filesystem::path& lockFile);

        // Releases the lock early (the destructor does it otherwise)
        void release();

    private:
        std::filesystem::path path;
        Status status = Status::Failed;
        std::string reason;
#if defined(_WIN32)
        void* handle = nullptr;
#else
        int fd = -1;
#endif
    };

}

#endif //SCOPEDFILELOCK_H
