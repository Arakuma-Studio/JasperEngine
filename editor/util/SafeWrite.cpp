// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SafeWrite.h"

#include "util/FileUtils.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <system_error>
#include <thread>
#include <utility>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <unistd.h>
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

        bool realRootOf(const fs::path& root, fs::path& realRoot, std::string& error) {
            realRoot = resolvedPath(root);
            if (realRoot.empty()) {
                error = shown(root) + " can't be resolved: a symbolic link on its way loops or can't be read.";
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

#ifdef _WIN32

        std::string systemMessage(DWORD code) { return std::system_category().message(static_cast<int>(code)); }

        bool isMissing(DWORD code) { return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND; }

        // The folder handles of a walk, held (no delete sharing) until it is destroyed
        struct Held {
            std::vector<HANDLE> handles;
            Held() = default;
            Held(const Held&) = delete;
            Held& operator=(const Held&) = delete;
            ~Held() {
                for (HANDLE h : handles) CloseHandle(h);
            }
        };

        // A folder (or whatever is there) opened without following a reparse point at it
        HANDLE openFolder(const fs::path& path, bool followReparse) {
            return CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | (followReparse ? 0 : FILE_FLAG_OPEN_REPARSE_POINT), nullptr);
        }

        DWORD attributesOf(HANDLE handle) {
            BY_HANDLE_FILE_INFORMATION info;
            if (!GetFileInformationByHandle(handle, &info)) return INVALID_FILE_ATTRIBUTES;
            return info.dwFileAttributes;
        }

        // The entry at `path` itself (a link: the link): its attributes, INVALID_FILE_ATTRIBUTES
        // when nothing is there (`missing`) or it can't be read
        DWORD entryAttributes(const fs::path& path, bool& missing) {
            missing = false;
            const HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                missing = isMissing(GetLastError());
                return INVALID_FILE_ATTRIBUTES;
            }
            const DWORD attributes = attributesOf(h);
            CloseHandle(h);
            return attributes;
        }

        // Every folder from realRoot down to realRoot/rel opened and held; `dir` is that folder.
        // `missing` (when not creating): a folder on the way does not exist.
        bool openDir(const fs::path& realRoot, const fs::path& rel, bool create, Held& held, fs::path& dir, std::string& error,
                     std::vector<fs::path>* created, bool* missing = nullptr) {
            if (missing) *missing = false;
            // the root is trusted (resolved already): opened following what it is
            HANDLE h = openFolder(realRoot, true);
            if (h == INVALID_HANDLE_VALUE) {
                error = "can't open the folder " + shown(realRoot) + ": " + systemMessage(GetLastError());
                return false;
            }
            held.handles.push_back(h);
            dir = realRoot;
            for (const fs::path& part : rel) {
                dir /= part;
                h = openFolder(dir, false);
                if (h == INVALID_HANDLE_VALUE && isMissing(GetLastError()) && create) {
                    if (CreateDirectoryW(dir.c_str(), nullptr)) {
                        if (created) created->push_back(dir);
                    } else if (GetLastError() != ERROR_ALREADY_EXISTS) {
                        error = "can't create the folder " + shown(dir) + ": " + systemMessage(GetLastError());
                        return false;
                    }
                    h = openFolder(dir, false);
                }
                if (h == INVALID_HANDLE_VALUE) {
                    const DWORD code = GetLastError();
                    if (missing && isMissing(code)) *missing = true;
                    error = "can't open the folder " + shown(dir) + ": " + systemMessage(code);
                    return false;
                }
                held.handles.push_back(h);
                const DWORD attributes = attributesOf(h);
                if (attributes == INVALID_FILE_ATTRIBUTES) {
                    error = "can't read " + shown(dir) + ": " + systemMessage(GetLastError());
                    return false;
                }
                if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    error = linkRefusal(dir, realRoot);
                    return false;
                }
                if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    error = shown(dir) + " is a file, not a folder: a path on the way to the file must be folders.";
                    return false;
                }
            }
            return true;
        }

        bool moveRetrying(const fs::path& from, const fs::path& to, DWORD flags, DWORD& code) {
            // a file a reader, the indexer or a virus scan briefly holds can't be replaced: tried again for a while
            for (int attempt = 0; attempt < FileUtils::renameAttempts; attempt++) {
                if (attempt > 0) std::this_thread::sleep_for(std::chrono::milliseconds(FileUtils::renameRetryMs));
                if (MoveFileExW(from.c_str(), to.c_str(), flags)) return true;
                code = GetLastError();
                if (code != ERROR_SHARING_VIOLATION && code != ERROR_ACCESS_DENIED && code != ERROR_LOCK_VIOLATION) return false;
            }
            return false;
        }

