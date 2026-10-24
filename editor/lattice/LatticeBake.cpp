// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// CONTRACT STUB: WP-BAKE replaces every body (spec §9.4).

#include "LatticeEditorOps.h"

using namespace jasperengine;
using namespace jasperengine::editor;

bool LatticeEditorOps::bake(Project*, uint32_t, Entity, std::string*, std::string*, std::string* error){
    if (error) *error = "lattice baking is not implemented yet";
    return false;
}

bool LatticeEditorOps::canBake(Scene*, Entity, std::string* reason){
    if (reason) *reason = "lattice baking is not implemented yet";
    return false;
}
