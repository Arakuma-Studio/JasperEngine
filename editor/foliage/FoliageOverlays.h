// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// T5 R51, R57: T1's terrain overlays gain "density" (the overlay's layer, a detail layer) and
// "tree_density" (trees per square unit), both heat maps from blue to red over the largest value.

namespace jasperengine::editor {

    // Registers the two sources; again replaces them
    void registerFoliageOverlays();

}
