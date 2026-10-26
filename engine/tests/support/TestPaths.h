// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JASPER_TEST_PATHS_H
#define JASPER_TEST_PATHS_H

#include <string>

namespace jasperengine { namespace test {

    // The directories of the running test binary. TestMain.cpp is compiled into every test
    // target with that target's JASPER_TEST_* definitions (jasperengine_add_test_target) and
    // sets them before anything else runs, so this library is built once per tree.
    struct TestPaths {
        std::string sourceDir;   // the target's test sources (JASPER_TEST_SOURCE_DIR)
        std::string binaryDir;   // its build directory (JASPER_TEST_BINARY_DIR): userdata, temp dirs
        std::string assetDir;    // TestSystem's asset and Lua root (JASPER_TEST_ASSET_DIR)
        std::string goldenDir;   // golden images (<source>/golden)
        std::string outputDir;   // images written on a golden mismatch (JASPER_TEST_OUTPUT_DIR)
    };

    void setTestPaths(const TestPaths& paths);
    const TestPaths& testPaths();

    // true when the environment variable is "1"
    bool envFlag(const char* name);

} }

#endif //JASPER_TEST_PATHS_H
