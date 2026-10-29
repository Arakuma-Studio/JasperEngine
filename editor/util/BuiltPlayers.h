// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace jasperengine::editor::builtplayers {

    // The identity of a built player: run_build runs only a file that still matches what the editor
    // recorded right after building it. Every field must match: the resolved path, the device and
    // inode (POSIX; 0 on Windows), the size, the modification time (POSIX: st_mtim in nanoseconds;
    // Windows: last_write_time ticks) and the SHA-256 of the bytes.
    struct Fingerprint {
        std::filesystem::path resolved;
        std::uint64_t dev = 0, ino = 0, size = 0;
        std::int64_t mtime = 0;
        std::string sha256;
        bool valid = false;
    };

    // The fingerprint of `exe` as it is now (its links resolved); valid=false when it can't be read.
    Fingerprint fingerprintOf(const std::filesystem::path& exe);

    // <internal folder>/user/runbuild.yaml: one entry per mode ("desktop" ...), keys resolved, dev,
    // ino, size, mtime, sha256. The editor's own file (agents can't write .jasperengine).
    std::filesystem::path recordFile(const std::filesystem::path& internalDir);

    // Records `exe` (as it is now) as the player built for `mode`. False when it can't be read or
    // the record can't be written.
    bool record(const std::filesystem::path& internalDir, const std::filesystem::path& exe, const std::string& mode);

    // The recorded fingerprint whose resolved path is `resolvedExe` (any mode); valid=false none.
    Fingerprint recorded(const std::filesystem::path& internalDir, const std::filesystem::path& resolvedExe);

    // "" when `exe` may run: not a symbolic link, recorded, and every recorded field equals the file
    // now. Otherwise why not.
    std::string verify(const std::filesystem::path& internalDir, const std::filesystem::path& exe);

}
