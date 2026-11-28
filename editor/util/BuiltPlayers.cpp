// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "BuiltPlayers.h"

#include "util/FileUtils.h"
#include "util/SafeWrite.h"
#include "htmlui/compat/Hash.h"
#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <vector>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

namespace jasperengine::editor::builtplayers {

    namespace {
        // a folder's identity (an .app): its files in order, each with its hash
        Fingerprint treeFingerprint(Fingerprint f) {
            std::error_code ec;
            std::vector<fs::path> files;
            for (fs::recursive_directory_iterator it(f.resolved, fs::directory_options::none, ec), end; it != end; it.increment(ec)) {
                if (ec) return f;
                if (it->is_regular_file(ec)) files.push_back(it->path());
            }
            std::sort(files.begin(), files.end());
            std::string listing;
            for (const fs::path& file : files) {
                std::ifstream in(file, std::ios::binary);
                if (!in) return f;
                const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                f.size += bytes.size();
                const auto written = fs::last_write_time(file, ec).time_since_epoch().count();
                f.mtime = std::max<std::int64_t>(f.mtime, static_cast<std::int64_t>(written));
                listing += FileUtils::pathToGenericUtf8(file.lexically_relative(f.resolved)) + "\n" +
                           jasperengine::htmlui::compat::sha256Hex(bytes) + "\n";
            }
            f.sha256 = jasperengine::htmlui::compat::sha256Hex(listing);
            f.valid = !files.empty() && !f.sha256.empty();
            return f;
        }
    }

