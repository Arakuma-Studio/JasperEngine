// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteNormalsCheck.h"

namespace fs = std::filesystem;

namespace jasperengine::editor::spritenormals {

    NormalsCheck checkSpriteNormals(const fs::path&, const std::vector<fs::path>&) {
        return NormalsCheck();
    }

    std::vector<fs::path> cachedMapsOf(const fs::path&, const fs::path&) {
        return {};
    }

}
