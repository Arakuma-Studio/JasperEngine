// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Writes, removals and renames that stay beneath a root directory at the moment they happen, and
// the link-resolving path walk the containment checks use (sprite normals outputs, the MCP write
// sandbox).
//
// A check of a path ("this resolves inside the project") and the write after it are two steps: a
// symbolic link (or a Windows junction) planted between them would lead the write elsewhere. The
// *Beneath functions close that gap: they never follow a link at all.
// - POSIX (Linux, macOS): `root` is opened once (it is trusted and may itself be reached through a
//   link), then every folder below it with openat(O_DIRECTORY | O_NOFOLLOW) from the folder before
//   (a missing one made with mkdirat, then opened the same way); the file is a temporary one made
//   with openat(O_CREAT | O_EXCL | O_NOFOLLOW) in the last folder, renamed over the name with
//   renameat on the same folder descriptor.
// - Windows: every folder from `root` down is opened (CreateFileW, FILE_FLAG_OPEN_REPARSE_POINT)
//   and refused when it is a reparse point (a link, a junction); the handles stay open, sharing
//   read and write but not delete, until the rename is done, so no folder on the way can be
//   renamed, removed or swapped for a link meanwhile. The temporary file is made with CREATE_NEW
//   and FILE_FLAG_OPEN_REPARSE_POINT, then MoveFileExW(MOVEFILE_REPLACE_EXISTING) (tried again
//   while a reader briefly holds the file).
//
// Policy for links met by the *Beneath functions:
// - a link (or junction) at a folder on the way is refused: the error names that folder and says
//   to remove the link (or make it a real folder) and try again;
// - a link at the final name is replaced by the new file (the rename swaps the entry; the link's
//   target is never written), and a removal removes the link itself, never its target.
//
// The *ResolvedBeneath functions are what callers that accept links INSIDE the root use (the
// editor's own writes, the MCP tools): the path is resolved first (resolvedPath: links inside the
// root are fine, e.g. an assets folder linked in), refused when that leads outside the root, and
// the resolved location is then written with the *Beneath function; a link planted at a folder of
// the resolved location since is refused as above. A live link at the final name whose target
// resolves inside the root is written through (its target gets the bytes, the link stays); one
// leading outside is refused; a dangling one is replaced (never followed).
//
// Not thread-safe against the same paths written concurrently by this process (two writers of one
// file: the last rename wins, as with any replace-by-rename).
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor::safewrite {

    // `path` (made absolute, lexically normal first) with every symbolic link (and Windows
    // junction) on the way followed, a dangling one too (unlike std::filesystem::weakly_canonical,
    // which stops at the first component that doesn't exist); below a component that doesn't exist
    // the rest stays lexical; then weakly_canonical (8.3 names, case on Windows). Empty when a link
    // loops (more than 40) or can't be read: inside nothing.
    std::filesystem::path resolvedPath(const std::filesystem::path& path);
    // `path` is `dir` or below it, lexically (both lexically normal; a trailing separator ignored)
    bool isAtOrBelow(const std::filesystem::path& path, const std::filesystem::path& dir);
    // `path` is `dir` or below it once both are resolved (resolvedPath): a symbolic link inside
    // `dir` leading elsewhere is not inside; `dir` itself reached through a link is fine
    bool isInsideResolved(const std::filesystem::path& path, const std::filesystem::path& dir);
    // Where a write to `path` lands: its folder resolved (resolvedPath), and the final name too when
    // it is a live link (else as named: a dangling link there is replaced, not followed). Empty
    // when a link loops.
    std::filesystem::path writeLocation(const std::filesystem::path& path);
    // ... a removal or a rename of `path`: its folder resolved, the final name as named (a link
    // there is the entry itself)
    std::filesystem::path entryLocation(const std::filesystem::path& path);

    // ---- beneath `root`, no link followed (see above). `target`: relative to `root`, or absolute
    // and lexically below `root` (or below `root` resolved). False with `error` (names the path and
    // says how to fix it) when it isn't, when a folder on the way is a link, or on an I/O failure;
    // nothing is left behind then (no temporary file; folders made for the write are kept only on
    // success, listed in `createdDirs` as absolute paths below the resolved root, outermost first).

    // Creates or replaces `target` with `bytes` (missing folders made). An existing regular file
    // keeps its permissions; a read-only one is refused (POSIX: no write permission; Windows: the
    // read-only attribute); a folder at the name is refused.
    bool writeFileBeneath(const std::filesystem::path& root, const std::filesystem::path& target, const std::string& bytes,
                          std::string& error, std::vector<std::filesystem::path>* createdDirs = nullptr);
    // Removes the entry at `target` (a file or a link; never a folder). True when nothing is there.
    bool removeFileBeneath(const std::filesystem::path& root, const std::filesystem::path& target, std::string& error);
    // Removes the folder `dir` when it is empty (and a real folder). True when nothing is there or
    // it is not empty (left alone).
    bool removeEmptyDirBeneath(const std::filesystem::path& root, const std::filesystem::path& dir, std::string& error);
    // Makes the folder `dir` (its parent must exist). False when something is there already.
    bool createDirBeneath(const std::filesystem::path& root, const std::filesystem::path& dir, std::string& error);
    // Renames the entry `from` to `to` (both beneath `root`; nothing may be at `to`). A link at
    // `from` is moved as a link.
    bool renameBeneath(const std::filesystem::path& root, const std::filesystem::path& from, const std::filesystem::path& to,
                       std::string& error);

    // ---- resolve first, then beneath (see above)

    // "" when `target` resolves inside `root` for a write (writeLocation) / a removal or rename
    // (entryLocation; `entry` true), else why not ("<target> resolves outside <root>: a symbolic
    // link on its way leads to <where>"). `shown` names the target in the message ("" = as given).
    std::string escapeOf(const std::filesystem::path& root, const std::filesystem::path& target, bool entry = false,
                         const std::string& shown = std::string());
    bool writeFileResolvedBeneath(const std::filesystem::path& root, const std::filesystem::path& target, const std::string& bytes,
                                  std::string& error, std::vector<std::filesystem::path>* createdDirs = nullptr);
    bool removeFileResolvedBeneath(const std::filesystem::path& root, const std::filesystem::path& target, std::string& error);
    bool removeEmptyDirResolvedBeneath(const std::filesystem::path& root, const std::filesystem::path& dir, std::string& error);
    bool createDirResolvedBeneath(const std::filesystem::path& root, const std::filesystem::path& dir, std::string& error);
    bool renameResolvedBeneath(const std::filesystem::path& root, const std::filesystem::path& from, const std::filesystem::path& to,
                               std::string& error);

    // For the file commands (DeleteFileCmd, RenameFileCmd, CopyFileCmd's move, CreateDirCmd):
    // with an empty `root` fs::rename / fs::create_directory as before; else the *ResolvedBeneath
    // function, a refusal or failure thrown as std::filesystem::filesystem_error (what
    // fs::rename throws) with the message
    void renameOrThrow(const std::filesystem::path& root, const std::filesystem::path& from, const std::filesystem::path& to);
    void createDirOrThrow(const std::filesystem::path& root, const std::filesystem::path& dir);

}
