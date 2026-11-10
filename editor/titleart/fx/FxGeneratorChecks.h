// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S83-title-effects-generators.md R6, R43, R46, §6.11)
// - Purpose: check_title's generator checks, registered beside S80's built-ins
//   (registerBuiltinTitleChecks calls registerTitleGeneratorChecks): `fx_follow_missing`, a
//   following generator whose layer is gone or was never picked (it draws nothing), with fix ops
//   that point it at the nearest layer or remove it.
// - Thread-safety: main thread (registration once; the checks run where check_title runs).

#pragma once

#include "titleart/TitleArtDoc.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    void registerTitleGeneratorChecks();

    // the generator layers of `doc` whose `follow` is required and empty, or names a layer the
    // document lacks: {generator layer id, its follow value}
    struct TitleFollowProblem {
        std::string layerId;
        std::string follow;
    };
    std::vector<TitleFollowProblem> titleFollowProblems(const titleart::TitleArtDoc& doc);

}
