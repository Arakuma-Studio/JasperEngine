// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "hub/StagingFolders.h"

#include "io/AtomicFile.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;

namespace jasperengine::editor::hub::staging {

void holdForTest() {
    const char* hold = std::getenv("JASPER_TEST_STAGING_HOLD");
    if (!hold || !*hold) return;
    const fs::path path(hold);
    std::error_code ec;
    if (!fs::exists(path, ec)) return;
    io::writeFileAtomic(fs::path(path.string() + ".held"), "held\n");
    while (fs::exists(path, ec)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

}
