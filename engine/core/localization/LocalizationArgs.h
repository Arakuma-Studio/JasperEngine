// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LOCALIZATION_ARGS_H
#define LOCALIZATION_ARGS_H

// The arguments of a localised message (docs/specs/game-ui-localization.md R5): name -> a string,
// an integer or a real. A string "@ns.key" is replaced by that key's text first (one level).

#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace jasperengine {

    using LocalizationValue = std::variant<std::string, int64_t, double>;
    using LocalizationArgs = std::vector<std::pair<std::string, LocalizationValue>>;

}

#endif //LOCALIZATION_ARGS_H
