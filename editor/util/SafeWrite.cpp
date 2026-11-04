// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SafeWrite.h"
#include "buffer/Buffer.h"
#include "htmlui/compat/runtime/DecoratorCommon.h"
#include "io/File.h"
#include "net/CurlApi.h"

#include "util/FileUtils.h"
#include "io/watch/FileWatchService.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <cwctype>
    #include <fcntl.h>
    #include <io.h>
#else
    #include <dirent.h>
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <unistd.h>
    #if defined(__linux__)
        #include <sys/syscall.h>
    #endif
#endif

namespace fs = std::filesystem;

namespace jasperengine::editor::safewrite {

    namespace {

        std::string shown(const fs::path& path) { return FileUtils::pathToGenericUtf8(path); }

        // `path` without a trailing separator, lexically normal
        fs::path normalDir(const fs::path& path) {
            fs::path p = path.lexically_normal();
            if (!p.has_filename() && p.has_parent_path()) p = p.parent_path();
            return p;
        }

        fs::path absoluteNormal(const fs::path& path) {
            std::error_code ec;
            fs::path absolute = path.is_absolute() ? path : fs::absolute(path, ec);
            if (ec) absolute = path;
            return normalDir(absolute);
        }

        bool isLink(const fs::file_status& status) {
            // MSVC reports a directory junction as file_type::junction, not as a symlink
#ifdef _MSC_VER
            return fs::is_symlink(status) || status.type() == fs::file_type::junction;
#else
            return fs::is_symlink(status);
#endif
        }

        std::string linkRefusal(const fs::path& at, const fs::path& root) {
            return shown(at) + " is a symbolic link (or junction): files are written beneath " + shown(root) +
                   " without following links on the way, and this folder is a link now (it was not when the path was checked). "
                   "Remove the link (or make it a real folder), then try again.";
        }

        std::string notAFolder(const fs::path& at) {
            return shown(at) + " is a file, not a folder: a path on the way to the file must be folders.";
        }

        // `target` below `root` as a relative path without "." or ".." (the root itself refused)
        bool relativeBelow(const fs::path& root, const fs::path& realRoot, const fs::path& target, fs::path& rel, std::string& error) {
            rel.clear();
            if (target.is_absolute() || target.has_root_name() || target.has_root_directory()) {
                const fs::path normal = normalDir(target);
                for (const fs::path& base : {normalDir(realRoot), normalDir(root)}) {
                    if (base.empty()) continue;
                    const fs::path r = normal.lexically_relative(base);
                    if (!r.empty() && *r.begin() != ".." && !r.is_absolute()) {
                        rel = r;
                        break;
                    }
                }
            } else {
                rel = target.lexically_normal();
            }
            fs::path clean;
            for (const fs::path& part : rel) {
                if (part.empty() || part == ".") continue;
                if (part == "..") {
                    clean.clear();
                    break;
                }
                clean /= part;
            }
            if (clean.empty()) {
                error = shown(target) + " is not inside " + shown(root) + " (or is that folder itself): name a file inside it.";
                return false;
            }
            rel = clean;
            return true;
        }

        // the trusted roots a project pinned (resolved when it loaded or a root setting was applied)
        std::mutex& pinnedMutex() {
            static std::mutex m;
            return m;
        }
        std::vector<fs::path>& pinnedRoots() {
            static std::vector<fs::path> roots;
            return roots;
        }
        bool isPinned(const fs::path& root) {
            const fs::path normal = normalDir(root);
            std::lock_guard<std::mutex> lock(pinnedMutex());
            return std::find(pinnedRoots().begin(), pinnedRoots().end(), normal) != pinnedRoots().end();
        }

        bool realRootOf(const fs::path& root, fs::path& realRoot, std::string& error) {
            realRoot = resolvedPath(root);
            if (realRoot.empty()) {
                error = shown(root) + " can't be resolved: a symbolic link on its way loops or can't be read.";
                return false;
            }
            // a pinned trusted root is used as it was resolved: one that resolves elsewhere now
            // (moved away, a link put in its place) is refused, never followed
            if (isPinned(root) && normalDir(realRoot) != normalDir(root)) {
                error = shown(root) + " (a trusted root of the project) is no longer the folder the project loaded: it leads to " +
                        shown(realRoot) + " now. Put the folder back (or reopen the project), then try again.";
                return false;
            }
            return true;
        }

        std::atomic<unsigned long long> tempCounter{0};

        // ".<name>.<pid>-<n>.tmp"
        fs::path tempNameFor(const fs::path& name) {
#ifdef _WIN32
            const unsigned long long processId = static_cast<unsigned long long>(GetCurrentProcessId());
#else
            const unsigned long long processId = static_cast<unsigned long long>(::getpid());
#endif
            fs::path temp(".");
            temp += name;
            temp += "." + std::to_string(processId) + "-" + std::to_string(tempCounter++) + ".tmp";
            return temp;
        }

        // What is at a name, seen without following it
        enum class Kind { Missing, File, Dir, Link, LinkToDir, Other };

#ifdef _WIN32

        // ---------------------------------------------------------------- Windows: NT, relative to handles

        std::string systemMessage(DWORD code) { return std::system_category().message(static_cast<int>(code)); }

        bool isMissing(DWORD code) { return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND; }

        // the NT structures, under names of their own (winternl.h differs between MSVC and MinGW)
        struct NtUnicodeString {
            USHORT Length;
            USHORT MaximumLength;
            PWSTR Buffer;
        };
        struct NtObjectAttributes {
            ULONG Length;
            HANDLE RootDirectory;
            NtUnicodeString* ObjectName;
            ULONG Attributes;
            PVOID SecurityDescriptor;
            PVOID SecurityQualityOfService;
        };
        struct NtIoStatusBlock {
            union {
                LONG Status;
                PVOID Pointer;
            };
            ULONG_PTR Information;
        };
        using NtCreateFileFn = LONG(WINAPI*)(PHANDLE, ACCESS_MASK, NtObjectAttributes*, NtIoStatusBlock*, PLARGE_INTEGER, ULONG, ULONG,
                                             ULONG, ULONG, PVOID, ULONG);
        using RtlNtStatusToDosErrorFn = ULONG(WINAPI*)(LONG);
        using GetInfoByHandleExFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
        using SetInfoByHandleFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
        using GetFinalPathFn = DWORD(WINAPI*)(HANDLE, LPWSTR, DWORD, DWORD);

        // NtCreateFile dispositions and options; FILE_INFO_BY_HANDLE_CLASS values
        constexpr ULONG kNtOpen = 1, kNtCreate = 2, kNtOpenIf = 3, kNtOverwriteIf = 5;
        constexpr ULONG kNtDirectoryFile = 0x1, kNtSynchronousIo = 0x20, kNtNonDirectoryFile = 0x40, kNtBackupIntent = 0x4000,
                        kNtOpenReparsePoint = 0x200000;
        constexpr ULONG kObjCaseInsensitive = 0x40;
        constexpr int kFileRenameInfo = 3, kFileDispositionInfo = 4, kFileAttributeTagInfo = 9, kFileFullDirectoryInfo = 14,
                      kFileFullDirectoryRestartInfo = 15, kFileDispositionInfoEx = 21, kFileRenameInfoEx = 22;
        // a name surrogate: the reparse point names another entry (symbolic link, junction)
        constexpr DWORD kNameSurrogateBit = 0x20000000;

        template <typename Fn>
        Fn procAddress(HMODULE module, const char* name) {
            if (!module) return nullptr;
            return reinterpret_cast<Fn>(reinterpret_cast<void (*)()>(GetProcAddress(module, name)));
        }

        struct Api {
            NtCreateFileFn ntCreateFile = nullptr;
            RtlNtStatusToDosErrorFn toDosError = nullptr;
            GetInfoByHandleExFn getInfo = nullptr;
            SetInfoByHandleFn setInfo = nullptr;
            GetFinalPathFn finalPath = nullptr;
        };

        const Api& api() {
            static const Api loaded = [] {
                Api a;
                const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
                const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
                a.ntCreateFile = procAddress<NtCreateFileFn>(ntdll, "NtCreateFile");
                a.toDosError = procAddress<RtlNtStatusToDosErrorFn>(ntdll, "RtlNtStatusToDosError");
                a.getInfo = procAddress<GetInfoByHandleExFn>(kernel, "GetFileInformationByHandleEx");
                a.setInfo = procAddress<SetInfoByHandleFn>(kernel, "SetFileInformationByHandle");
                a.finalPath = procAddress<GetFinalPathFn>(kernel, "GetFinalPathNameByHandleW");
                return a;
            }();
            return loaded;
        }

        // `name` (one component) opened relative to the folder handle `parent`, never following a
        // reparse point at it. 0, or the Win32 error.
        DWORD ntOpen(HANDLE parent, const std::wstring& name, ACCESS_MASK access, ULONG share, ULONG disposition, ULONG options,
                     HANDLE& out) {
            out = INVALID_HANDLE_VALUE;
            const Api& a = api();
            if (!a.ntCreateFile || !a.toDosError) return ERROR_CALL_NOT_IMPLEMENTED;
            NtUnicodeString unicodeName;
            unicodeName.Length = unicodeName.MaximumLength = static_cast<USHORT>(name.size() * sizeof(wchar_t));
            unicodeName.Buffer = const_cast<PWSTR>(name.c_str());
            NtObjectAttributes attributes{sizeof(NtObjectAttributes), parent, &unicodeName, kObjCaseInsensitive, nullptr, nullptr};
            NtIoStatusBlock io{};
            HANDLE handle = nullptr;
            const LONG status = a.ntCreateFile(&handle, access | SYNCHRONIZE, &attributes, &io, nullptr, FILE_ATTRIBUTE_NORMAL, share, disposition,
                                               options | kNtSynchronousIo | kNtBackupIntent | kNtOpenReparsePoint, nullptr, 0);
            if (status < 0) return a.toDosError(status);
            out = handle;
            return 0;
        }

        struct HandleCloser {
            HANDLE h = INVALID_HANDLE_VALUE;
            explicit HandleCloser(HANDLE handle) : h(handle) {}
            HandleCloser(const HandleCloser&) = delete;
            HandleCloser& operator=(const HandleCloser&) = delete;
            ~HandleCloser() {
                if (h != INVALID_HANDLE_VALUE && h != nullptr) CloseHandle(h);
            }
        };