#else

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

        constexpr int kDirFlags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;

        // realRoot/rel opened folder by folder from realRoot (no link followed below it); `missing`
        // (when not creating): a folder on the way does not exist
        bool openDir(const fs::path& realRoot, const fs::path& rel, bool create, Fd& out, std::string& error,
                     std::vector<fs::path>* created, bool* missing = nullptr) {
            if (missing) *missing = false;
            // the root is trusted (resolved already): opened following what it is
            Fd dir(::open(realRoot.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
            if (dir.get() < 0) {
                error = "can't open the folder " + shown(realRoot) + ": " + std::strerror(errno);
                return false;
            }
            fs::path at = realRoot;
            for (const fs::path& part : rel) {
                at /= part;
                int fd = ::openat(dir.get(), part.c_str(), kDirFlags);
                if (fd < 0 && errno == ENOENT && create) {
                    if (::mkdirat(dir.get(), part.c_str(), 0777) == 0) {
                        if (created) created->push_back(at);
                    } else if (errno != EEXIST) {
                        error = "can't create the folder " + shown(at) + ": " + std::strerror(errno);
                        return false;
                    }
                    fd = ::openat(dir.get(), part.c_str(), kDirFlags);
                }
                if (fd < 0) {
                    const int code = errno;
                    struct stat st;
                    const bool stated = ::fstatat(dir.get(), part.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0;
                    if (stated && S_ISLNK(st.st_mode)) {
                        error = linkRefusal(at, realRoot);
                    } else if (stated && !S_ISDIR(st.st_mode)) {
                        error = shown(at) + " is a file, not a folder: a path on the way to the file must be folders.";
                    } else {
                        if (missing && code == ENOENT) *missing = true;
                        error = "can't open the folder " + shown(at) + ": " + std::strerror(code);
                    }
                    errno = code;
                    return false;
                }
                dir = Fd(fd);
            }
            out = std::move(dir);
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

    bool isAtOrBelow(const fs::path& path, const fs::path& dir) {
        const fs::path rel = normalDir(path).lexically_relative(normalDir(dir));
        return !rel.empty() && *rel.begin() != "..";
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

    bool writeFileBeneath(const fs::path& root, const fs::path& target, const std::string& bytes, std::string& error,
                          std::vector<fs::path>* createdDirs) {
        fs::path realRoot, rel;
        if (!realRootOf(root, realRoot, error) || !relativeBelow(root, realRoot, target, rel, error)) return false;
        const fs::path name = rel.filename();
        const fs::path full = realRoot / rel;
        std::vector<fs::path> made;
#ifdef _WIN32
        bool ok = false;
        {
            Held held;
            fs::path dir;
            if (openDir(realRoot, rel.parent_path(), true, held, dir, error, &made)) {
                const fs::path finalPath = dir / name;
                bool missing = false;
                const DWORD existing = entryAttributes(finalPath, missing);
                if (existing != INVALID_FILE_ATTRIBUTES && (existing & FILE_ATTRIBUTE_DIRECTORY) && !(existing & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    error = shown(full) + " is a folder: name a file.";
                } else if (existing != INVALID_FILE_ATTRIBUTES && !(existing & FILE_ATTRIBUTE_REPARSE_POINT) && (existing & FILE_ATTRIBUTE_READONLY)) {
                    error = shown(full) + " is read-only: clear its read-only attribute, then try again.";
                } else {
                    fs::path temp;
                    HANDLE file = INVALID_HANDLE_VALUE;
                    DWORD code = 0;
                    for (int attempt = 0; attempt < 16 && file == INVALID_HANDLE_VALUE; attempt++) {
                        temp = dir / tempNameFor(name);
                        // CREATE_NEW + FILE_FLAG_OPEN_REPARSE_POINT: an entry already at the name
                        // (a planted link) fails the create instead of being followed
                        file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                        if (file == INVALID_HANDLE_VALUE) {
                            code = GetLastError();
                            if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS) break;
                        }
                    }
                    if (file == INVALID_HANDLE_VALUE) {
                        error = "can't write " + shown(full) + ": " + systemMessage(code);
                    } else {
                        bool wrote = true;
                        size_t done = 0;
                        while (wrote && done < bytes.size()) {
                            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - done, 1u << 30));
                            DWORD written = 0;
                            wrote = WriteFile(file, bytes.data() + done, chunk, &written, nullptr) && written > 0;
                            if (!wrote) code = GetLastError();
                            done += written;
                        }
                        if (!CloseHandle(file) && wrote) {
                            wrote = false;
                            code = GetLastError();
                        }
                        if (wrote && moveRetrying(temp, finalPath, MOVEFILE_REPLACE_EXISTING, code)) {
                            ok = true;
                        } else {
                            error = "can't write " + shown(full) + ": " + systemMessage(code);
                            DeleteFileW(temp.c_str());
                        }
                    }
                }
            }
        }
#else
        bool ok = false;
        Fd dir;
        if (openDir(realRoot, rel.parent_path(), true, dir, error, &made)) {
            struct stat st;
            bool existingFile = false;
            bool refused = false;
            mode_t keep = 0;
            if (::fstatat(dir.get(), name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    error = shown(full) + " is a folder: name a file.";
                    errno = EISDIR;
                    refused = true;
                } else if (S_ISREG(st.st_mode)) {
                    existingFile = true;
                    keep = st.st_mode & 07777;
                    // the folder's permissions would let the rename replace a file the user may not write
                    if (::faccessat(dir.get(), name.c_str(), W_OK, 0) != 0) {
                        const int code = errno;
                        error = shown(full) + " is read-only (" + std::strerror(code) + "): make it writable, then try again.";
                        errno = code;
                        refused = true;
                    }
                }
            }
            if (!refused) {
                fs::path temp;
                int fd = -1;
                int code = 0;
                for (int attempt = 0; attempt < 16 && fd < 0; attempt++) {
                    temp = tempNameFor(name);
                    fd = ::openat(dir.get(), temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
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
                    size_t done = 0;
                    while (done < bytes.size()) {
                        const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
                        if (n < 0) {
                            if (errno == EINTR) continue;
                            code = errno;
                            wrote = false;
                            break;
                        }
                        done += static_cast<size_t>(n);
                    }
                    if (wrote && existingFile) (void)::fchmod(fd, keep);
                    if (::close(fd) != 0 && wrote) {
                        code = errno;
                        wrote = false;
                    }
                    // renameat on the folder's descriptor: a link at the name is replaced, never followed
                    if (wrote && ::renameat(dir.get(), temp.c_str(), dir.get(), name.c_str()) != 0) {
                        code = errno;
                        wrote = false;
                    }
                    if (wrote) {
                        ok = true;
                    } else {
                        ::unlinkat(dir.get(), temp.c_str(), 0);
                        error = "can't write " + shown(full) + ": " + std::strerror(code);
                        errno = code;
                    }
                }
            }
        }
        dir.reset();
#endif
        if (!ok) {
            const int code = errno;
            removeMade(realRoot, made);
            errno = code;
            return false;
        }
        if (createdDirs) createdDirs->insert(createdDirs->end(), made.begin(), made.end());
        return true;
    }

    bool removeFileBeneath(const fs::path& root, const fs::path& target, std::string& error) {
        fs::path realRoot, rel;
        if (!realRootOf(root, realRoot, error) || !relativeBelow(root, realRoot, target, rel, error)) return false;
        const fs::path full = realRoot / rel;
        bool missing = false;
#ifdef _WIN32
        Held held;
        fs::path dir;
        if (!openDir(realRoot, rel.parent_path(), false, held, dir, error, nullptr, &missing)) return missing;
        const fs::path entry = dir / rel.filename();
        const DWORD attributes = entryAttributes(entry, missing);
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            if (missing) return true;
            error = "can't remove " + shown(full) + ": " + systemMessage(GetLastError());
            return false;
        }
        const bool reparse = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) && !reparse) {
            error = shown(full) + " is a folder, not a file: it was left alone.";
            return false;
        }
        // a link to a folder (or a junction) goes with RemoveDirectoryW: the link, not its target
        const BOOL removed = (attributes & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(entry.c_str()) : DeleteFileW(entry.c_str());
        if (!removed && !isMissing(GetLastError())) {
            error = "can't remove " + shown(full) + ": " + systemMessage(GetLastError());
            return false;
        }
        return true;
#else
        Fd dir;
        if (!openDir(realRoot, rel.parent_path(), false, dir, error, nullptr, &missing)) return missing;
        struct stat st;
        if (::fstatat(dir.get(), rel.filename().c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) return true;
            error = "can't remove " + shown(full) + ": " + std::strerror(errno);
            return false;
        }
        if (S_ISDIR(st.st_mode)) {
            error = shown(full) + " is a folder, not a file: it was left alone.";
            return false;
        }
        if (::unlinkat(dir.get(), rel.filename().c_str(), 0) != 0 && errno != ENOENT) {
            error = "can't remove " + shown(full) + ": " + std::strerror(errno);
            return false;
        }
        return true;
#endif
    }

    bool removeEmptyDirBeneath(const fs::path& root, const fs::path& target, std::string& error) {
        fs::path realRoot, rel;
        if (!realRootOf(root, realRoot, error) || !relativeBelow(root, realRoot, target, rel, error)) return false;
        bool missing = false;
#ifdef _WIN32
        Held held;
        fs::path dir;
        if (!openDir(realRoot, rel.parent_path(), false, held, dir, error, nullptr, &missing)) return missing;
        const fs::path entry = dir / rel.filename();
        const DWORD attributes = entryAttributes(entry, missing);
        // gone, a link now, or no folder: nothing of ours to remove
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return true;
        if (!RemoveDirectoryW(entry.c_str())) {
            const DWORD code = GetLastError();
            if (code == ERROR_DIR_NOT_EMPTY || isMissing(code)) return true;
            error = "can't remove the folder " + shown(realRoot / rel) + ": " + systemMessage(code);
            return false;
        }
        return true;
#else
        Fd dir;
        if (!openDir(realRoot, rel.parent_path(), false, dir, error, nullptr, &missing)) return missing;
        struct stat st;
        // gone, a link now, or no folder: nothing of ours to remove
        if (::fstatat(dir.get(), rel.filename().c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(st.st_mode)) return true;
        if (::unlinkat(dir.get(), rel.filename().c_str(), AT_REMOVEDIR) != 0) {
            if (errno == ENOTEMPTY || errno == EEXIST || errno == ENOENT) return true;
            error = "can't remove the folder " + shown(realRoot / rel) + ": " + std::strerror(errno);
            return false;
        }
        return true;
#endif
    }

    bool createDirBeneath(const fs::path& root, const fs::path& target, std::string& error) {
        fs::path realRoot, rel;
        if (!realRootOf(root, realRoot, error) || !relativeBelow(root, realRoot, target, rel, error)) return false;
        const fs::path full = realRoot / rel;
#ifdef _WIN32
        Held held;
        fs::path dir;
        if (!openDir(realRoot, rel.parent_path(), false, held, dir, error, nullptr)) return false;
        if (!CreateDirectoryW((dir / rel.filename()).c_str(), nullptr)) {
            const DWORD code = GetLastError();
            error = code == ERROR_ALREADY_EXISTS ? shown(full) + " already exists." : "can't create the folder " + shown(full) + ": " + systemMessage(code);
            return false;
        }
        return true;
#else
        Fd dir;
        if (!openDir(realRoot, rel.parent_path(), false, dir, error, nullptr)) return false;
        if (::mkdirat(dir.get(), rel.filename().c_str(), 0777) != 0) {
            error = errno == EEXIST ? shown(full) + " already exists." : "can't create the folder " + shown(full) + ": " + std::strerror(errno);
            return false;
        }
        return true;
#endif
    }

    bool renameBeneath(const fs::path& root, const fs::path& from, const fs::path& to, std::string& error) {
        fs::path realRoot, relFrom, relTo;
        if (!realRootOf(root, realRoot, error) || !relativeBelow(root, realRoot, from, relFrom, error) ||
            !relativeBelow(root, realRoot, to, relTo, error)) {
            return false;
        }
        const fs::path fullFrom = realRoot / relFrom, fullTo = realRoot / relTo;
#ifdef _WIN32
        Held heldFrom, heldTo;
        fs::path dirFrom, dirTo;
        if (!openDir(realRoot, relFrom.parent_path(), false, heldFrom, dirFrom, error, nullptr) ||
            !openDir(realRoot, relTo.parent_path(), false, heldTo, dirTo, error, nullptr)) {
            return false;
        }
        DWORD code = 0;
        // no MOVEFILE_REPLACE_EXISTING: an entry at `to` fails the move
        if (!moveRetrying(dirFrom / relFrom.filename(), dirTo / relTo.filename(), 0, code)) {
            error = code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS
                        ? shown(fullTo) + " already exists: delete it first or pick another name."
                        : "can't move " + shown(fullFrom) + " to " + shown(fullTo) + ": " + systemMessage(code);
            return false;
        }
        return true;
#else
        Fd dirFrom, dirTo;
        if (!openDir(realRoot, relFrom.parent_path(), false, dirFrom, error, nullptr) ||
            !openDir(realRoot, relTo.parent_path(), false, dirTo, error, nullptr)) {
            return false;
        }
        struct stat st;
        if (::fstatat(dirTo.get(), relTo.filename().c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
            error = shown(fullTo) + " already exists: delete it first or pick another name.";
            return false;
        }
        if (::renameat(dirFrom.get(), relFrom.filename().c_str(), dirTo.get(), relTo.filename().c_str()) != 0) {
            error = "can't move " + shown(fullFrom) + " to " + shown(fullTo) + ": " + std::strerror(errno);
            return false;
        }
        return true;
#endif
    }

    // ------------------------------------------------------------------ resolved, then beneath

    std::string escapeOf(const fs::path& root, const fs::path& target, bool entry, const std::string& shownName) {
        const std::string name = shownName.empty() ? shown(target) : shownName;
        const fs::path realRoot = resolvedPath(root);
        if (realRoot.empty()) return shown(root) + " can't be resolved: a symbolic link on its way loops or can't be read.";
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

    bool createDirResolvedBeneath(const fs::path& root, const fs::path& dir, std::string& error) {
        if (refuseEscape(root, dir, true, error)) return false;
        return createDirBeneath(resolvedPath(root), entryLocation(absoluteIn(root, dir)), error);
    }

    bool renameResolvedBeneath(const fs::path& root, const fs::path& from, const fs::path& to, std::string& error) {
        if (refuseEscape(root, from, true, error) || refuseEscape(root, to, true, error)) return false;
        return renameBeneath(resolvedPath(root), entryLocation(absoluteIn(root, from)), entryLocation(absoluteIn(root, to)), error);
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

}
