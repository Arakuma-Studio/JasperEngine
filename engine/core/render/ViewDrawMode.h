// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VIEWDRAWMODE_H
#define VIEWDRAWMODE_H

#include <cstdint>

namespace jasperengine{

    // The Scene view's Draw Mode (spec S54 R11), editor views only: Wireframe draws the meshes'
    // wires alone (the editor's line overlay, SceneRender), Shaded Wireframe both, LOD Level
    // tints each mesh by the level it draws (the two of a fade dithered: striped), Overdraw adds
    // a constant heat per layer, without depth
    enum class ViewDrawMode : uint8_t{
        SHADED,
        WIREFRAME,
        SHADED_WIREFRAME,
        LOD_LEVEL,
        OVERDRAW
    };

}

#endif //VIEWDRAWMODE_H
