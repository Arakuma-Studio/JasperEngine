// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Whether this process may create symbolic links (Windows without Developer Mode or the privilege
// may not): the symlink tests skip without it (GTEST_SKIP in the parent, before any child runs).
#pragma once

#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#define JASPER_TEST_GETPID _getpid
#else
#include <unistd.h>
#define JASPER_TEST_GETPID getpid
#endif

namespace jasperengine::editor::tests {

    inline bool symlinksPermitted() {
        static const bool permitted = [] {
            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::path base = fs::temp_directory_path(ec) / ("jasper-symlink-probe-" + std::to_string(JASPER_TEST_GETPID()));
            fs::remove_all(base, ec);
            fs::create_directories(base, ec);
            std::error_code link;
            fs::create_directory_symlink(base, base / "dir-link", link);
            std::error_code file;
            fs::create_symlink(base / "missing", base / "file-link", file);
            fs::remove_all(base, ec);
            return !link && !file;
        }();
        return permitted;
    }

}

#define SKIP_WITHOUT_SYMLINKS()                                                                \
    do {                                                                                       \
        if (!::jasperengine::editor::tests::symlinksPermitted())                               \
            GTEST_SKIP() << "this process may not create symbolic links";                      \
    } while (0)
