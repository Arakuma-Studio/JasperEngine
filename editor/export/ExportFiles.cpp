// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "export/ExportFiles.h"

#include "util/FileUtils.h"
#include "util/SafeWrite.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <set>
#include <vector>

namespace jasperengine::editor::exportfiles {

namespace {


    std::string text(const fs::path& path) {
        return FileUtils::pathToGenericUtf8(path);
    }

    bool sameContents(const fs::path& a, const fs::path& b) {
        std::error_code ec;
        const uintmax_t sizeA = fs::file_size(a, ec);
        if (ec) return false;
        const uintmax_t sizeB = fs::file_size(b, ec);
        if (ec || sizeA != sizeB) return false;
        std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
        if (!fa || !fb) return false;
        std::vector<char> bufA(1 << 16), bufB(1 << 16);
        while (true) {
            fa.read(bufA.data(), static_cast<std::streamsize>(bufA.size()));
            fb.read(bufB.data(), static_cast<std::streamsize>(bufB.size()));
            if (fa.gcount() != fb.gcount()) return false;
            if (fa.gcount() == 0) return true;
            if (std::memcmp(bufA.data(), bufB.data(), static_cast<size_t>(fa.gcount())) != 0) return false;
        }
    }

    // remove_all that never follows a link out of the tree (util/SafeWrite.h)
    bool removeNoFollow(const fs::path& path, std::string& error) {
        std::error_code ec;
        if (!fs::exists(fs::symlink_status(path, ec))) return true;
        const fs::path root = safewrite::existingFolderOf(path.parent_path());
        std::string why;
        if (root.empty() || !safewrite::removeTreeBeneath(root, safewrite::entryLocation(path), why)) {
            error = "Failed to remove " + text(path) + (why.empty() ? std::string() : ": " + why);
            return false;
        }
        return true;
    }

}

fs::path longPath(const fs::path& path) {
#ifdef _WIN32
    std::error_code ec;
    const fs::path absolutePath = fs::absolute(path, ec);
    if (ec) return path;
    const fs::path::string_type nativePath = absolutePath.native();
    if (nativePath.rfind(L"\\\\?\\", 0) == 0) return absolutePath;
    if (nativePath.rfind(L"\\\\", 0) == 0) return fs::path(L"\\\\?\\UNC\\" + nativePath.substr(2));
    return fs::path(L"\\\\?\\" + nativePath);
#else
    return path;
#endif
}

bool isSkippableOpenError(const std::error_code& ec) {
    return ec == std::errc::permission_denied || ec == std::errc::operation_not_permitted;
}

WalkResult walk(const fs::path& root, const DirFilter& filter, const Visitor& visit) {
    WalkResult result;
    std::error_code ec;
    const fs::file_status rootStatus = fs::status(root, ec);
    if (ec || !fs::is_directory(rootStatus)) {
        result.ok = false;
        result.error = "Cannot read the folder " + text(root) + ": " + (ec ? ec.message() : std::string("not a folder"));
        return result;
    }

    // the long form: entries' paths come out in it, so every copy of them works past MAX_PATH
    const fs::path base = longPath(root);
    std::vector<fs::path> pending{base};
    while (!pending.empty()) {
        const fs::path dir = pending.back();
        pending.pop_back();

        fs::directory_iterator it(dir, ec);
        if (ec) {
            if (isSkippableOpenError(ec)) {
                result.unreadable.push_back(dir);
                ec.clear();
                continue;
            }
            result.ok = false;
            result.error = "Cannot read the folder " + text(dir) + ": " + ec.message();
            return result;
        }
        // children in name order: a deterministic walk (and the same warnings every export)
        std::vector<fs::directory_entry> entries;
        for (fs::directory_iterator end; it != end; it.increment(ec)) {
            entries.push_back(*it);
        }
        if (ec) {
            result.ok = false;
            result.error = "Cannot read the folder " + text(dir) + ": " + ec.message();
            return result;
        }
        std::sort(entries.begin(), entries.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
            return a.path().filename() < b.path().filename();
        });

        std::vector<fs::path> subdirs;
        for (const fs::directory_entry& entry : entries) {
            const fs::path rel = entry.path().lexically_relative(base);
            if (rel.empty()) continue;
            std::error_code statusError;
            const fs::file_status linkStatus = entry.symlink_status(statusError);
            if (statusError) {
                result.ok = false;
                result.error = "Cannot read " + text(entry.path()) + ": " + statusError.message();
                return result;
            }
            if (fs::is_directory(linkStatus)) {
                if (!filter || filter(entry, rel) == DirAction::Descend) subdirs.push_back(entry.path());
                continue;
            }
            // a link to a folder is not entered (the linked-folder walk is Exporter::walkTree, S68);
            // a link to a file is visited as that file
            if (fs::is_symlink(linkStatus) && fs::is_directory(entry.status(statusError))) continue;
            if (!entry.is_regular_file(statusError)) continue;
            if (!visit(entry, rel)) {
                result.ok = false;
                return result;
            }
        }
        // depth first, in name order
        for (auto subdir = subdirs.rbegin(); subdir != subdirs.rend(); ++subdir) pending.push_back(*subdir);
    }
    return result;
}

bool copyFile(const fs::path& from, const fs::path& to, std::string& error) {
    std::error_code ec;
    if (!to.parent_path().empty()) fs::create_directories(longPath(to.parent_path()), ec);
    if (!ec) fs::copy_file(longPath(from), longPath(to), fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = "Failed to copy " + text(from) + " to " + text(to) + ": " + ec.message();
        return false;
    }
    return true;
}

