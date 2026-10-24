// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TempDir.h"

#include "TestPaths.h"

#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#include <process.h>
#define JASPER_TEST_GETPID _getpid
#else
#include <unistd.h>
#define JASPER_TEST_GETPID getpid
#endif

using namespace jasperengine;
using namespace jasperengine::test;

namespace {
    std::atomic<unsigned> tempDirCounter{0};

    // test names may carry '/' (parameterized suites) or other characters no file system likes
    std::string safeName(std::string name){
        for (char& c : name){
            const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
            if (!keep)
                c = '_';
        }
        return name;
    }
}

TempDir::TempDir(){
    std::string name = "no-test";
    if (const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info())
        name = std::string(info->test_suite_name()) + "." + info->name();

    std::ostringstream leaf;
    leaf << safeName(name) << "-" << JASPER_TEST_GETPID() << "-" << tempDirCounter++;

    root = std::filesystem::path(testPaths().binaryDir) / "test-tmp" / leaf.str();
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    if (error)
        ADD_FAILURE() << "TempDir: cannot create " << root.string() << ": " << error.message();
}

TempDir::~TempDir(){
    if (envFlag("JASPER_TEST_KEEP_TEMP"))
        return;
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

const std::filesystem::path& TempDir::path() const{
    return root;
}

std::string TempDir::str() const{
    return root.generic_string();
}

std::filesystem::path TempDir::file(const std::string& relative) const{
    std::filesystem::path full = root / std::filesystem::path(relative);
    std::error_code error;
    std::filesystem::create_directories(full.parent_path(), error);
    return full;
}

std::filesystem::path TempDir::write(const std::string& relative, const std::string& content) const{
    std::filesystem::path full = file(relative);
    std::ofstream out(full, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out)
        ADD_FAILURE() << "TempDir: cannot write " << full.string();
    return full;
}

std::string TempDir::read(const std::string& relative) const{
    std::ifstream in(root / std::filesystem::path(relative), std::ios::binary);
    if (!in)
        return {};
    std::ostringstream content;
    content << in.rdbuf();
    return content.str();
}

bool TempDir::exists(const std::string& relative) const{
    std::error_code error;
    return std::filesystem::exists(root / std::filesystem::path(relative), error);
}
