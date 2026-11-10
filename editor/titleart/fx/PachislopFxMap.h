// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S83-title-effects-generators.md R51, §6.9, D16)
// - Purpose: Pachislop's effect types and param keys mapped to Title Art's, read from the data file
//   editor/resources/titleart/pachislop-fx-map.yaml (embedded: title_pachislop_fx_map.h). S84's
//   look converter and S87's importers convert an effect instance with convertPachislopEffect:
//   keys renamed, x/y pairs joined into Vec2s, angles negated where the conventions differ, the
//   params Pachislop had no key for set.
// - Thread-safety: the map is parsed once on first use; reads from any thread.

#pragma once

#include "titleart/TitleArtDoc.h"

#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor {

    struct PachislopFxEntry {
        std::string type;                                       // ours
        std::map<std::string, std::string> params;              // Pachislop key -> ours
        std::map<std::string, std::vector<std::string>> vec2;   // our Vec2 param -> its [x, y] Pachislop keys
        std::vector<std::string> negate;                        // Pachislop keys whose sign flips
        titleart::Json set = titleart::Json::object();          // our params Pachislop had no key for
    };

    // every Pachislop effect type the map knows, by its Pachislop name
    const std::map<std::string, PachislopFxEntry>& pachislopEffectMap();

    struct PachislopFxConversion {
        bool ok = false;
        std::string type;
        titleart::Json params = titleart::Json::object();
        std::string error;                                      // an unknown type or key
    };
    // a Pachislop effect instance's params as ours (option values camelCase -> snake_case)
    PachislopFxConversion convertPachislopEffect(const std::string& pachislopType, const titleart::Json& params);

}
