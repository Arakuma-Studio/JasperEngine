// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ATOMICFILE_H
#define ATOMICFILE_H

// Crash-safe file replacement: every persistent write of the editor and the runtime goes
// through writeFileAtomic() or publishFile() (docs/specs/S03-data-safety.md, docs/data-safety.md).
//
// The bytes are written to a hidden temporary sibling of the target
// (".<name>.<pid>-<counter>.je-tmp"), flushed, and renamed over the target. A rename within
// one directory is atomic, so after any failure, crash or power loss the target holds either
// its previous bytes or the new ones, never a mix or a truncation. A failed replace never
// falls back to copying over the target.

#include "Export.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace jasperengine::io {

    enum class Durability {
        // Regenerable output (caches, thumbnails, export files): atomic, but never synced
        Cache,
        // User data (scenes, settings): the temp is flushed to the disk before it is
        // published, and on Linux/Android the directory entry too
        Document
    };

    enum class WriteError {
        None,
        // skipIfUnchanged and the target already held the bytes: nothing was touched
        Unchanged,
        CreateDirectory,
        // createDirectories is false and the target's folder does not exist: nothing was touched
        MissingDirectory,
        OpenTemp,
        WriteTemp,
        SyncTemp,
        // The target exists without write permission (a VCS lockable file that is not
        // locked, a read-only checkout): nothing was touched
        ReadOnlyTarget,
        Replace,
        Cancelled
    };

    struct AtomicWriteOptions {
        Durability durability = Durability::Document;
        // Compare first; when the target already holds the bytes, return Unchanged without
        // touching the file or its modification time
        bool skipIfUnchanged = false;
        // Create the target's missing parent directories (the default). False for a write that
        // must land in an existing folder (a file created into a folder the user picked): a
        // missing one fails with MissingDirectory instead of being created
        bool createDirectories = true;
        // The publish rename is retried while another process holds the target open
        // (antivirus, indexers, an editor on Windows); attempts * retryDelay bounds the wait
        int attempts = 5;
        std::chrono::milliseconds retryDelay{200};
        // Polled before every rename attempt; true stops the write with Cancelled
        std::function<bool()> shouldCancel;
    };

    struct WriteResult {
        WriteError error = WriteError::None;
        std::error_code code;
        // "<step> '<path>': <system message>", empty on success
        std::string message;
        // Rename attempts made (0 when the write stopped before publishing)
        int attempts = 0;

        bool ok() const { return error == WriteError::None || error == WriteError::Unchanged; }
        bool written() const { return error == WriteError::None; }
    };

    // Writes `bytes` to `target` atomically. Parent directories are created (unless
    // options.createDirectories is false). On success the
    // target holds exactly the new bytes; on any failure it holds exactly its previous bytes
    // (or is still absent) and the temporary sibling is removed. An existing target keeps its
    // permission bits, and a symbolic link keeps pointing at the (rewritten) file it named.
    JASPERENGINE_API WriteResult writeFileAtomic(const std::filesystem::path& target, std::string_view bytes,
            const AtomicWriteOptions& options = {});

    // Publishes a complete temporary file that the caller wrote itself (a stream writer):
    // the same read-only refusal, mode preservation, sync and retry rules as writeFileAtomic.
    // The temp should come from makeTemporarySiblingPath(target). It is removed on failure.
    JASPERENGINE_API WriteResult publishFile(const std::filesystem::path& temp, const std::filesystem::path& target,
            const AtomicWriteOptions& options = {});

    // A file written in pieces, for output too large to hold in memory (writeFileAtomic needs
    // all the bytes at once). The pieces go to a temporary sibling of the target, and commit()
    // publishes it under writeFileAtomic's rules. Without a commit the temporary is removed
    // when the writer goes away, so an abandoned or failed write leaves the target untouched.
    class JASPERENGINE_API AtomicFileWriter {
    public:
        AtomicFileWriter() = default;
        ~AtomicFileWriter();
        AtomicFileWriter(const AtomicFileWriter&) = delete;
        AtomicFileWriter& operator=(const AtomicFileWriter&) = delete;

        // Creates the temporary (and the target's folder, per options). False: see result()
        bool open(const std::filesystem::path& target, const AtomicWriteOptions& options = {});
        // Appends; the first failure sticks and every later piece is dropped
        bool write(const void* data, size_t size);
        // Publishes the pieces. The writer is finished afterwards, whatever the outcome
        WriteResult commit();
        // Removes the temporary and finishes the writer
        void discard();

        // The file being written, readable while the writer is open (a working file a stream
        // reads back): the pieces reach it as write() returns
        const std::filesystem::path& temporaryPath() const { return temp; }
        // Open, write and commit failures, none while everything went well
        const WriteResult& result() const { return status; }

    private:
        void closeHandle();

        std::filesystem::path realTarget;
        std::filesystem::path temp;
        AtomicWriteOptions writeOptions;
        WriteResult status;
        // A file descriptor, or a Win32 HANDLE; -1 when closed
        intptr_t handle = -1;
    };

    // Renames (moves) with the publish retry policy: a sharing violation, a permission error
    // or a busy target is retried; anything else (missing source, cross-device) fails at once.
    // For moves and renames of user files; the Document/Cache durability is ignored.
    JASPERENGINE_API bool renameWithRetry(const std::filesystem::path& from, const std::filesystem::path& to,
            const AtomicWriteOptions& options, std::error_code& error);
    JASPERENGINE_API bool renameWithRetry(const std::filesystem::path& from, const std::filesystem::path& to,
            std::error_code& error);

    // ".<filename>.<pid>-<counter>.je-tmp" next to the target (the counter is process-wide).
    // The leading dot hides it from the Project window and the asset database.
    JASPERENGINE_API std::filesystem::path makeTemporarySiblingPath(const std::filesystem::path& target);
    // True for a name makeTemporarySiblingPath() produces (file watchers ignore these)
    JASPERENGINE_API bool isTemporarySiblingName(std::string_view filename);
    // The writing process of a temporary sibling, -1 when the name is not one
    JASPERENGINE_API int64_t temporarySiblingWriter(std::string_view filename);
    // The target a temporary sibling was going to replace (same directory), empty when the
    // name is not one
    JASPERENGINE_API std::filesystem::path temporarySiblingTarget(const std::filesystem::path& temp);

    // The user-facing reason of a failed write, e.g. "the file is read-only"
    JASPERENGINE_API std::string describeWriteError(const WriteResult& result);
    JASPERENGINE_API const char* writeErrorName(WriteError error);

    JASPERENGINE_API int64_t currentProcessId();
    // False for a process that has exited. Single-process platforms (the browser) know only
    // themselves: every other pid counts as gone.
    JASPERENGINE_API bool isProcessRunning(int64_t pid);

    struct OrphanSweep {
        // Where each non-empty orphan was moved
        std::vector<std::filesystem::path> quarantined;
        // Empty orphans, deleted
        std::vector<std::filesystem::path> removed;
    };

    // Temporary siblings left by a process that is gone (a crash between write and rename)
    // are moved under `quarantineRoot`, keeping their target's path relative to `relativeTo`
    // plus a timestamp (".../Level1.scene.20261023-143205"): an orphan can be the newest
    // complete copy of a document. Empty ones are deleted; names that are not temporary
    // siblings, and temps of running processes, are left alone. The candidates come from a
    // walk the caller already does; this function never walks a directory tree.
    JASPERENGINE_API OrphanSweep quarantineOrphanedTemporaryFiles(const std::vector<std::filesystem::path>& candidates,
            const std::filesystem::path& quarantineRoot, const std::filesystem::path& relativeTo = {});
    // Deletes quarantined files older than maxAge (their quarantine time); returns how many
    JASPERENGINE_API size_t pruneQuarantine(const std::filesystem::path& quarantineRoot,
            std::chrono::hours maxAge = std::chrono::hours(24 * 7));

    // Test seam: called before each step of a write with "open", "write" (before every chunk of
    // at most kWriteChunkSize bytes), "sync" or "rename" (before every attempt) and the path
    // involved; a non-empty error code makes that step fail with it. Empty function: no-op.
    using AtomicFileFaultInjector = std::function<std::error_code(std::string_view step, const std::filesystem::path& path)>;
    JASPERENGINE_API void setAtomicFileFaultInjector(AtomicFileFaultInjector injector);
    constexpr size_t kWriteChunkSize = 64 * 1024;

    // Called after every successful publish with the target, its size and its new mtime. For
    // the file watcher's self-write ledger (S05) only: nothing else may subscribe.
    using PublishListener = std::function<void(const std::filesystem::path& target, uintmax_t size,
            std::filesystem::file_time_type mtime)>;
    JASPERENGINE_API void setPublishListener(PublishListener listener);

    // Called before each publish replaces (or creates) the target, while its old bytes are
    // still there. For the file watcher (S05) only, like the publish listener: an edit of
    // another program it has not delivered yet is delivered before the editor's bytes replace
    // it (they would look like the echo of the editor's write).
    using PrePublishListener = std::function<void(const std::filesystem::path& target)>;
    JASPERENGINE_API void setPrePublishListener(PrePublishListener listener);

}

#endif //ATOMICFILE_H
