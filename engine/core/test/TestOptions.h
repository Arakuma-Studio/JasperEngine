// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A case's options (docs/specs/project-test-runner.md R5, §6.3) over the file's Test.config and
// tests.yaml: the same rules for the static listing (the literals it read) and a loaded file.

#include "Export.h"
#include "test/TestTypes.h"

#include <string>

namespace jasperengine::gametest {

    // R20's refusal of the project's Play data
    inline constexpr const char* PROJECT_DATA_REFUSED =
        "tests never use the project's Play data: save a snapshot (Game view > Play Data > Save Snapshot...) and use data = \"<name>\"";

    // spec gets settings, then config, then options (tags add up); false + error for a refused
    // or malformed value (the case is then an error)
    JASPERENGINE_API bool applyOptions(CaseSpec& spec, const Settings& settings, const Json& config, const Json& options,
                                       std::string& error);

    // R22: [A-Za-z0-9_-]{1,64}
    JASPERENGINE_API bool isSnapshotName(const std::string& name);

}
