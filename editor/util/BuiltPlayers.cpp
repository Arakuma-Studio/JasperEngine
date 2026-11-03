// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "BuiltPlayers.h"

#include "util/FileUtils.h"
#include "util/SafeWrite.h"
#include "htmlui/compat/Hash.h"
#include "yaml-cpp/yaml.h"

#include <fstream>
#include <iterator>
#include <system_error>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

namespace jasperengine::editor::builtplayers {

    Fingerprint fingerprintOf(const fs::path& exe) {
        Fingerprint f;
        f.resolved = safewrite::resolvedPath(exe);
        if (f.resolved.empty()) return f;
        std::error_code ec;
        if (!fs::is_regular_file(f.resolved, ec)) return f;
#ifdef _WIN32
        f.size = static_cast<std::uint64_t>(fs::file_size(f.resolved, ec));
        f.mtime = static_cast<std::int64_t>(fs::last_write_time(f.resolved, ec).time_since_epoch().count());
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
        if (was.dev != now.dev || was.ino != now.ino) return "it is not the file that was built (replaced since)";
        if (was.mtime != now.mtime) return "it was modified since it was built";
        return std::string();
    }

}
