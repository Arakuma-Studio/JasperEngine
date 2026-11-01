// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The camera shake Inspector's pure parts (InspectorWindowCamera.cpp), for the tests

#pragma once

#include "camera/ShakePreset.h"

#include <filesystem>
#include <string>

namespace jasperengine::editor {

    class Project;

    namespace inspectorcamera {

        // Writes `file` canonically to `path` as one step of the project history (the .shake asset
        // view's edits); true without a step when the file already holds that text. False and
        // `error` when it can't be written
        bool writeShakePresetFile(Project* project, const std::filesystem::path& path, const camera::ShakePresetFile& file,
                                  std::string& error);

    }

}