    Fingerprint fingerprintOf(const fs::path& exe) {
        Fingerprint f;
        f.resolved = safewrite::resolvedPath(exe);
        if (f.resolved.empty()) return f;
        std::error_code ec;
        if (fs::is_directory(f.resolved, ec)) {
#ifndef _WIN32
            struct stat st{};
            if (::stat(f.resolved.c_str(), &st) != 0) return f;
            f.dev = static_cast<std::uint64_t>(st.st_dev);
            f.ino = static_cast<std::uint64_t>(st.st_ino);
#endif
            return treeFingerprint(f);
        }
        if (!fs::is_regular_file(f.resolved, ec)) return f;
#ifdef _WIN32
        f.size = static_cast<std::uint64_t>(fs::file_size(f.resolved, ec));
        f.mtime = static_cast<std::int64_t>(fs::last_write_time(f.resolved, ec).time_since_epoch().count());
        // the file's identity, as st_dev / st_ino: its volume and its file index on it
        const HANDLE file = CreateFileW(f.resolved.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (file == INVALID_HANDLE_VALUE) return f;
        BY_HANDLE_FILE_INFORMATION info{};
        const bool identified = GetFileInformationByHandle(file, &info) != FALSE;
        CloseHandle(file);
        if (!identified) return f;
        f.dev = info.dwVolumeSerialNumber;
        f.ino = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
#else
        struct stat st{};
        if (::stat(f.resolved.c_str(), &st) != 0) return f;
        f.dev = static_cast<std::uint64_t>(st.st_dev);
        f.ino = static_cast<std::uint64_t>(st.st_ino);
        f.size = static_cast<std::uint64_t>(st.st_size);
#if defined(__APPLE__)
        f.mtime = static_cast<std::int64_t>(st.st_mtimespec.tv_sec) * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
        f.mtime = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
#endif
#endif
        std::ifstream in(f.resolved, std::ios::binary);
        if (!in) return f;
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        f.sha256 = jasperengine::htmlui::compat::sha256Hex(bytes);
        f.valid = !f.sha256.empty();
        return f;
    }

    fs::path recordFile(const fs::path& internalDir) {
        return internalDir / "user" / "runbuild.yaml";
    }

    bool record(const fs::path& internalDir, const fs::path& exe, const std::string& mode) {
        const Fingerprint f = fingerprintOf(exe);
        if (!f.valid) return false;
        try {
            const fs::path file = recordFile(internalDir);
            YAML::Node root;
            std::error_code ec;
            if (fs::is_regular_file(file, ec)) {
                std::ifstream in(file, std::ios::binary);
                root = YAML::Load(in);
            }
            if (!root.IsMap()) root = YAML::Node(YAML::NodeType::Map);
            YAML::Node entry;
            entry["resolved"] = FileUtils::pathToUtf8(f.resolved);
            entry["dev"] = f.dev;
            entry["ino"] = f.ino;
            entry["size"] = f.size;
            entry["mtime"] = f.mtime;
            entry["sha256"] = f.sha256;
            root[mode] = entry;
            const fs::path dir = file.parent_path();
            if (!fs::is_directory(dir, ec)) {
                safewrite::EditorInternal makeDir(dir);
                FileUtils::createDirectories(dir, ec);
            }
            safewrite::EditorInternal ownRecord(file);   // the editor's own record (util/SafeWrite.h)
            return FileUtils::writeFileReplacing(file, YAML::Dump(root) + "\n");
        } catch (const std::exception&) {
            return false;
        }
    }

    fs::path recordedPath(const fs::path& internalDir, const std::string& mode) {
        try {
            const fs::path file = recordFile(internalDir);
            std::error_code ec;
            if (!fs::is_regular_file(file, ec)) return {};
            std::ifstream in(file, std::ios::binary);
            const YAML::Node root = YAML::Load(in);
            if (!root.IsMap() || !root[mode] || !root[mode]["resolved"]) return {};
            return FileUtils::pathFromUtf8(root[mode]["resolved"].as<std::string>());
        } catch (const std::exception&) {
            return {};
        }
    }

    Fingerprint recorded(const fs::path& internalDir, const fs::path& resolvedExe) {
        Fingerprint f;
        try {
            const fs::path file = recordFile(internalDir);
            std::error_code ec;
            if (!fs::is_regular_file(file, ec)) return f;
            std::ifstream in(file, std::ios::binary);
            const YAML::Node root = YAML::Load(in);
            if (!root.IsMap()) return f;
            for (const auto& kv : root) {
                const YAML::Node& e = kv.second;
                if (!e.IsMap() || !e["resolved"] || !e["sha256"]) continue;
                const fs::path path = FileUtils::pathFromUtf8(e["resolved"].as<std::string>()).lexically_normal();
                if (path != resolvedExe.lexically_normal()) continue;
                f.resolved = path;
                f.dev = e["dev"] ? e["dev"].as<std::uint64_t>() : 0;
                f.ino = e["ino"] ? e["ino"].as<std::uint64_t>() : 0;
                f.size = e["size"] ? e["size"].as<std::uint64_t>() : 0;
                f.mtime = e["mtime"] ? e["mtime"].as<std::int64_t>() : 0;
                f.sha256 = e["sha256"].as<std::string>();
                f.valid = !f.sha256.empty();
                return f;
            }
        } catch (const std::exception&) {
        }
        return f;
    }

    std::string verify(const fs::path& internalDir, const fs::path& exe) {
        std::error_code ec;
        if (fs::is_symlink(fs::symlink_status(exe, ec))) return "it is a symbolic link";
        const Fingerprint now = fingerprintOf(exe);
        if (!now.valid) return "it can't be read";
        const Fingerprint was = recorded(internalDir, now.resolved);
        if (!was.valid) return "it was not built by this editor for this project";
        if (was.size != now.size || was.sha256 != now.sha256) return "its contents changed since it was built";
#ifdef _WIN32
        // a record of an editor that kept no identity on Windows (0, 0): the other fields decide
        const bool identityRecorded = was.dev != 0 || was.ino != 0;
#else
        const bool identityRecorded = true;
#endif
        if (identityRecorded && (was.dev != now.dev || was.ino != now.ino)) return "it is not the file that was built (replaced since)";
        if (was.mtime != now.mtime) return "it was modified since it was built";
        return std::string();
    }

}
