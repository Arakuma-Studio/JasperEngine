// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The asset root of the animation clip tests (jasperengine-core-tests, engine/tests/core/animation):
// while one of them runs, the test System's asset-relative paths resolve under a fresh temporary
// asset directory (AnimTestEnvironment.cpp sets it around each test of this folder), so tests can
// write .anim files and load them through both the asset-relative and the absolute form.

#ifndef ANIM_TEST_ENVIRONMENT_H
#define ANIM_TEST_ENVIRONMENT_H

#include <string>
#include <vector>

namespace animtest {

    // the asset root (absolute, generic separators, no trailing '/')
    const std::string& assetDir();
    // writes `text` to assetDir()/relativePath (directories created); returns the absolute path
    std::string writeAsset(const std::string& relativePath, const std::string& text);
    // the warnings logged (Log::warn) since the test began or the last call, oldest first
    std::vector<std::string> takeWarnings();

}

#endif