        // What the open handle is: a name surrogate (link, junction) is a link; any other reparse
        // point (cloud files, deduplication...) is what its attributes say
        Kind kindOf(HANDLE handle) {
            struct AttributeTag {
                DWORD FileAttributes;
                DWORD ReparseTag;
            } tag{};
            DWORD attributes = 0;
            bool surrogate = false;
            if (api().getInfo && api().getInfo(handle, kFileAttributeTagInfo, &tag, sizeof(tag))) {
                attributes = tag.FileAttributes;
                surrogate = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) && (tag.ReparseTag & kNameSurrogateBit);
            } else {
                BY_HANDLE_FILE_INFORMATION info;
                if (!GetFileInformationByHandle(handle, &info)) return Kind::Other;
                attributes = info.dwFileAttributes;
                surrogate = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;   // no tag: every reparse point counts
            }
            if (surrogate) return (attributes & FILE_ATTRIBUTE_DIRECTORY) ? Kind::LinkToDir : Kind::Link;
            return (attributes & FILE_ATTRIBUTE_DIRECTORY) ? Kind::Dir : Kind::File;
        }

        bool readOnly(HANDLE handle) {
            BY_HANDLE_FILE_INFORMATION info;
            return GetFileInformationByHandle(handle, &info) && (info.dwFileAttributes & FILE_ATTRIBUTE_READONLY);
        }

        // The entry `name` in `dir`: what it is (Missing when nothing is there)
        Kind childKind(HANDLE dir, const std::wstring& name, DWORD& code, bool* isReadOnly = nullptr) {
            HANDLE h = INVALID_HANDLE_VALUE;
            code = ntOpen(dir, name, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, kNtOpen, 0, h);
            if (code) return isMissing(code) ? Kind::Missing : Kind::Other;
            HandleCloser closer(h);
            if (isReadOnly) *isReadOnly = readOnly(h);
            return kindOf(h);
        }

        // Marks the open handle's entry deleted (POSIX semantics where the system has them)
        bool deleteByHandle(HANDLE handle, DWORD& code) {
            const Api& a = api();
            if (!a.setInfo) {
                code = ERROR_CALL_NOT_IMPLEMENTED;
                return false;
            }
            struct DispositionEx {
                DWORD Flags;
            } ex{0x1 | 0x2};   // FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS
            if (a.setInfo(handle, kFileDispositionInfoEx, &ex, sizeof(ex))) return true;
            code = GetLastError();
            if (code != ERROR_INVALID_PARAMETER && code != ERROR_NOT_SUPPORTED && code != ERROR_INVALID_FUNCTION) return false;
            struct Disposition {
                BOOLEAN DeleteFile;
            } classic{TRUE};
            if (a.setInfo(handle, kFileDispositionInfo, &classic, sizeof(classic))) return true;
            code = GetLastError();
            return false;
        }

        // Renames the open handle's entry to `name` in the folder handle `targetDir`
        bool renameByHandle(HANDLE handle, HANDLE targetDir, const std::wstring& name, bool replace, DWORD& code) {
            const Api& a = api();
            if (!a.setInfo) {
                code = ERROR_CALL_NOT_IMPLEMENTED;
                return false;
            }
            struct RenameInfo {
                union {
                    BOOLEAN ReplaceIfExists;
                    DWORD Flags;
                };
                HANDLE RootDirectory;
                DWORD FileNameLength;
                WCHAR FileName[1];
            };
            std::vector<unsigned char> buffer(sizeof(RenameInfo) + name.size() * sizeof(WCHAR));
            RenameInfo* info = reinterpret_cast<RenameInfo*>(buffer.data());
            info->RootDirectory = targetDir;
            info->FileNameLength = static_cast<DWORD>(name.size() * sizeof(WCHAR));
            std::memcpy(info->FileName, name.data(), name.size() * sizeof(WCHAR));
            // FileRenameInfoEx: FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS
            info->Flags = replace ? (0x1 | 0x2) : 0;
            for (int attempt = 0; attempt < FileUtils::renameAttempts; attempt++) {
                if (attempt > 0) std::this_thread::sleep_for(std::chrono::milliseconds(FileUtils::renameRetryMs));
                if (a.setInfo(handle, kFileRenameInfoEx, info, static_cast<DWORD>(buffer.size()))) return true;
                code = GetLastError();
                if (code == ERROR_INVALID_PARAMETER || code == ERROR_NOT_SUPPORTED || code == ERROR_INVALID_FUNCTION) {
                    // older Windows: FileRenameInfo
                    info->Flags = 0;
                    info->ReplaceIfExists = replace ? TRUE : FALSE;
                    if (a.setInfo(handle, kFileRenameInfo, info, static_cast<DWORD>(buffer.size()))) return true;
                    code = GetLastError();
                    info->Flags = replace ? (0x1 | 0x2) : 0;
                }
                // a file a reader, the indexer or a virus scan briefly holds can't be replaced: tried again
                if (code != ERROR_SHARING_VIOLATION && code != ERROR_ACCESS_DENIED && code != ERROR_LOCK_VIOLATION) return false;
            }
            return false;
        }

        // The final path of an open handle is below `realRoot` (the fallback without handle renames)
        bool finalPathBelow(HANDLE handle, const fs::path& realRoot) {
            if (!api().finalPath) return false;
            std::vector<wchar_t> buffer(32768);
            const DWORD n = api().finalPath(handle, buffer.data(), static_cast<DWORD>(buffer.size()), 0);
            if (n == 0 || n >= buffer.size()) return false;
            std::wstring text(buffer.data(), n);
            if (text.rfind(L"\\\\?\\UNC\\", 0) == 0) text = L"\\\\" + text.substr(8);
            else if (text.rfind(L"\\\\?\\", 0) == 0) text = text.substr(4);
            auto lowered = [](std::wstring s) {
                std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
                return s;
            };
            return isAtOrBelow(fs::path(lowered(text)), fs::path(lowered(normalDir(realRoot).wstring())));
        }

        // Every folder from realRoot down to realRoot/rel opened (relative to the one before) and
        // held until the walk goes; the folders above the last one are shared for reading only
        struct Walk {
            std::vector<HANDLE> handles;
            fs::path at;
            Walk() = default;
            Walk(const Walk&) = delete;
            Walk& operator=(const Walk&) = delete;
            ~Walk() {
                for (HANDLE h : handles) CloseHandle(h);
            }
            HANDLE last() const { return handles.back(); }
        };

        constexpr ACCESS_MASK kWalkAccess = FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES;

        // `shareWrite`: every folder shares writing (a rename between two walks opens the target's
        // folder for adding an entry, which may be an ancestor in the other walk)
        bool walkTo(const fs::path& realRoot, const fs::path& rel, bool create, Walk& walk, std::string& error,
                    std::vector<fs::path>* created, bool* missing = nullptr, bool shareWrite = false) {
            if (missing) *missing = false;
            // the root is trusted (resolved already): opened by its path, following what it is
            // (resolved already: a name surrogate swapped in at its last component since is refused)
            const HANDLE rootHandle = CreateFileW(realRoot.c_str(), kWalkAccess | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (rootHandle == INVALID_HANDLE_VALUE) {
                error = "can't open the folder " + shown(realRoot) + ": " + systemMessage(GetLastError());
                return false;
            }
            {
                const Kind rootKind = kindOf(rootHandle);
                if (rootKind == Kind::Link || rootKind == Kind::LinkToDir) {
                    CloseHandle(rootHandle);
                    error = linkRefusal(realRoot, realRoot);
                    return false;
                }
            }
            walk.handles.push_back(rootHandle);
            walk.at = realRoot;
            const std::vector<fs::path> parts(rel.begin(), rel.end());
            for (size_t i = 0; i < parts.size(); i++) {
                walk.at /= parts[i];
                const std::wstring name = parts[i].wstring();
                // the last folder shares writing too: a rename into it opens it for adding a file
                const ULONG share = shareWrite || i + 1 == parts.size() ? (FILE_SHARE_READ | FILE_SHARE_WRITE) : FILE_SHARE_READ;
                HANDLE h = INVALID_HANDLE_VALUE;
                DWORD code = ntOpen(walk.last(), name, kWalkAccess, share, kNtOpen, 0, h);
                if (code && isMissing(code) && create) {
                    code = ntOpen(walk.last(), name, kWalkAccess, share, kNtCreate, kNtDirectoryFile, h);
                    if (!code) {
                        if (created) created->push_back(walk.at);
                    } else if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) {
                        code = ntOpen(walk.last(), name, kWalkAccess, share, kNtOpen, 0, h);
                    }
                }
                if (code) {
                    if (missing && isMissing(code)) *missing = true;
                    error = (create ? "can't create the folder " : "can't open the folder ") + shown(walk.at) + ": " + systemMessage(code);
                    return false;
                }
                walk.handles.push_back(h);
                const Kind kind = kindOf(h);
                if (kind == Kind::Link || kind == Kind::LinkToDir) {
                    error = linkRefusal(walk.at, realRoot);
                    return false;
                }
                if (kind != Kind::Dir) {
                    error = notAFolder(walk.at);
                    return false;
                }
            }
            return true;
        }

        // A removal of `name` in the walk's last folder: the entry itself; recursing into real
        // folders when `tree`
        bool removeChild(HANDLE dir, const std::wstring& name, const fs::path& shownPath, bool tree, std::string& error, int depth = 0) {
            constexpr ULONG shareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
            HANDLE h = INVALID_HANDLE_VALUE;
            DWORD code = ntOpen(dir, name, DELETE | FILE_READ_ATTRIBUTES, shareAll, kNtOpen, 0, h);
            if (code) {
                if (isMissing(code)) return true;
                error = "can't remove " + shown(shownPath) + ": " + systemMessage(code);
                return false;
            }
            HandleCloser closer(h);
            Kind kind = kindOf(h);
            if (kind == Kind::Dir && tree) {
                // listed through a handle of its own, which must still be that real folder
                CloseHandle(h);
                closer.h = INVALID_HANDLE_VALUE;
                code = ntOpen(dir, name, DELETE | FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES, shareAll, kNtOpen, kNtDirectoryFile, h);
                if (code) {
                    if (isMissing(code)) return true;
                    error = "can't open " + shown(shownPath) + " to remove it: " + systemMessage(code);
                    return false;
                }
                closer.h = h;
                kind = kindOf(h);
            }
            if (kind == Kind::Dir) {
                if (!tree) {
                    error = shown(shownPath) + " is a folder, not a file: it was left alone.";
                    return false;
                }
                if (depth > 256) {
                    error = shown(shownPath) + " is too deep to remove.";
                    return false;
                }
                // the names first, then each removed relative to this handle
                std::vector<std::wstring> names;
                std::vector<unsigned char> buffer(64 * 1024);
                struct FullDirInfo {
                    ULONG NextEntryOffset;
                    ULONG FileIndex;
                    LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime, EndOfFile, AllocationSize;
                    ULONG FileAttributes;
                    ULONG FileNameLength;
                    ULONG EaSize;
                    WCHAR FileName[1];
                };
                bool first = true;
                while (api().getInfo) {
                    if (!api().getInfo(h, first ? kFileFullDirectoryRestartInfo : kFileFullDirectoryInfo, buffer.data(),
                                       static_cast<DWORD>(buffer.size()))) {
                        code = GetLastError();
                        if (code == ERROR_NO_MORE_FILES) break;
                        error = "can't list " + shown(shownPath) + ": " + systemMessage(code);
                        return false;
                    }
                    first = false;
                    size_t offset = 0;
                    while (true) {
                        const FullDirInfo* entry = reinterpret_cast<const FullDirInfo*>(buffer.data() + offset);
                        std::wstring child(entry->FileName, entry->FileNameLength / sizeof(WCHAR));
                        if (child != L"." && child != L"..") names.push_back(std::move(child));
                        if (entry->NextEntryOffset == 0) break;
                        offset += entry->NextEntryOffset;
                    }
                }
                for (const std::wstring& child : names) {
                    if (!removeChild(h, child, shownPath / fs::path(child), true, error, depth + 1)) return false;
                }
            }
            if (!deleteByHandle(h, code)) {
                error = "can't remove " + shown(shownPath) + ": " + systemMessage(code);
                return false;
            }
            return true;
        }

#else

