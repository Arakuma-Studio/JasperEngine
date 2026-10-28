// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Writes, removals, renames and copies that stay beneath a root directory at the moment they
// happen, the link-resolving path walk the containment checks use (sprite normals outputs, the
// MCP write sandbox), and the write scope MCP calls run in.
//
// A check of a path ("this resolves inside the project") and the write after it are two steps: a
// symbolic link (or a Windows junction) planted between them would lead the write elsewhere. The
// *Beneath functions close that gap: they never follow a link below the root.
// - POSIX (Linux, macOS): `root` is opened once (it is trusted and may itself be reached through a
//   link), then every folder below it with openat(O_DIRECTORY | O_NOFOLLOW) from the folder before
//   (O_PATH on Linux, O_SEARCH where available: a search-only folder works; a missing one made
//   with mkdirat, then opened the same way). A file is a temporary one made with
//   openat(O_CREAT | O_EXCL | O_NOFOLLOW) in the last folder and renamed over the name with
//   renameat on the same folder descriptor; removals are unlinkat on it; a rename that must not
//   replace uses renameat2(RENAME_NOREPLACE) (Linux) / renameatx_np(RENAME_EXCL) (macOS), else a
//   check then renameat. A tree is removed with openat(O_NOFOLLOW | O_DIRECTORY) + fdopendir +
//   unlinkat, never following a link inside it.
// - Windows: the root is opened by path; every folder below it is opened RELATIVE to the handle
//   of the folder before (NtCreateFile with OBJECT_ATTRIBUTES.RootDirectory and
//   FILE_OPEN_REPARSE_POINT), so nothing below the root is parsed by name again. A folder that is
//   a name-surrogate reparse point (a symbolic link, a junction: IsReparseTagNameSurrogate) is
//   refused; other reparse points (OneDrive / cloud-file placeholders, deduplication...) are
//   ordinary folders and files. The handles stay open (the folders above the last one shared for
//   reading only) until the operation is done. Files and folders are created relative to the held
//   handle; the temporary file stays open and is renamed by its handle
//   (SetFileInformationByHandle, FileRenameInfoEx with RootDirectory = the held folder,
//   REPLACE_IF_EXISTS | POSIX_SEMANTICS; FileRenameInfo on older Windows; tried again while a
//   reader briefly holds the file); removals by handle (FileDispositionInfoEx / FileDispositionInfo).
//   Without SetFileInformationByHandle the temporary file's final path
//   (GetFinalPathNameByHandleW) is checked to be under the root before MoveFileExW.
//
// Policy for links met by the *Beneath functions:
// - a link (or junction) at a folder on the way is refused: the error names that folder and says
//   to remove the link (or make it a real folder) and try again;
// - a link at the final name is replaced by the new file (the rename swaps the entry; the link's
//   target is never written), and a removal removes the link itself, never its target. Windows: a
//   link to a FOLDER at the final name of a file write is refused up front (it can't be replaced
//   by a file).
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