bool writeFile(const fs::path& path, const std::string& content, std::string& error) {
    std::error_code ec;
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path(), ec);
    if (ec || FileUtils::writeFile(path, content) == FileUtils::WriteResult::Failed) {
        error = "Failed to write " + text(path) + (ec ? ": " + ec.message() : std::string());
        return false;
    }
    return true;
}

bool isIdeOutput(const fs::path& path) {
    static const std::set<std::string> names = {".gradle", ".idea", ".cxx", ".externalNativeBuild", ".kotlin", "build",
                                                "captures", "local.properties", "xcuserdata", "DerivedData", ".DS_Store"};
    return names.count(FileUtils::pathToUtf8(path.filename())) > 0 || path.extension() == ".iml";
}

std::string exportFolderMarker(const fs::path& dir, const std::string& appName) {
    std::error_code ec;
    for (const char* name : {"jasperengine.py", "content-manifest.json", "jasperengine-player-st.wasm"}) {
        if (fs::is_regular_file(dir / name, ec)) return name;
    }
    if (appName.empty()) return std::string();
    for (const std::string& name : {appName + ".wasm", appName, appName + ".exe", appName + ".aab", appName + ".apk",
                                    appName + "-arm64-v8a.apk", appName + "-armeabi-v7a.apk", appName + "-x86_64.apk",
                                    appName + "-x86.apk"}) {
        if (fs::is_regular_file(dir / FileUtils::pathFromUtf8(name), ec)) return name;
    }
    if (fs::is_directory(dir / FileUtils::pathFromUtf8(appName + ".app"), ec)) return appName + ".app";
    return std::string();
}

ExportFolderFinder::ExportFolderFinder(fs::path root, std::string appName)
    : root(root.lexically_normal()), appName(std::move(appName)) {}

bool ExportFolderFinder::contains(const fs::path& file) {
    const fs::path rel = (file.is_absolute() ? file.lexically_normal().lexically_relative(root) : file.lexically_normal());
    if (rel.empty() || *rel.begin() == "..") return false;
    // each folder from the root down to the file's own
    fs::path folder = root;
    for (auto part = rel.begin(); part != rel.end() && std::next(part) != rel.end(); ++part) {
        folder /= *part;
        auto known = folders.find(folder);
        if (known == folders.end()) known = folders.emplace(folder, !exportFolderMarker(folder, appName).empty()).first;
        if (known->second) return true;
    }
    return false;
}

bool mirrorTree(const fs::path& src, const fs::path& dst, bool keepIdeOutput, std::string& error) {
    const fs::path srcAdj = longPath(src);
    const fs::path dstAdj = longPath(dst);
    std::error_code ec;
    fs::create_directories(dstAdj, ec);
    if (ec) {
        error = "Failed to create " + text(dst) + ": " + ec.message();
        return false;
    }

    std::string copyError;
    const WalkResult copied = walk(srcAdj,
        [&](const fs::directory_entry& entry, const fs::path& rel) {
            if (keepIdeOutput && isIdeOutput(entry.path())) return DirAction::Skip;
            std::error_code dirError;
            fs::create_directories(dstAdj / rel, dirError);
            if (dirError && copyError.empty()) copyError = "Failed to create " + text(dst / rel) + ": " + dirError.message();
            return copyError.empty() ? DirAction::Descend : DirAction::Skip;
        },
        [&](const fs::directory_entry& entry, const fs::path& rel) {
            if (!copyError.empty()) return false;
            if (keepIdeOutput && isIdeOutput(entry.path())) return true;
            const fs::path target = dstAdj / rel;
            std::error_code status;
            if (fs::is_regular_file(target, status) && sameContents(entry.path(), target)) {
#if !defined(_WIN32)
                // an older export made by an editor that lost the execute bits keeps the
                // file's time for the incremental build but must still run as ./gradlew
                const fs::perms wanted = entry.status(status).permissions() & fs::perms::mask;
                if (!status && (fs::status(target, status).permissions() & fs::perms::mask) != wanted) {
                    fs::permissions(target, wanted, fs::perm_options::replace, status);
                    if (status) {
                        copyError = "Failed to set the permissions of " + text(dst / rel) + ": " + status.message();
                        return false;
                    }
                }
#endif
                return true;
            }
            if (fs::exists(fs::symlink_status(target, status)) && !fs::is_regular_file(fs::symlink_status(target, status))) {
                if (!removeNoFollow(target, copyError)) return false;
            }
            return copyFile(entry.path(), target, copyError);
        });
    if (!copyError.empty()) {
        error = copyError;
        return false;
    }
    if (!copied.ok) {
        error = copied.error;
        return false;
    }
    if (!copied.unreadable.empty()) {
        error = "Cannot read the folder " + text(copied.unreadable.front());
        return false;
    }

    // what the source no longer has (renamed or removed in an SDK update) would still be
    // compiled by the exported build's recursive globs
    std::vector<fs::path> stale;
    const WalkResult scanned = walk(dstAdj,
        [&](const fs::directory_entry& entry, const fs::path& rel) {
            if (keepIdeOutput && isIdeOutput(entry.path())) return DirAction::Skip;
            std::error_code status;
            if (!fs::exists(srcAdj / rel, status)) {
                stale.push_back(entry.path());
                return DirAction::Skip;
            }
            return DirAction::Descend;
        },
        [&](const fs::directory_entry& entry, const fs::path& rel) {
            if (keepIdeOutput && isIdeOutput(entry.path())) return true;
            std::error_code status;
            if (!fs::exists(srcAdj / rel, status)) stale.push_back(entry.path());
            return true;
        });
    if (!scanned.ok) {
        error = scanned.error;
        return false;
    }
    for (const fs::path& path : stale) {
        if (!removeNoFollow(path, error)) return false;
    }
    return true;
}

}
