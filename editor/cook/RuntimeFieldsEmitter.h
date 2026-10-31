// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The runtime field tables of the cooked scenes (docs/specs/S30-shipping.md R4): every saved
// field of every component and struct of the reflection tables, as the C++ the player compiles
// (engine/core/serialization/RuntimeFields.gen.inc). `jasperengine-editor reflect
// --emit-runtime-fields <file>` writes it; CI and RuntimeFields.Generated check that the
// committed file is what the tables give now.

#include <string>
#include <vector>

namespace jasperengine::editor::cook {

    // the whole file; problems (a field the tables cannot name as a member) in `errors`
    std::string emitRuntimeFields(std::vector<std::string>& errors);

    // the field id of a struct's field: FNV-1a 32 of "<owner>.<yamlKey>" (owner: the component's
    // key for its body, the struct's name for a nested one)
    uint32_t runtimeFieldId(const std::string& owner, const std::string& yamlKey);

}