#include <cstdio>
#include <filesystem>
#include <functional>
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
    // The nearest existing folder at or above `path`, resolved ("" when none): the root of a write
    // with no containment that still follows no link planted below it
    std::filesystem::path existingFolderOf(const std::filesystem::path& path);

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
    // Removes the entry at `target` and, when it is a real folder, everything in it (links inside
    // it are removed as links, never followed). True when nothing is there.
    bool removeTreeBeneath(const std::filesystem::path& root, const std::filesystem::path& target, std::string& error);
    // Makes the folder `dir` (its parent must exist). False when something is there already.
    bool createDirBeneath(const std::filesystem::path& root, const std::filesystem::path& dir, std::string& error);
    // Makes `dir` and every missing folder above it (fs::create_directories); true when it is there
    // already as a folder. `createdDirs` as for writeFileBeneath.
    bool createDirectoriesBeneath(const std::filesystem::path& root, const std::filesystem::path& dir, std::string& error,
                                  std::vector<std::filesystem::path>* createdDirs = nullptr);
    // Renames the entry `from` to `to` (both beneath `root`). A link at `from` is moved as a link.
    // `replace` false: nothing may be at `to` (no-replace rename, atomic where the system has one);
    // true: an existing file there is replaced (fs::rename), never a folder.
    bool renameBeneath(const std::filesystem::path& root, const std::filesystem::path& from, const std::filesystem::path& to,
                       std::string& error, bool replace = false);
    // Opens `target` for writing (made when missing, its folders too; `append`: at its end, else
    // emptied). A link at the name is refused. The caller owns the FILE* (fclose).
    std::FILE* openFileBeneath(const std::filesystem::path& root, const std::filesystem::path& target, bool append, std::string& error);

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
    bool removeTreeResolvedBeneath(const std::filesystem::path& root, const std::filesystem::path& target, std::string& error);
    bool createDirResolvedBeneath(const std::filesystem::path& root, const std::filesystem::path& dir, std::string& error);
    bool createDirectoriesResolvedBeneath(const std::filesystem::path& root, const std::filesystem::path& dir, std::string& error,
                                          std::vector<std::filesystem::path>* createdDirs = nullptr);
    bool renameResolvedBeneath(const std::filesystem::path& root, const std::filesystem::path& from, const std::filesystem::path& to,
                               std::string& error, bool replace = false);
    std::FILE* openFileResolvedBeneath(const std::filesystem::path& root, const std::filesystem::path& target, bool append,
                                       std::string& error);
    // Copies `from` (a file, or a folder with everything in it; read wherever links lead: a copy
    // out of a linked-in library is a read) to `to` beneath `root` (folders made with
    // createDirBeneath, files with writeFileBeneath). `overwrite`: existing files at the
    // destination are replaced (fs::copy_options::overwrite_existing). When `to` did not exist,
    // a failure removes what was made of it again.
    bool copyResolvedBeneath(const std::filesystem::path& root, const std::filesystem::path& from, const std::filesystem::path& to,
                             bool overwrite, std::string& error);

    // For the file commands (DeleteFileCmd, RenameFileCmd, CopyFileCmd, CreateDirCmd): with an
    // empty `root` the std::filesystem call as before; else the *ResolvedBeneath function, a
    // refusal or failure thrown as std::filesystem::filesystem_error with the message
    void renameOrThrow(const std::filesystem::path& root, const std::filesystem::path& from, const std::filesystem::path& to);
    void createDirOrThrow(const std::filesystem::path& root, const std::filesystem::path& dir);
    void removeTreeOrThrow(const std::filesystem::path& root, const std::filesystem::path& target);

    // ---- the write scope

    // While a Scope is alive on this thread, writes of paths lexically inside its root go beneath
    // it: FileUtils::writeFileReplacing / writeFile and its createDirectories / removePath /
    // removeAll / renamePath / copyPath helpers (FileUtils::ScopedOps, installed by the first
    // Scope) route to the *ResolvedBeneath functions, and every Command constructed meanwhile
    // records the root (Command::writeRoot), so CommandHistory runs its execute, undo and redo in
    // a Scope of that root again, also when the GUI triggers them. The MCP server opens one around
    // each tool's main-thread run and its continuations (the project directory). Paths outside the
    // root (another project, the editor's settings) and every write outside a Scope keep the plain
    // behaviour. A Scope with an empty root suspends an outer one. Nested Scopes: the innermost wins.
    class Scope {
    public:
        explicit Scope(std::filesystem::path root);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        // The innermost root on this thread ("" when none, or suspended)
        static std::filesystem::path current();
    };
    // The innermost active root holding `path` lexically (its spelling or once resolved), else ""
    // (also "" while the innermost scope is suspended). Every other active root holding the path
    // applies too: a scoped operation is refused when one of them doesn't hold it once resolved.
    std::filesystem::path scopeRootFor(const std::filesystem::path& path);
    // Why the last scoped FileUtils operation on this thread failed ("" when it did not)
    const std::string& lastScopedError();
    // How many scoped FileUtils operations failed on this thread so far (CommandHistory compares
    // it around a step: a write the scope refused inside a command is that step's failure / undo
    // problem, lastScopedError the reason)
    unsigned long long scopedFailureCount();

    // Warnings of the writers (a restore that failed in an undo...): to the editor's console once
    // it installs its sink (stderr until then)
    void setWarningSink(std::function<void(const std::string&)> sink);
    void warn(const std::string& message);

}
