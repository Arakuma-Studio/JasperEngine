// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JASPER_TEST_TEMP_DIR_H
#define JASPER_TEST_TEMP_DIR_H

#include <filesystem>
#include <string>

namespace jasperengine { namespace test {

    // A fresh directory for one test, removed again when it goes out of scope:
    // <binary>/test-tmp/<Suite>.<Test>-<pid>-<n>, so tests running in parallel processes
    // (ctest -j, gtest sharding) never share one. JASPER_TEST_KEEP_TEMP=1 keeps them all.
    class TempDir {
    public:
        TempDir();
        ~TempDir();

        TempDir(const TempDir&) = delete;
        TempDir& operator=(const TempDir&) = delete;

        const std::filesystem::path& path() const;
        std::string str() const;     // path() with '/' separators (test paths are ASCII)

        // <path>/<relative>, its parent directories created
        std::filesystem::path file(const std::string& relative) const;
        // writes (replaces) <path>/<relative> with the bytes of `content`, returns its path
        std::filesystem::path write(const std::string& relative, const std::string& content) const;
        // the bytes of <path>/<relative>; empty when it cannot be read
        std::string read(const std::string& relative) const;
        bool exists(const std::string& relative) const;

    private:
        std::filesystem::path root;
    };

} }

#endif //JASPER_TEST_TEMP_DIR_H
