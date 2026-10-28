// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The name indexes of the tables (docs/specs/S08-reflection.md R18, R20): per struct, its field
// names and aliases sorted, for path resolution without allocation. Built once for every
// registered component's structs.

#include "reflect/Reflect.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace jasperengine::editor::reflect {

    struct NameIndex {
        struct Entry {
            std::string_view name;
            uint16_t field;
            bool alias;
        };
        std::vector<Entry> entries;
    };

    const NameIndex& nameIndex(const StructDesc& desc);
    // builds the registered structs' indexes (once) and returns how many there are
    size_t buildNameIndexes();
    // their memory
    size_t nameIndexBytes();

}