        // ---------------------------------------------------------------- POSIX: openat from folder descriptors

        // A file descriptor closed when it goes
        class Fd {
        public:
            Fd() = default;
            explicit Fd(int fd) : fd(fd) {}
            Fd(const Fd&) = delete;
            Fd& operator=(const Fd&) = delete;
            Fd(Fd&& other) noexcept : fd(other.fd) { other.fd = -1; }
            Fd& operator=(Fd&& other) noexcept {
                if (this != &other) {
                    reset();
                    fd = other.fd;
                    other.fd = -1;
                }
                return *this;
            }
            ~Fd() { reset(); }
            void reset() {
                if (fd >= 0) ::close(fd);
                fd = -1;
            }
            int get() const { return fd; }

        private:
            int fd = -1;
        };

        // Folders on the way are opened for searching only where the system can (a folder the
        // user may search but not list works): O_PATH on Linux, O_SEARCH where it exists
#if defined(__linux__) && defined(O_PATH)
        constexpr int kSearchOnly = O_PATH;
#elif defined(O_SEARCH)
        constexpr int kSearchOnly = O_SEARCH;
#else
        constexpr int kSearchOnly = O_RDONLY;
#endif

        int openFolderAt(int dirfd, const char* name) {
            int fd = ::openat(dirfd, name, kSearchOnly | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            // an O_SEARCH the running system doesn't know
            if (fd < 0 && errno == EINVAL && kSearchOnly != O_RDONLY) fd = ::openat(dirfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            return fd;
        }

        // the root is resolved already (no link in it): its last component opened without
        // following one swapped in since
        int openRootFolder(const fs::path& realRoot) {
            int fd = ::open(realRoot.c_str(), kSearchOnly | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0 && errno == EINVAL && kSearchOnly != O_RDONLY) fd = ::open(realRoot.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            return fd;
        }

        Kind childKind(int dirfd, const char* name, struct stat* out = nullptr) {
            struct stat st;
            if (::fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) return errno == ENOENT ? Kind::Missing : Kind::Other;
            if (out) *out = st;
            if (S_ISLNK(st.st_mode)) return Kind::Link;
            if (S_ISDIR(st.st_mode)) return Kind::Dir;
            return Kind::File;
        }

        struct Walk {
            std::vector<Fd> fds;
            fs::path at;
            int last() const { return fds.back().get(); }
        };

        // realRoot/rel opened folder by folder from realRoot (no link followed below it); `missing`
        // (when not creating): a folder on the way does not exist
        bool walkTo(const fs::path& realRoot, const fs::path& rel, bool create, Walk& walk, std::string& error,
                    std::vector<fs::path>* created, bool* missing = nullptr) {
            if (missing) *missing = false;
            // the root is trusted (resolved already): opened following what it is
            Fd root(openRootFolder(realRoot));
            if (root.get() < 0) {
                error = "can't open the folder " + shown(realRoot) + ": " + std::strerror(errno);
                return false;
            }
            walk.fds.push_back(std::move(root));
            walk.at = realRoot;
            for (const fs::path& part : rel) {
                walk.at /= part;
                int fd = openFolderAt(walk.last(), part.c_str());
                if (fd < 0 && errno == ENOENT && create) {
                    if (::mkdirat(walk.last(), part.c_str(), 0777) == 0) {
                        if (created) created->push_back(walk.at);
                    } else if (errno != EEXIST) {
                        error = "can't create the folder " + shown(walk.at) + ": " + std::strerror(errno);
                        return false;
                    }
                    fd = openFolderAt(walk.last(), part.c_str());
                }
                if (fd < 0) {
                    const int code = errno;
                    const Kind kind = childKind(walk.last(), part.c_str());
                    if (kind == Kind::Link) {
                        error = linkRefusal(walk.at, realRoot);
                    } else if (kind == Kind::File) {
                        error = notAFolder(walk.at);
                    } else {
                        if (missing && code == ENOENT) *missing = true;
                        error = "can't open the folder " + shown(walk.at) + ": " + std::strerror(code);
                    }
                    errno = code;
                    return false;
                }
                walk.fds.emplace_back(fd);
            }
            return true;
        }

        // A rename that must not replace what is at the destination
        int renameNoReplace(int fromDir, const char* from, int toDir, const char* to) {
#if defined(__linux__) && defined(SYS_renameat2)
            const long r = ::syscall(SYS_renameat2, fromDir, from, toDir, to, 1u /* RENAME_NOREPLACE */);
            if (r == 0) return 0;
            if (errno != ENOSYS && errno != EINVAL) return -1;
#elif defined(__APPLE__) && defined(RENAME_EXCL)
            if (::renameatx_np(fromDir, from, toDir, to, RENAME_EXCL) == 0) return 0;
            if (errno != ENOTSUP && errno != EINVAL) return -1;
#endif
            // no atomic no-replace rename here: checked, then renamed
            struct stat st;
            if (::fstatat(toDir, to, &st, AT_SYMLINK_NOFOLLOW) == 0) {
                errno = EEXIST;
                return -1;
            }
            return ::renameat(fromDir, from, toDir, to);
        }

        // The entry `name` in `dirfd` removed (a link as a link); real folders with everything in
        // them when `tree`
        bool removeChild(int dirfd, const char* name, const fs::path& shownPath, bool tree, std::string& error, int depth = 0) {
            const Kind kind = childKind(dirfd, name);
            if (kind == Kind::Missing) return true;
            if (kind == Kind::Dir) {
                if (!tree) {
                    error = shown(shownPath) + " is a folder, not a file: it was left alone.";
                    return false;
                }
                if (depth > 256) {
                    error = shown(shownPath) + " is too deep to remove.";
                    return false;
                }
                const int fd = ::openat(dirfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                if (fd < 0) {
                    if (errno == ENOENT) return true;
                    error = "can't open " + shown(shownPath) + " to remove it: " + std::strerror(errno);
                    return false;
                }
                // fdopendir takes the descriptor; the names first, then each removed relative to it
                DIR* listing = ::fdopendir(fd);
                if (!listing) {
                    ::close(fd);
                    error = "can't list " + shown(shownPath) + ": " + std::strerror(errno);
                    return false;
                }
                std::vector<std::string> names;
                while (const dirent* entry = ::readdir(listing)) {
                    const std::string child = entry->d_name;
                    if (child != "." && child != "..") names.push_back(child);
                }
                bool ok = true;
                for (const std::string& child : names) {
                    if (!removeChild(::dirfd(listing), child.c_str(), shownPath / child, true, error, depth + 1)) {
                        ok = false;
                        break;
                    }
                }
                ::closedir(listing);
                if (!ok) return false;
                if (::unlinkat(dirfd, name, AT_REMOVEDIR) != 0 && errno != ENOENT) {
                    error = "can't remove the folder " + shown(shownPath) + ": " + std::strerror(errno);
                    return false;
                }
                return true;
            }
            if (::unlinkat(dirfd, name, 0) != 0 && errno != ENOENT) {
                error = "can't remove " + shown(shownPath) + ": " + std::strerror(errno);
                return false;
            }
            return true;
        }

#endif

        // The folders made for a write that failed, removed again (deepest first; only empty ones)
        void removeMade(const fs::path& realRoot, std::vector<fs::path>& made) {
            for (auto it = made.rbegin(); it != made.rend(); ++it) {
                std::string ignored;
                removeEmptyDirBeneath(realRoot, *it, ignored);
            }
            made.clear();
        }

        fs::path absoluteIn(const fs::path& root, const fs::path& target) {
            return target.is_absolute() ? normalDir(target) : normalDir(absoluteNormal(root) / target);
        }

        // resolve, refuse an escape, then the rel below the resolved root
        bool prepare(const fs::path& root, const fs::path& target, fs::path& realRoot, fs::path& rel, std::string& error) {
            return realRootOf(root, realRoot, error) && relativeBelow(root, realRoot, target, rel, error);
        }

    }

    // ------------------------------------------------------------------ resolving

    fs::path resolvedPath(const fs::path& path) {
        if (path.empty()) return path;
        const fs::path absolute = absoluteNormal(path);
        // component by component (weakly_canonical stops at the first one that doesn't exist, so
        // a dangling link would stay unresolved): every link on the way is followed (a junction
        // too), a dangling one too, up to kMaxLinks; below a component that doesn't exist the rest
        // is lexical; weakly_canonical last
        constexpr int kMaxLinks = 40;
        std::deque<fs::path> rest;
        for (const fs::path& part : absolute.relative_path()) rest.push_back(part);
        fs::path out = absolute.root_path();
        int links = 0;
        bool lexical = false;
        while (!rest.empty()) {
            const fs::path part = rest.front();
            rest.pop_front();
            if (part.empty() || part == ".") continue;
            if (part == "..") {
                if (out.has_relative_path()) out = out.parent_path();
                continue;
            }
            const fs::path next = out / part;
            if (!lexical) {
                std::error_code statusError;
                const fs::file_status status = fs::symlink_status(next, statusError);
                if (!statusError && isLink(status)) {
                    std::error_code readError;
                    const fs::path target = fs::read_symlink(next, readError);
                    // a loop, or a link that can't be read: nothing it could be inside of
                    if (++links > kMaxLinks || readError) return fs::path();
                    // an absolute target starts again from its root, a relative one from the link's folder
                    if (target.is_absolute()) out = target.root_path();
                    const fs::path parts = target.is_absolute() ? target.relative_path() : target;
                    const std::vector<fs::path> list(parts.begin(), parts.end());
                    for (auto it = list.rbegin(); it != list.rend(); ++it) rest.push_front(*it);
                    continue;
                }
                if (statusError || !fs::exists(status)) lexical = true;
            }
            out = next;
        }
        // and what the walk can't see (Windows: junctions it missed, 8.3 short names, case)
        const fs::path walked = out.lexically_normal();
        std::error_code canonicalError;
        const fs::path canonical = fs::weakly_canonical(walked, canonicalError);
        return canonicalError || canonical.empty() ? walked : canonical.lexically_normal();
    }

#if defined(_WIN32) || defined(__APPLE__)
    namespace {
        // one path component as a case-insensitive file system compares it (Windows, macOS)
        // two path components the way the local file system compares names. Windows: NTFS-style
        // ordinal, case-insensitive (CompareStringOrdinal, the same folding NTFS uses — upper-cases
        // with the invariant table, so no locale surprises). macOS: ASCII case folding (a plain,
        // documented approximation of HFS+/APFS case-insensitive volumes; full Unicode case folding
        // is not applied). Linux never reaches here (case-sensitive).
        bool nameEqual(const fs::path& a, const fs::path& b) {
#ifdef _WIN32
            const std::wstring wa = a.native(), wb = b.native();
            return CompareStringOrdinal(wa.c_str(), static_cast<int>(wa.size()),
                                        wb.c_str(), static_cast<int>(wb.size()), TRUE) == CSTR_EQUAL;
#else
            std::string sa = a.string(), sb = b.string();
            for (char& c : sa) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            for (char& c : sb) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            return sa == sb;
#endif
        }
    }
#endif

    bool isAtOrBelow(const fs::path& path, const fs::path& dir) {
#if defined(_WIN32) || defined(__APPLE__)
        // the file system ignores letter case here: so does the comparison ("Assets" is "assets")
        const fs::path p = normalDir(path), d = normalDir(dir);
        auto pi = p.begin();
        for (auto di = d.begin(); di != d.end(); ++di) {
            if (di->empty()) continue;
            while (pi != p.end() && pi->empty()) ++pi;
            if (pi == p.end() || !nameEqual(*pi, *di)) return false;
            ++pi;
        }
        return true;
#else
        const fs::path rel = normalDir(path).lexically_relative(normalDir(dir));
        return !rel.empty() && *rel.begin() != "..";
#endif
    }

    std::string comparableName(const fs::path& path) {
        fs::path name = path.filename();
#ifdef _WIN32
        // an existing short (8.3) name: its long form
        {
            std::error_code ec;
            if (!path.empty() && fs::exists(fs::symlink_status(path, ec)) && !ec) {
                std::wstring buffer(32768, L'\0');
                const DWORD length = GetLongPathNameW(path.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
                if (length > 0 && length < buffer.size()) name = fs::path(buffer.substr(0, length)).filename();
            }
        }
#endif
        std::string text = FileUtils::pathToUtf8(name);
        if (text == "." || text == "..") return text;
        // an NTFS stream ("name:stream") is part of the file of that name
        if (const size_t colon = text.find(':'); colon != std::string::npos) text.erase(colon);
        // Windows drops trailing dots and spaces ("project.yaml. " is project.yaml)
        while (!text.empty() && (text.back() == '.' || text.back() == ' ')) text.pop_back();
        for (char& c : text) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return text;
    }

    bool isProjectFileName(const fs::path& path) {
        return comparableName(path) == "project.yaml";
    }

    bool isInsideResolved(const fs::path& path, const fs::path& dir) {
        if (path.empty() || dir.empty()) return false;
        const fs::path real = resolvedPath(path);
        const fs::path realDir = resolvedPath(dir);
        return !real.empty() && !realDir.empty() && isAtOrBelow(real, realDir);
    }

    fs::path entryLocation(const fs::path& path) {
        const fs::path absolute = absoluteNormal(path);
        if (!absolute.has_filename()) return resolvedPath(absolute);
        const fs::path parent = resolvedPath(absolute.parent_path());
        if (parent.empty()) return fs::path();
        return parent / absolute.filename();
    }

    fs::path existingFolderOf(const fs::path& path) {
        fs::path dir = absoluteNormal(path);
        std::error_code ec;
        while (!dir.empty() && !fs::is_directory(dir, ec)) {
            if (!dir.has_relative_path()) return fs::path();
            dir = dir.parent_path();
        }
        return dir.empty() ? dir : resolvedPath(dir);
    }

    fs::path writeLocation(const fs::path& path) {
        const fs::path entry = entryLocation(path);
        if (entry.empty()) return entry;
        std::error_code ec;
        if (isLink(fs::symlink_status(entry, ec)) && !ec) {
            // a live link: what it leads to; a dangling one is replaced, not followed
            std::error_code statusError;
            if (fs::exists(fs::status(entry, statusError)) && !statusError) return resolvedPath(entry);
        }
        return entry;
    }

    // ------------------------------------------------------------------ beneath the root
    namespace {
        // What a write puts in the file: bytes in memory, or a stream read in chunks (a copy)
        class ByteSource {
        public:
            virtual ~ByteSource() = default;
            // the next chunk (false at the end, or `failed` when the source could not be read)
            virtual bool next(const char*& data, size_t& size, bool& failed) = 0;
        };
        class StringSource : public ByteSource {
        public:
            explicit StringSource(const std::string& text) : text(text) {}
            bool next(const char*& data, size_t& size, bool&) override {
                if (given) return false;
                given = true;
                data = text.data();
                size = text.size();
                return true;
            }

        private:
            const std::string& text;
            bool given = false;
        };
        class StreamSource : public ByteSource {
        public:
            explicit StreamSource(std::istream& in) : in(in), buffer(1u << 20) {}
            bool next(const char*& data, size_t& size, bool& failed) override {
                in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const std::streamsize n = in.gcount();
                if (in.bad()) {
                    failed = true;
                    return false;
                }
                if (n <= 0) return false;
                data = buffer.data();
                size = static_cast<size_t>(n);
                return true;
            }

        private:
            std::istream& in;
            std::vector<char> buffer;
        };

        bool writeSourceBeneath(const fs::path& root, const fs::path& target, ByteSource& source, std::string& error,
                                std::vector<fs::path>* createdDirs);
        // (the write scope, below) deny-by-default: an owned/hidden place written in an MCP call
        std::string protectedPlaceRefusal(const fs::path& path, bool entry);
        // (the write scope, below) a strict (MCP) scope is in force on this thread
        bool strictScopeActive();
    }

    bool writeFileBeneath(const fs::path& root, const fs::path& target, const std::string& bytes, std::string& error,
                          std::vector<fs::path>* createdDirs) {
        StringSource source(bytes);
        return writeSourceBeneath(root, target, source, error, createdDirs);
    }


    namespace {
    bool writeSourceBeneath(const fs::path& root, const fs::path& target, ByteSource& source, std::string& error,
                            std::vector<fs::path>* createdDirs) {
        if (std::string refusal = protectedPlaceRefusal(absoluteIn(root, target), false); !refusal.empty()) {
            error = std::move(refusal);
            return false;
        }
        fs::path realRoot, rel;
        if (!prepare(root, target, realRoot, rel, error)) return false;
        const fs::path name = rel.filename();
        const fs::path full = realRoot / rel;
        // the editor's own write (S05 R10): announced, what another program left there delivered
        // first, reported once there, so the file watcher knows it for what it is
        ScopedExpectedWrite expected(full);
        FileWatchService::get().reportEditorReplacing(full);
        std::vector<fs::path> made;
        bool ok = false;
#ifdef _WIN32
        {
            Walk walk;
            if (walkTo(realRoot, rel.parent_path(), true, walk, error, &made)) {
                const std::wstring finalName = name.wstring();
                DWORD code = 0;
                bool isReadOnly = false;
                const Kind existing = childKind(walk.last(), finalName, code, &isReadOnly);
                if (existing == Kind::Dir) {
                    error = shown(full) + " is a folder: name a file.";
                } else if (existing == Kind::LinkToDir) {
                    error = shown(full) + " is a link to a folder (or a junction): a file can't replace it. Remove the link, then try again.";
                } else if (existing == Kind::File && isReadOnly) {
                    error = shown(full) + " is read-only: clear its read-only attribute, then try again.";
                } else {
                    HANDLE file = INVALID_HANDLE_VALUE;
                    std::wstring tempName;
                    for (int attempt = 0; attempt < 16 && file == INVALID_HANDLE_VALUE; attempt++) {
                        tempName = tempNameFor(name).wstring();
                        // created relative to the held folder: an entry already at the name (a
                        // planted link) fails the create instead of being followed
                        code = ntOpen(walk.last(), tempName, GENERIC_WRITE | DELETE, 0, kNtCreate, kNtNonDirectoryFile, file);
                        if (code && code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS) break;
                    }
                    if (file == INVALID_HANDLE_VALUE) {
                        error = "can't write " + shown(full) + ": " + systemMessage(code);
                    } else {
                        bool wrote = true;
                        const char* data = nullptr;
                        size_t size = 0;
                        bool readFailed = false;
                        while (wrote && source.next(data, size, readFailed)) {
                            size_t done = 0;
                            while (wrote && done < size) {
                                const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size - done, 1u << 30));
                                DWORD written = 0;
                                wrote = WriteFile(file, data + done, chunk, &written, nullptr) && written > 0;
                                if (!wrote) code = GetLastError();
                                done += written;
                            }
                        }
                        if (readFailed) {
                            wrote = false;
                            code = ERROR_READ_FAULT;
                        }
                        if (wrote) {
                            if (api().setInfo) {
                                // renamed by its handle into the held folder: nothing re-parsed by name
                                wrote = renameByHandle(file, walk.last(), finalName, true, code);
                            } else if (finalPathBelow(file, realRoot)) {
                                // no handle renames (very old Windows): checked, then by path
                                const fs::path tempPath = walk.at / tempName;
                                CloseHandle(file);
                                file = INVALID_HANDLE_VALUE;
                                wrote = MoveFileExW(tempPath.c_str(), (walk.at / finalName).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
                                if (!wrote) {
                                    code = GetLastError();
                                    DeleteFileW(tempPath.c_str());
                                }
                            } else {
                                wrote = false;
                                code = ERROR_ACCESS_DENIED;
                            }
                        }
                        if (!wrote && file != INVALID_HANDLE_VALUE) {
                            DWORD ignored = 0;
                            deleteByHandle(file, ignored);
                        }
                        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
                        if (wrote) ok = true;
                        else error = "can't write " + shown(full) + ": " + systemMessage(code);
                    }
                }
            }
        }
#else
        {
            Walk walk;
            if (walkTo(realRoot, rel.parent_path(), true, walk, error, &made)) {
                struct stat st;
                bool existingFile = false;
                bool refused = false;
                mode_t keep = 0;
                const Kind existing = childKind(walk.last(), name.c_str(), &st);
                if (existing == Kind::Dir) {
                    error = shown(full) + " is a folder: name a file.";
                    errno = EISDIR;
                    refused = true;
                } else if (existing == Kind::File && S_ISREG(st.st_mode)) {
                    existingFile = true;
                    keep = st.st_mode & 07777;
                    // the folder's permissions would let the rename replace a file the user may not write
                    if (::faccessat(walk.last(), name.c_str(), W_OK, 0) != 0) {
                        const int code = errno;
                        error = shown(full) + " is read-only (" + std::strerror(code) + "): make it writable, then try again.";
                        errno = code;
                        refused = true;
                    }
                }
                if (!refused) {
                    fs::path temp;
                    int fd = -1;
                    int code = 0;
                    for (int attempt = 0; attempt < 16 && fd < 0; attempt++) {
                        temp = tempNameFor(name);
                        fd = ::openat(walk.last(), temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
                        if (fd < 0) {
                            code = errno;
                            if (code != EEXIST) break;
                        }
                    }
                    if (fd < 0) {
                        error = "can't write " + shown(full) + ": " + std::strerror(code);
                        errno = code;
                    } else {
                        bool wrote = true;
                        const char* data = nullptr;
                        size_t size = 0;
                        bool readFailed = false;
                        while (wrote && source.next(data, size, readFailed)) {
                            size_t done = 0;
                            while (done < size) {
                                const ssize_t n = ::write(fd, data + done, size - done);
                                if (n < 0) {
                                    if (errno == EINTR) continue;
                                    code = errno;
                                    wrote = false;
                                    break;
                                }
                                done += static_cast<size_t>(n);
                            }
                        }
                        if (readFailed) {
                            wrote = false;
                            code = EIO;
                        }
                        // a replaced file keeps its permissions, but an MCP call never grants (or
                        // keeps) the execute bits: an agent can't turn a file into a program
                        if (strictScopeActive()) keep &= ~static_cast<mode_t>(0111);
                        if (wrote && existingFile) (void)::fchmod(fd, keep);
                        if (::close(fd) != 0 && wrote) {
                            code = errno;
                            wrote = false;
                        }
                        // renameat on the folder's descriptor: a link at the name is replaced, never followed
                        if (wrote && ::renameat(walk.last(), temp.c_str(), walk.last(), name.c_str()) != 0) {
                            code = errno;
                            wrote = false;
                        }
                        if (wrote) {
                            ok = true;
                        } else {
                            ::unlinkat(walk.last(), temp.c_str(), 0);
                            error = "can't write " + shown(full) + ": " + std::strerror(code);
                            errno = code;
                        }
                    }
                }
            }
        }
#endif
        if (!ok) {
            const int code = errno;
            removeMade(realRoot, made);
            errno = code;
            return false;
        }
        for (const fs::path& dir : made) expected.reported(dir);
        expected.reported();
        if (createdDirs) createdDirs->insert(createdDirs->end(), made.begin(), made.end());
        return true;
    }
    }

    namespace {
        // removeFileBeneath / removeTreeBeneath
        bool removeBeneath(const fs::path& root, const fs::path& target, bool tree, std::string& error) {
            fs::path realRoot, rel;
            if (!prepare(root, target, realRoot, rel, error)) return false;
            bool missing = false;
            Walk walk;
            if (!walkTo(realRoot, rel.parent_path(), false, walk, error, nullptr, &missing)) return missing;
            // the editor's own removal (S05 R10), as a write is
            ScopedExpectedWrite expected(realRoot / rel, tree);
#ifdef _WIN32
            const bool removed = removeChild(walk.last(), rel.filename().wstring(), realRoot / rel, tree, error);
#else
            const bool removed = removeChild(walk.last(), rel.filename().c_str(), realRoot / rel, tree, error);
#endif
            if (removed) expected.reportedRemoved(realRoot / rel);
            return removed;
        }
    }

    bool removeFileBeneath(const fs::path& root, const fs::path& target, std::string& error) {
        return removeBeneath(root, target, false, error);
    }

    bool removeTreeBeneath(const fs::path& root, const fs::path& target, std::string& error) {
        return removeBeneath(root, target, true, error);
    }

    static bool removeEmptyDirBeneathUnreported(const fs::path& root, const fs::path& target, std::string& error) {
        fs::path realRoot, rel;
        if (!prepare(root, target, realRoot, rel, error)) return false;
        bool missing = false;
        Walk walk;
        if (!walkTo(realRoot, rel.parent_path(), false, walk, error, nullptr, &missing)) return missing;
#ifdef _WIN32
        HANDLE h = INVALID_HANDLE_VALUE;
        DWORD code = ntOpen(walk.last(), rel.filename().wstring(), DELETE | FILE_READ_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, kNtOpen, 0, h);
        if (code) return true;   // gone, or nothing of ours to remove
        HandleCloser closer(h);
        // a link now, or no folder: left alone
        if (kindOf(h) != Kind::Dir) return true;
        if (!deleteByHandle(h, code)) {
            if (code == ERROR_DIR_NOT_EMPTY || isMissing(code)) return true;
            error = "can't remove the folder " + shown(realRoot / rel) + ": " + systemMessage(code);
            return false;
        }
        return true;
#else
        // gone, a link now, or no folder: nothing of ours to remove
        if (childKind(walk.last(), rel.filename().c_str()) != Kind::Dir) return true;
        if (::unlinkat(walk.last(), rel.filename().c_str(), AT_REMOVEDIR) != 0) {
            if (errno == ENOTEMPTY || errno == EEXIST || errno == ENOENT) return true;
            error = "can't remove the folder " + shown(realRoot / rel) + ": " + std::strerror(errno);
            return false;
        }
        return true;
#endif
    }

    bool removeEmptyDirBeneath(const fs::path& root, const fs::path& target, std::string& error) {
        const fs::path full = absoluteIn(root, target);
        ScopedExpectedWrite expected(full);
        const bool removed = removeEmptyDirBeneathUnreported(root, target, error);
        if (removed) expected.reportedRemoved(full);
        return removed;
    }

    static bool createDirBeneathUnreported(const fs::path& root, const fs::path& target, std::string& error) {
        if (std::string refusal = protectedPlaceRefusal(absoluteIn(root, target), false); !refusal.empty()) {
            error = std::move(refusal);
            return false;
        }
        fs::path realRoot, rel;
        if (!prepare(root, target, realRoot, rel, error)) return false;
        const fs::path full = realRoot / rel;
        Walk walk;
        if (!walkTo(realRoot, rel.parent_path(), false, walk, error, nullptr)) return false;
#ifdef _WIN32
        HANDLE h = INVALID_HANDLE_VALUE;
        const DWORD code = ntOpen(walk.last(), rel.filename().wstring(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, kNtCreate, kNtDirectoryFile, h);
        if (code) {
            error = code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS ? shown(full) + " already exists."
                                                                              : "can't create the folder " + shown(full) + ": " + systemMessage(code);
            return false;
        }
        CloseHandle(h);
        return true;
#else
        if (::mkdirat(walk.last(), rel.filename().c_str(), 0777) != 0) {
            error = errno == EEXIST ? shown(full) + " already exists." : "can't create the folder " + shown(full) + ": " + std::strerror(errno);
            return false;
        }
        return true;
#endif
    }

    bool createDirBeneath(const fs::path& root, const fs::path& target, std::string& error) {
        const fs::path full = absoluteIn(root, target);
        ScopedExpectedWrite expected(full);
        const bool made = createDirBeneathUnreported(root, target, error);
        if (made) expected.reported();
        return made;
    }

    bool createDirectoriesBeneath(const fs::path& root, const fs::path& target, std::string& error, std::vector<fs::path>* createdDirs) {
        std::error_code existing;
        const bool alreadyDir = fs::is_directory(fs::symlink_status(absoluteIn(root, target), existing)) && !existing;
        if (std::string refusal = alreadyDir ? std::string() : protectedPlaceRefusal(absoluteIn(root, target), false); !refusal.empty()) {
            error = std::move(refusal);
            return false;
        }
        fs::path realRoot, rel;
        if (!realRootOf(root, realRoot, error)) return false;
        // the root itself: there already
        if (normalDir(absoluteIn(root, target)) == normalDir(absoluteNormal(root)) || normalDir(absoluteIn(root, target)) == normalDir(realRoot)) {
            return true;
        }
        if (!relativeBelow(root, realRoot, target, rel, error)) return false;
        Walk walk;
        std::vector<fs::path> made;
        if (!walkTo(realRoot, rel, true, walk, error, &made)) {
            removeMade(realRoot, made);
            return false;
        }
        for (const fs::path& dir : made) ScopedExpectedWrite::notePublished(dir);
        if (createdDirs) createdDirs->insert(createdDirs->end(), made.begin(), made.end());
        return true;
    }

    static bool renameBeneathUnreported(const fs::path& root, const fs::path& from, const fs::path& to, std::string& error, bool replace);

    bool renameBeneath(const fs::path& root, const fs::path& from, const fs::path& to, std::string& error, bool replace) {
        const fs::path fullFrom = absoluteIn(root, from), fullTo = absoluteIn(root, to);
        ScopedExpectedWrite expected(fullTo);
        const bool moved = renameBeneathUnreported(root, from, to, error, replace);
        if (moved) expected.reportedMoved(fullFrom, fullTo);
        return moved;
    }

    static bool renameBeneathUnreported(const fs::path& root, const fs::path& from, const fs::path& to, std::string& error, bool replace) {
        // renamed to project.yaml (one moved as it is, to the trash and back, is not a new one)
        if (!isProjectFileName(from)) {
            if (std::string refusal = protectedPlaceRefusal(absoluteIn(root, to), true); !refusal.empty()) {
                error = std::move(refusal);
                return false;
            }
        }
        fs::path realRoot, relFrom, relTo;
        if (!realRootOf(root, realRoot, error) || !relativeBelow(root, realRoot, from, relFrom, error) ||
            !relativeBelow(root, realRoot, to, relTo, error)) {
            return false;
        }
        const fs::path fullFrom = realRoot / relFrom, fullTo = realRoot / relTo;
        Walk fromWalk, toWalk;
#ifdef _WIN32
        const bool walked = walkTo(realRoot, relFrom.parent_path(), false, fromWalk, error, nullptr, nullptr, true) &&
                            walkTo(realRoot, relTo.parent_path(), false, toWalk, error, nullptr, nullptr, true);
#else
        const bool walked = walkTo(realRoot, relFrom.parent_path(), false, fromWalk, error, nullptr) &&
                            walkTo(realRoot, relTo.parent_path(), false, toWalk, error, nullptr);
#endif
        if (!walked) return false;
        const std::string exists = shown(fullTo) + " already exists: delete it first or pick another name.";
#ifdef _WIN32
        DWORD code = 0;
        const Kind there = childKind(toWalk.last(), relTo.filename().wstring(), code);
        if (there != Kind::Missing && (!replace || there == Kind::Dir || there == Kind::LinkToDir)) {
            error = exists;
            return false;
        }
        HANDLE h = INVALID_HANDLE_VALUE;
        code = ntOpen(fromWalk.last(), relFrom.filename().wstring(), DELETE | FILE_READ_ATTRIBUTES,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, kNtOpen, 0, h);
        if (code) {
            error = "can't move " + shown(fullFrom) + ": " + systemMessage(code);
            return false;
        }
        HandleCloser closer(h);
        if (!renameByHandle(h, toWalk.last(), relTo.filename().wstring(), replace, code)) {
            error = code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS ? exists
                                                                              : "can't move " + shown(fullFrom) + " to " + shown(fullTo) + ": " + systemMessage(code);
            return false;
        }
        return true;
#else
        const fs::path fromLeaf = relFrom.filename(), toLeaf = relTo.filename();
        const char* fromName = fromLeaf.c_str();
        const char* toName = toLeaf.c_str();
        if (replace) {
            // fs::rename semantics, but never onto a folder
            if (childKind(toWalk.last(), toName) == Kind::Dir) {
                error = exists;
                return false;
            }
            if (::renameat(fromWalk.last(), fromName, toWalk.last(), toName) != 0) {
                error = "can't move " + shown(fullFrom) + " to " + shown(fullTo) + ": " + std::strerror(errno);
                return false;
            }
            return true;
        }
        if (renameNoReplace(fromWalk.last(), fromName, toWalk.last(), toName) != 0) {
            error = errno == EEXIST ? exists : "can't move " + shown(fullFrom) + " to " + shown(fullTo) + ": " + std::strerror(errno);
            return false;
        }
        return true;
#endif
    }

    std::FILE* openFileBeneath(const fs::path& root, const fs::path& target, bool append, std::string& error) {
        if (std::string refusal = protectedPlaceRefusal(absoluteIn(root, target), false); !refusal.empty()) {
            error = std::move(refusal);
            return nullptr;
        }
        fs::path realRoot, rel;
        if (!prepare(root, target, realRoot, rel, error)) return nullptr;
        const fs::path full = realRoot / rel;
        std::vector<fs::path> made;
        Walk walk;
        if (!walkTo(realRoot, rel.parent_path(), true, walk, error, &made)) {
            removeMade(realRoot, made);
            return nullptr;
        }
#ifdef _WIN32
        DWORD code = 0;
        const Kind existing = childKind(walk.last(), rel.filename().wstring(), code);
        if (existing == Kind::Link || existing == Kind::LinkToDir || existing == Kind::Dir) {
            error = shown(full) + (existing == Kind::Dir ? " is a folder: name a file." : " is a link: a file is not written through one. Remove it, then try again.");
            return nullptr;
        }
        HANDLE h = INVALID_HANDLE_VALUE;
        code = ntOpen(walk.last(), rel.filename().wstring(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      append ? kNtOpenIf : kNtOverwriteIf, kNtNonDirectoryFile, h);
        if (code) {
            error = "can't open " + shown(full) + ": " + systemMessage(code);
            return nullptr;
        }
        if (kindOf(h) != Kind::File) {
            CloseHandle(h);
            error = shown(full) + " is a link: a file is not written through one. Remove it, then try again.";
            return nullptr;
        }
        const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_WRONLY | _O_BINARY | (append ? _O_APPEND : 0));
        if (fd < 0) {
            CloseHandle(h);
            error = "can't open " + shown(full);
            return nullptr;
        }
        std::FILE* file = _fdopen(fd, append ? "ab" : "wb");
        if (!file) {
            _close(fd);
            error = "can't open " + shown(full);
        }
        return file;
#else
        const int fd = ::openat(walk.last(), rel.filename().c_str(), O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC | (append ? O_APPEND : O_TRUNC), 0666);
        if (fd < 0) {
            error = errno == ELOOP ? shown(full) + " is a link: a file is not written through one. Remove it, then try again."
                                   : "can't open " + shown(full) + ": " + std::strerror(errno);
            return nullptr;
        }
        struct stat st;
        if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
            ::close(fd);
            error = shown(full) + " is not a regular file.";
            return nullptr;
        }
        std::FILE* file = ::fdopen(fd, append ? "ab" : "wb");
        if (!file) {
            ::close(fd);
            error = "can't open " + shown(full) + ": " + std::strerror(errno);
        }
        return file;
#endif
    }

    // ------------------------------------------------------------------ resolved, then beneath

    std::string escapeOf(const fs::path& root, const fs::path& target, bool entry, const std::string& shownName) {
        const std::string name = shownName.empty() ? shown(target) : shownName;
        fs::path realRoot;
        std::string rootError;
        if (!realRootOf(root, realRoot, rootError)) return rootError;
        const fs::path absolute = absoluteIn(root, target);
        const fs::path where = entry ? entryLocation(absolute) : writeLocation(absolute);
        if (where.empty()) return name + " can't be resolved: a symbolic link on its way loops or can't be read.";
        if (!isAtOrBelow(where, realRoot)) {
            return name + " resolves outside " + shown(root) + ": a symbolic link on its way leads to " + shown(where) + ".";
        }
        return std::string();
    }

    namespace {
        bool refuseEscape(const fs::path& root, const fs::path& target, bool entry, std::string& error) {
            const std::string escape = escapeOf(root, target, entry);
            if (escape.empty()) return false;
            error = escape + " Remove the link (or use a path that stays inside), then try again.";
            return true;
        }
    }

    bool writeFileResolvedBeneath(const fs::path& root, const fs::path& target, const std::string& bytes, std::string& error,
                                  std::vector<fs::path>* createdDirs) {
        if (refuseEscape(root, target, false, error)) return false;
        return writeFileBeneath(resolvedPath(root), writeLocation(absoluteIn(root, target)), bytes, error, createdDirs);
    }

    bool removeFileResolvedBeneath(const fs::path& root, const fs::path& target, std::string& error) {
        if (refuseEscape(root, target, true, error)) return false;
        return removeFileBeneath(resolvedPath(root), entryLocation(absoluteIn(root, target)), error);
    }

    bool removeEmptyDirResolvedBeneath(const fs::path& root, const fs::path& dir, std::string& error) {
        if (refuseEscape(root, dir, true, error)) return false;
        return removeEmptyDirBeneath(resolvedPath(root), entryLocation(absoluteIn(root, dir)), error);
    }

    bool removeTreeResolvedBeneath(const fs::path& root, const fs::path& target, std::string& error) {
        if (refuseEscape(root, target, true, error)) return false;
        return removeTreeBeneath(resolvedPath(root), entryLocation(absoluteIn(root, target)), error);
    }

    bool createDirResolvedBeneath(const fs::path& root, const fs::path& dir, std::string& error) {
        if (refuseEscape(root, dir, true, error)) return false;
        return createDirBeneath(resolvedPath(root), entryLocation(absoluteIn(root, dir)), error);
    }

    bool createDirectoriesResolvedBeneath(const fs::path& root, const fs::path& dir, std::string& error, std::vector<fs::path>* createdDirs) {
        if (refuseEscape(root, dir, false, error)) return false;
        return createDirectoriesBeneath(resolvedPath(root), writeLocation(absoluteIn(root, dir)), error, createdDirs);
    }

    bool renameResolvedBeneath(const fs::path& root, const fs::path& from, const fs::path& to, std::string& error, bool replace) {
        if (refuseEscape(root, from, true, error) || refuseEscape(root, to, true, error)) return false;
        return renameBeneath(resolvedPath(root), entryLocation(absoluteIn(root, from)), entryLocation(absoluteIn(root, to)), error, replace);
    }

    std::FILE* openFileResolvedBeneath(const fs::path& root, const fs::path& target, bool append, std::string& error) {
        if (refuseEscape(root, target, true, error)) return nullptr;
        return openFileBeneath(resolvedPath(root), entryLocation(absoluteIn(root, target)), append, error);
    }

    namespace {
        bool copyInto(const fs::path& root, const fs::path& from, const fs::path& to, bool overwrite, std::string& error, int depth) {
            std::error_code ec;
            // the source is read wherever its links lead
            if (fs::is_directory(from, ec)) {
                if (depth > 256) {
                    error = shown(from) + " is too deep to copy.";
                    return false;
                }
                std::vector<fs::path> made;
                if (!fs::is_directory(fs::symlink_status(to, ec)) && !createDirectoriesResolvedBeneath(root, to, error, &made)) return false;
                for (const fs::directory_entry& entry : fs::directory_iterator(from, ec)) {
                    if (!copyInto(root, entry.path(), to / entry.path().filename(), overwrite, error, depth + 1)) return false;
                }
                if (ec) {
                    error = "can't list " + shown(from) + ": " + ec.message();
                    return false;
                }
                return true;
            }
            if (!overwrite && fs::exists(fs::symlink_status(to, ec))) {
                error = shown(to) + " already exists.";
                return false;
            }
            // streamed into the temporary file (a large asset is never held in memory whole)
            std::ifstream in(from, std::ios::binary);
            if (!in) {
                error = "can't read " + shown(from) + ".";
                return false;
            }
            std::string refusal;
            if (refuseEscape(root, to, false, refusal)) {
                error = refusal;
                return false;
            }
            StreamSource source(in);
            if (!writeSourceBeneath(resolvedPath(root), writeLocation(absoluteIn(root, to)), source, error, nullptr)) return false;
            return true;
        }
    }

    bool copyResolvedBeneath(const fs::path& root, const fs::path& from, const fs::path& to, bool overwrite, std::string& error) {
        std::error_code ec;
        const bool existed = fs::exists(fs::symlink_status(to, ec));
        if (copyInto(root, from, to, overwrite, error, 0)) return true;
        if (!existed) {
            std::string ignored;
            removeTreeResolvedBeneath(root, to, ignored);
        }
        return false;
    }

    void renameOrThrow(const fs::path& root, const fs::path& from, const fs::path& to) {
        if (root.empty()) {
            fs::rename(from, to);
            return;
        }
        std::string error;
        if (!renameResolvedBeneath(root, from, to, error)) {
            throw fs::filesystem_error(error, from, to, std::make_error_code(std::errc::operation_not_permitted));
        }
    }

    void createDirOrThrow(const fs::path& root, const fs::path& dir) {
        if (root.empty()) {
            fs::create_directory(dir);
            return;
        }
        std::error_code ec;
        if (fs::is_directory(fs::symlink_status(dir, ec))) return;   // there already, as fs::create_directory
        std::string error;
        if (!createDirResolvedBeneath(root, dir, error)) {
            throw fs::filesystem_error(error, dir, std::make_error_code(std::errc::operation_not_permitted));
        }
    }

    void removeTreeOrThrow(const fs::path& root, const fs::path& target) {
        if (root.empty()) {
            fs::remove_all(target);
            return;
        }
        std::string error;
        if (!removeTreeResolvedBeneath(root, target, error)) {
            throw fs::filesystem_error(error, target, std::make_error_code(std::errc::operation_not_permitted));
        }
    }

    // ------------------------------------------------------------------ the write scope

    namespace {
        // a scope's trusted roots (the project directory and its assets directory: each resolved
        // once); no roots: a suspending scope
        struct ScopeEntry {
            std::vector<fs::path> roots;
            bool strict = false;
        };
        thread_local std::vector<ScopeEntry> scopes;
        thread_local std::string scopedError;
        // the scoped failures on this thread: a running count and the latest texts (a step reads
        // the first one since its mark: a later success doesn't hide it)
        thread_local unsigned long long scopedFailures = 0;
        thread_local std::deque<std::string> recentFailures;
        constexpr size_t kRecentFailures = 64;

        std::mutex& globalMutex() {
            static std::mutex m;
            return m;
        }
        std::function<void(const std::string&)>& sink() {
            static std::function<void(const std::string&)> s;
            return s;
        }
        std::vector<fs::path>& outsideAllowed() {
            static std::vector<fs::path> dirs;
            return dirs;
        }

        int scopedResult(bool ok, const std::string& error, std::error_code& ec) {
            if (ok) {
                scopedError.clear();
                ec.clear();
                return 1;
            }
            scopedError = error;
            scopedFailures++;
            recentFailures.push_back(error);
            if (recentFailures.size() > kRecentFailures) recentFailures.pop_front();
            ec = std::make_error_code(std::errc::operation_not_permitted);
            warn(error);
            return 0;
        }

        bool lexicallyIn(const fs::path& absolute, const fs::path& root) {
            if (isAtOrBelow(absolute, root)) return true;
            const fs::path real = resolvedPath(root);
            return !real.empty() && isAtOrBelow(absolute, real);
        }

        // The scopes in force: those above the innermost suspending (empty) one
        std::vector<const ScopeEntry*> activeScopes() {
            std::vector<const ScopeEntry*> active;
            for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
                if (it->roots.empty()) break;
                active.push_back(&*it);   // innermost first
            }
            return active;
        }

        // the scope holds the path as spelled, or where it resolves to
        bool holdsLexically(const ScopeEntry& scope, const fs::path& absolute) {
            for (const fs::path& root : scope.roots) {
                if (lexicallyIn(absolute, root)) return true;
            }
            const fs::path where = writeLocation(absolute);
            for (const fs::path& root : scope.roots) {
                if (!where.empty() && isAtOrBelow(where, root)) return true;
            }
            return false;
        }

        // the first trusted root of `roots` holding every path of `paths` once links are resolved ("" none)
        fs::path containingRoot(const std::vector<fs::path>& roots, std::initializer_list<fs::path> paths, bool entry) {
            for (const fs::path& root : roots) {
                bool all = true;
                for (const fs::path& p : paths) {
                    if (!p.empty() && !escapeOf(root, p, entry).empty()) all = false;
                }
                if (all) return root;
            }
            return fs::path();
        }

        // by resolved paths: a path spelled under an allowed folder that a link leads elsewhere is not
        bool allowedOutside(const fs::path& absolute) {
            const fs::path where = writeLocation(absolute);
            if (where.empty()) return false;
            std::lock_guard<std::mutex> lock(globalMutex());
            for (const fs::path& dir : outsideAllowed()) {
                if (isAtOrBelow(where, dir)) return true;
            }
            return false;
        }

        std::string rootsShown(const ScopeEntry& scope) {
            std::string text;
            for (const fs::path& root : scope.roots) text += (text.empty() ? "" : " or ") + shown(root);
            return text;
        }

        enum class Route { Plain, Beneath, Refused };

        // How a scoped operation on `path` goes: plain (no scope in force holds it, and no strict
        // scope refuses it), beneath `root` (a trusted root of the innermost scope holding it once
        // resolved; fail-closed: every scope in force must hold it once resolved, in one of its
        // trusted roots), or refused (`error`)
        Route route(const fs::path& path, bool entry, fs::path& root, std::string& error, const fs::path& alsoHolding = {}) {
            root.clear();
            const std::vector<const ScopeEntry*> active = activeScopes();
            if (active.empty() || path.empty()) return Route::Plain;
            const fs::path absolute = absoluteNormal(path);
            const fs::path other = alsoHolding.empty() ? fs::path() : absoluteNormal(alsoHolding);
            bool held = false;
            for (const ScopeEntry* scope : active) {
                if (holdsLexically(*scope, absolute) || (!other.empty() && holdsLexically(*scope, other))) held = true;
            }
            if (!held) {
                // outside every root: an MCP call (a strict scope) writes nowhere else, but the
                // editor's own places (its settings, the temporary folder)
                for (const ScopeEntry* scope : active) {
                    if (scope->strict && !allowedOutside(absolute)) {
                        error = shown(absolute) + " is outside the project (" + rootsShown(*scope) +
                                "): an MCP call writes only inside it. Use a path inside the project.";
                        return Route::Refused;
                    }
                }
                return Route::Plain;
            }
            // fail-closed: every scope in force holds each path once links are resolved, in one of
            // its trusted roots
            for (const ScopeEntry* scope : active) {
                for (const fs::path& p : {absolute, other}) {
                    if (p.empty() || !containingRoot(scope->roots, {p}, entry).empty()) continue;
                    error = escapeOf(scope->roots.front(), p, entry);
                    if (scope->roots.size() > 1) error += " (nor inside " + rootsShown(*scope) + ")";
                    error += " Remove the link (or use a path that stays inside), then try again.";
                    return Route::Refused;
                }
            }
            // beneath the innermost scope's root holding both ends (a rename)
            root = containingRoot(active.front()->roots, {absolute, other}, entry);
            if (root.empty()) {
                error = shown(absolute) + " and " + shown(other) + " are not inside one trusted root (" + rootsShown(*active.front()) +
                        "): a move across them is refused.";
                return Route::Refused;
            }
            return Route::Beneath;
        }

        // The editor's own internal writers that reach an owned place name it here (SafeWrite.h,
        // EditorInternal); an agent-named write gets none.
        thread_local std::vector<fs::path> editorInternalAllowed;   // spelled (lexically normal)
        thread_local std::vector<fs::path> editorInternalResolved;  // and resolved

        bool editorInternalCovers(const fs::path& absolute) {
            if (editorInternalAllowed.empty()) return false;
            const fs::path resolved = writeLocation(absolute);
            for (size_t i = 0; i < editorInternalAllowed.size(); i++) {
                if (isAtOrBelow(absolute, editorInternalAllowed[i])) return true;
                if (!resolved.empty() && !editorInternalResolved[i].empty() && isAtOrBelow(resolved, editorInternalResolved[i])) return true;
            }
            return false;
        }

        // why `rel` (relative to the project directory, lexically normal) names an editor-owned or
        // hidden place, "" when it doesn't: a component starting with '.' (hidden), a file named
        // project.yaml at any depth, or the project's own root CMakeLists.txt. Names are compared as
        // the file system may (comparableName). ProjectBuild.cmake is not owned.
        std::string ownedRelReason(const fs::path& rel, bool projectRoot = true) {
            if (rel.empty() || *rel.begin() == "..") return std::string();
            size_t parts = 0;
            for (const fs::path& part : rel) {
                const std::string name = comparableName(part);
                if (name == ".") continue;
                parts++;
                if (!name.empty() && name[0] == '.') {
                    return "it is under the editor's hidden folder " + FileUtils::pathToUtf8(part) +
                           " (.jasperengine and other dot-folders are the editor's): tools don't write, create, move or copy files there";
                }
            }
            if (isProjectFileName(rel)) {
                return "it is a file named project.yaml: the editor writes the project's own (set_project_settings); a tool makes no "
                       "file of that name in any folder (a second project.yaml would be a project an agent could open)";
            }
            if (projectRoot && parts == 1 && comparableName(rel) == "cmakelists.txt") {
                return "CMakeLists.txt is regenerated on every build; put custom build settings in ProjectBuild.cmake";
            }
            return std::string();
        }

        // Deny by default inside a strict scope: a write / create / rename-to / copy-to of an
        // editor-owned or hidden place (ownedRelReason), as named and where it resolves, unless an
        // EditorInternal token covers it. "" when allowed. `entry`: a rename/remove of the entry
        // itself (its final name as named, not followed).
        bool strictScopeActive() {
            for (const ScopeEntry* scope : activeScopes()) {
                if (scope->strict) return true;
            }
            return false;
        }

        std::string protectedPlaceRefusal(const fs::path& path, bool entry) {
            if (path.empty()) return std::string();
            const ScopeEntry* strict = nullptr;
            for (const ScopeEntry* scope : activeScopes()) {
                if (scope->strict) { strict = scope; break; }
            }
            if (!strict || strict->roots.empty()) return std::string();
            const fs::path absolute = absoluteNormal(path);
            if (editorInternalCovers(absolute)) return std::string();
            const fs::path where = entry ? entryLocation(absolute) : writeLocation(absolute);
            // below EVERY trusted root (the project directory first, then the assets and Lua roots,
            // which may lie outside it): hidden folders and project.yaml anywhere; the root
            // CMakeLists.txt only at the project directory
            for (size_t i = 0; i < strict->roots.size(); i++) {
                const fs::path& root = strict->roots[i];
                // the root as named and where it resolves: a project reached through a link (macOS's
                // /var is one) has `where` below the latter only
                for (const fs::path& base : {root, resolvedPath(root)}) {
                    for (const fs::path& loc : {absolute, where}) {
                        if (base.empty() || loc.empty() || !isAtOrBelow(loc, base)) continue;
                        if (std::string why = ownedRelReason(loc.lexically_relative(base), i == 0); !why.empty()) {
                            return shown(absolute) + ": " + why + ".";
                        }
                    }
                }
            }
            // a file named project.yaml wherever it lands (also outside every root)
            if (isProjectFileName(absolute) || (!where.empty() && isProjectFileName(where))) {
                return shown(absolute) + ": " + ownedRelReason(fs::path("project.yaml"), false) + ".";
            }
            return std::string();
        }

        // ... and for a folder copied in an MCP call: one named project.yaml anywhere in it
        // (links not followed; bounded: a folder too big to check is refused)
        std::string projectFileInsideRefusal(const fs::path& from, const fs::path& to) {
            if (const std::string refusal = protectedPlaceRefusal(to, false); !refusal.empty()) return refusal;
            bool strict = false;
            for (const ScopeEntry* scope : activeScopes()) strict = strict || scope->strict;
            std::error_code ec;
            if (!strict || from.empty() || editorInternalCovers(absoluteNormal(to)) ||
                !fs::is_directory(fs::symlink_status(from, ec))) {
                return std::string();
            }
            constexpr size_t kMaxEntries = 200000;
            size_t entries = 0;
            for (fs::recursive_directory_iterator it(from, fs::directory_options::none, ec), end; !ec && it != end; it.increment(ec)) {
                if (++entries > kMaxEntries) {
                    return shown(from) + " holds too many files to check for a project.yaml (over " + std::to_string(kMaxEntries) +
                           "): an MCP call doesn't copy it. Copy it in the editor.";
                }
                if (isProjectFileName(it->path())) {
                    return shown(from) + " holds a file named project.yaml (" + shown(it->path()) + "): an MCP call copies "
                           "no such file (a second project.yaml would be a project an agent could open). Remove it first, or copy the folder "
                           "in the editor.";
                }
            }
            return std::string();
        }

        int scopedWrite(const fs::path& path, const std::string& content, std::error_code& ec) {
            if (const std::string refusal = protectedPlaceRefusal(path, false); !refusal.empty()) return scopedResult(false, refusal, ec);
            fs::path root;
            std::string error;
            const Route r = route(path, false, root, error);
            if (r == Route::Plain) return -1;
            if (r == Route::Refused) return scopedResult(false, error, ec);
            return scopedResult(writeFileResolvedBeneath(root, path, content, error), error, ec);
        }
        // a real folder already there (not a link): creating it again creates nothing, so it is no
        // write into an owned place (a writer whose token names only its file still ensures its folder)
        bool existingRealDir(const fs::path& path) {
            std::error_code ec;
            return fs::is_directory(fs::symlink_status(absoluteNormal(path), ec)) && !ec;
        }

        int scopedCreateDirectories(const fs::path& path, std::error_code& ec) {
            if (!existingRealDir(path)) {
                if (const std::string refusal = protectedPlaceRefusal(path, false); !refusal.empty()) return scopedResult(false, refusal, ec);
            }
            fs::path root;
            std::string error;
            const Route r = route(path, false, root, error);
            if (r == Route::Plain) return -1;
            if (r == Route::Refused) return scopedResult(false, error, ec);
            return scopedResult(createDirectoriesResolvedBeneath(root, path, error), error, ec);
        }
        int scopedRemove(const fs::path& path, std::error_code& ec) {
            fs::path root;
            std::string error;
            const Route r = route(path, true, root, error);
            if (r == Route::Plain) return -1;
            if (r == Route::Refused) return scopedResult(false, error, ec);
            std::error_code status;
            // fs::remove: a file, a link or an empty folder
            if (fs::is_directory(fs::symlink_status(path, status))) {
                if (!fs::is_empty(path, status)) {
                    // as fs::remove: an error, nothing to warn about
                    ec = std::make_error_code(std::errc::directory_not_empty);
                    return 0;
                }
                return scopedResult(removeEmptyDirResolvedBeneath(root, path, error), error, ec);
            }
            return scopedResult(removeFileResolvedBeneath(root, path, error), error, ec);
        }
        int scopedRemoveAll(const fs::path& path, std::error_code& ec) {
            fs::path root;
            std::string error;
            const Route r = route(path, true, root, error);
            if (r == Route::Plain) return -1;
            if (r == Route::Refused) return scopedResult(false, error, ec);
            return scopedResult(removeTreeResolvedBeneath(root, path, error), error, ec);
        }
        int scopedRename(const fs::path& from, const fs::path& to, std::error_code& ec) {
            // either end inside a scope: beneath the innermost root holding both
            // renamed into an owned place: refused (an owned entry moved as it is — to the trash and
            // back — is not a new one, so a project.yaml being relocated is let through)
            if (!isProjectFileName(from)) {
                if (const std::string refusal = protectedPlaceRefusal(to, true); !refusal.empty()) return scopedResult(false, refusal, ec);
            }
            fs::path root;
            std::string error;
            const Route r = route(from, true, root, error, to);
            if (r == Route::Plain) return -1;
            if (r == Route::Refused) return scopedResult(false, error, ec);
            return scopedResult(renameResolvedBeneath(root, from, to, error, true), error, ec);
        }
        int scopedCopy(const fs::path& from, const fs::path& to, bool overwrite, std::error_code& ec) {
            if (const std::string refusal = projectFileInsideRefusal(from, to); !refusal.empty()) return scopedResult(false, refusal, ec);
            fs::path root;
            std::string error;
            const Route r = route(to, false, root, error);
            if (r == Route::Plain) return -1;
            if (r == Route::Refused) return scopedResult(false, error, ec);
            return scopedResult(copyResolvedBeneath(root, from, to, overwrite, error), error, ec);
        }

        const FileUtils::ScopedOps kScopedOps{scopedWrite, scopedCreateDirectories, scopedRemove, scopedRemoveAll, scopedRename, scopedCopy};
    }

    Scope::Scope(fs::path root, bool strict) {
        // installed once (an atomic store: readers on any thread see it whole)
        FileUtils::installScopedOps(&kScopedOps);
        ScopeEntry entry;
        if (!root.empty()) entry.roots.push_back(std::move(root));
        entry.strict = strict;
        scopes.push_back(std::move(entry));
    }

    Scope::Scope(std::vector<fs::path> roots, bool strict) {
        FileUtils::installScopedOps(&kScopedOps);
        ScopeEntry entry;
        for (fs::path& root : roots) {
            if (root.empty()) continue;
            if (std::find(entry.roots.begin(), entry.roots.end(), root) == entry.roots.end()) entry.roots.push_back(std::move(root));
        }
        entry.strict = strict;
        scopes.push_back(std::move(entry));
    }

    Scope::~Scope() {
        scopes.pop_back();
    }

    EditorInternal::EditorInternal(fs::path path) {
        if (path.empty()) return;
        editorInternalAllowed.push_back(absoluteNormal(path));
        editorInternalResolved.push_back(writeLocation(absoluteNormal(path)));
        pushed = true;
    }

    EditorInternal::~EditorInternal() {
        if (!pushed) return;
        editorInternalAllowed.pop_back();
        editorInternalResolved.pop_back();
    }

    fs::path Scope::current() {
        return scopes.empty() || scopes.back().roots.empty() ? fs::path() : scopes.back().roots.front();
    }

    std::vector<fs::path> Scope::currentRoots() {
        return scopes.empty() ? std::vector<fs::path>() : scopes.back().roots;
    }

    fs::path trustedRootFor(const std::vector<fs::path>& roots, const fs::path& path, bool entry) {
        if (roots.empty()) return fs::path();
        const fs::path root = containingRoot(roots, {path}, entry);
        return root.empty() ? roots.front() : root;
    }

    fs::path scopeRootFor(const fs::path& path) {
        // the innermost scope in force holding the path: its trusted root holding it once
        // resolved (else the first one holding it as spelled); "" none, or suspended
        if (path.empty()) return fs::path();
        const fs::path absolute = absoluteNormal(path);
        for (const ScopeEntry* scope : activeScopes()) {
            if (!holdsLexically(*scope, absolute)) continue;
            const fs::path root = containingRoot(scope->roots, {absolute}, false);
            if (!root.empty()) return root;
            for (const fs::path& r : scope->roots) {
                if (lexicallyIn(absolute, r)) return r;
            }
        }
        return fs::path();
    }

    std::string scopesRefusal(const fs::path& path, bool entry) {
        fs::path root;
        std::string error;
        return route(path, entry, root, error) == Route::Refused ? error : std::string();
    }

    void allowOutsideScopes(const fs::path& dir) {
        if (dir.empty()) return;
        const fs::path resolved = resolvedPath(dir);
        if (resolved.empty()) return;
        std::lock_guard<std::mutex> lock(globalMutex());
        outsideAllowed().push_back(normalDir(resolved));
    }

    void pinTrustedRoot(const fs::path& resolvedRoot) {
        if (resolvedRoot.empty()) return;
        const fs::path normal = normalDir(resolvedRoot);
        std::lock_guard<std::mutex> lock(pinnedMutex());
        if (std::find(pinnedRoots().begin(), pinnedRoots().end(), normal) == pinnedRoots().end()) pinnedRoots().push_back(normal);
    }

    bool trustedRootIntact(const fs::path& resolvedRoot) {
        if (resolvedRoot.empty()) return false;
        std::error_code ec;
        if (fs::is_symlink(fs::symlink_status(resolvedRoot, ec))) return false;
        return normalDir(resolvedPath(resolvedRoot)) == normalDir(resolvedRoot);
    }

    bool Scope::currentStrict() {
        return !scopes.empty() && scopes.back().strict;
    }

    unsigned long long scopedFailureCount() {
        return scopedFailures;
    }

    std::string scopedFailureSince(unsigned long long mark) {
        if (scopedFailures <= mark) return std::string();
        const unsigned long long first = scopedFailures - recentFailures.size();
        const unsigned long long index = mark < first ? 0 : mark - first;
        return index < recentFailures.size() ? recentFailures[static_cast<size_t>(index)] : std::string();
    }

    const std::string& lastScopedError() {
        return scopedError;
    }

    void setWarningSink(std::function<void(const std::string&)> s) {
        std::lock_guard<std::mutex> lock(globalMutex());
        sink() = std::move(s);
    }

    void warn(const std::string& message) {
        std::function<void(const std::string&)> s;
        {
            std::lock_guard<std::mutex> lock(globalMutex());
            s = sink();
        }
        if (s) s(message);
        else std::cerr << message << std::endl;
    }

}
