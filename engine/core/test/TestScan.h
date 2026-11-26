// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The static listing of a test root (docs/specs/project-test-runner.md R2, R3): every
// *.test.lua under it, and in each the cases written as Test.case("<literal>" ...), with the
// options given as literals (and the file's Test.config{...} literals as their defaults). A text
// scan: no project code runs to list.

#include "Export.h"
#include "test/TestTypes.h"

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::gametest {

    struct ScannedFile {
        std::string file;                 // root-relative, forward slashes
        std::vector<CaseSpec> cases;
    };

    class JASPERENGINE_API TestScan {
    public:
        // the root's *.test.lua files (recursively, sorted), skipping Golden/ and Data/
        static std::vector<std::string> listFiles(const std::filesystem::path& root);
        // one file's text; `settings` gives the defaults
        static ScannedFile scanText(const std::string& code, const std::string& file, const Settings& settings);
        // every file of the root, read from disk
        static std::vector<ScannedFile> scan(const std::filesystem::path& root, const Settings& settings);
        // the cases of every file, in file order
        static std::vector<CaseSpec> cases(const std::vector<ScannedFile>& files);
    };

}
