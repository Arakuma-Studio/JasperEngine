// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The .poseanim file (spec: pose animation phase 2, section 2.1): UTF-8 JSON, 2-space indent,
// keys in a fixed order, numbers rounded to 4 decimals. Writing is deterministic (the same data
// gives byte-identical text), so undo snapshots and diffs are stable.
//
// Parsing never fails on content problems: unknown keys, unknown DOF names, wrongly typed
// values and out-of-range values are ignored or clamped (PoseRanges) and reported as warnings.
// Only text that is not a JSON object, a wrong `format` or a `version` above 1 fails.
//
// A "transition" object of a pose makes it a custom transition; its missing fields take the
// Transition() defaults (not the animation's transition). "transition": null (or no key) uses the
// animation's transition.
//
// This unit has no engine dependency beyond the data model. Reading and writing files (the
// engine file API, which needs the engine's System) is in PoseFile.h.
//
// Templates (spec: phase 6, section 2) are .poseanim files with an extra
// "template": {"name", "description", "category"} object after `version`; it is returned
// through the optional `templateInfo` out-parameter and written back when one is given.

#ifndef POSE_ANIMATION_IO_H
#define POSE_ANIMATION_IO_H

#include "Export.h"
#include "pose/PoseTypes.h"

#include <optional>
#include <string>
#include <vector>

namespace jasperengine::pose {

    constexpr const char* POSEANIM_FORMAT = "jasperengine.poseanim";
    constexpr int POSEANIM_VERSION = 1;

    struct PoseAnimationTemplateInfo {
        std::string name;
        std::string description;
        std::string category;
    };

    // `out` starts from the defaults, keys missing in the text keep them. `templateInfo`
    // (optional) receives the "template" object, or nullopt when the file has none.
    JASPERENGINE_API bool parsePoseAnimation(const std::string& text, PoseAnimationData& out,
                                             std::vector<std::string>& warnings, std::string* error,
                                             std::optional<PoseAnimationTemplateInfo>* templateInfo = nullptr);

    // `templateInfo` (optional): writes the "template" object after `version`
    JASPERENGINE_API std::string writePoseAnimation(const PoseAnimationData& data,
                                                    const PoseAnimationTemplateInfo* templateInfo = nullptr);

}

#endif // POSE_ANIMATION_IO_H
