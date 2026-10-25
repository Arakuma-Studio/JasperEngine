// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The engine's System for the anim tests (started once per process by AnimTestEnvironment.cpp):
// asset-relative paths resolve under a fresh temporary asset directory, so tests can write
// .anim files and load them through both the asset-relative and the absolute form.

#ifndef ANIM_TEST_ENVIRONMENT_H
#define ANIM_TEST_ENVIRONMENT_H

#include <string>
#include <vector>

namespace animtest {

    // the asset root (absolute, generic separators, no trailing '/')
    const std::string& assetDir();
    // writes `text` to assetDir()/relativePath (directories created); returns the absolute path
    std::string writeAsset(const std::string& relativePath, const std::string& text);
    // the warnings logged (Log::warn) since the last call, oldest first; clears them
    std::vector<std::string> takeWarnings();

}

#endif
