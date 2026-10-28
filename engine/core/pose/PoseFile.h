// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Reading and writing the pose files (.poseanim, .humanoid). The only pose unit that touches the
// engine file API, and so the engine's System; parse and write of the formats are in
// PoseAnimationIO.h and HumanoidMapIO.h.
//
// Reads go through Data (project and asset paths, the resource pack of an exported game), so
// they work on every platform. Writes are for the editor (desktop): the engine File API writes a
// temporary file next to the target, which is then renamed over it.

#ifndef POSE_FILE_H
#define POSE_FILE_H

#include "Export.h"
#include "pose/HumanoidMapIO.h"
#include "pose/PoseAnimationIO.h"

#include <optional>
#include <string>
#include <vector>

namespace jasperengine::pose {

    // Whole file as text. When the file does not exist `*notFound` (optional) is set and false is
    // returned; a file that exists but cannot be read is an error without `notFound`.
    JASPERENGINE_API bool readTextFile(const std::string& path, std::string& text, std::string* error,
                                       bool* notFound = nullptr);
    JASPERENGINE_API bool writeTextFile(const std::string& path, const std::string& text, std::string* error);

    JASPERENGINE_API bool loadPoseAnimationFile(const std::string& path, PoseAnimationData& out,
                                                std::vector<std::string>& warnings, std::string* error,
                                                std::optional<PoseAnimationTemplateInfo>* templateInfo = nullptr);
    JASPERENGINE_API bool savePoseAnimationFile(const std::string& path, const PoseAnimationData& data,
                                                std::string* error,
                                                const PoseAnimationTemplateInfo* templateInfo = nullptr);

    // A missing sidecar is not an error: `out` is all-auto, true is returned and `*notFound`
    // (optional) is set. A sidecar that exists but is unreadable or corrupt fails with `*error`.
    JASPERENGINE_API bool loadHumanoidOverridesFile(const std::string& path, HumanoidOverrides& out,
                                                    std::vector<std::string>& warnings, std::string* error,
                                                    bool* notFound = nullptr);
    JASPERENGINE_API bool saveHumanoidOverridesFile(const std::string& path, const HumanoidOverrides& overrides,
                                                    std::string* error);

}

#endif // POSE_FILE_H
