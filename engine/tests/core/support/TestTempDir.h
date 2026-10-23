// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A directory unique to one test, removed with everything in it when the object goes away

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#if defined(_WIN32)
    #include <process.h>
#else
    #include <unistd.h>
#endif

namespace jasperengine::test {

    class TestTempDir {
    public:
        explicit TestTempDir(const std::string& name = "test"){
            static std::atomic<unsigned> counter{0};
#if defined(_WIN32)
            const long pid = (long)_getpid();
#else
            const long pid = (long)::getpid();
#endif
            root = std::filesystem::temp_directory_path() /
                    ("jasper-" + name + "-" + std::to_string(pid) + "-" + std::to_string(counter++));
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
            std::filesystem::create_directories(root);
        }

        ~TestTempDir(){
            std::error_code ec;
            // read-only files left by a test must not keep the directory alive
            for (auto it = std::filesystem::recursive_directory_iterator(root, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)){
                std::error_code permEc;
                std::filesystem::permissions(it->path(), std::filesystem::perms::owner_write, std::filesystem::perm_options::add, permEc);
            }
            std::filesystem::remove_all(root, ec);
        }

        TestTempDir(const TestTempDir&) = delete;
        TestTempDir& operator=(const TestTempDir&) = delete;

        const std::filesystem::path& path() const { return root; }
        std::filesystem::path operator/(const std::string& relative) const { return root / relative; }

        static std::string read(const std::filesystem::path& file){
            std::ifstream in(file, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }

        // Test fixtures are written plainly: the code under test is what writes atomically
        static void write(const std::filesystem::path& file, const std::string& bytes){
            std::filesystem::create_directories(file.parent_path());
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out.write(bytes.data(), (std::streamsize)bytes.size());
        }

    private:
        std::filesystem::path root;
    };

}
