// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef CAMERA_TEST_ENVIRONMENT_H
#define CAMERA_TEST_ENVIRONMENT_H

#include <string>
#include <vector>

namespace cameratest {

    // the asset root (absolute, generic separators, no trailing '/')
    const std::string& assetDir();
    // writes `text` to assetDir()/relativePath (directories created); returns the absolute path
    std::string writeAsset(const std::string& relativePath, const std::string& text);
    // the warnings logged (Log::warn) since the last call, oldest first; clears them
    std::vector<std::string> takeWarnings();

}

#endif
