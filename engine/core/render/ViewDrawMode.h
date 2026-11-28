// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VIEWDRAWMODE_H
#define VIEWDRAWMODE_H

#include <cstdint>

// The Draw Modes are drawn in the editor, and in the unit tests' engine that checks them
// (JASPERENGINE_VIEW_DRAW_MODES, engine/CMakeLists.txt): a game has none of their code
#if defined(JASPERENGINE_EDITOR) || defined(JASPERENGINE_VIEW_DRAW_MODES)
    #define JE_VIEW_DRAW_MODES 1
#endif

namespace jasperengine{

    // The Scene view's Draw Mode (spec S54 R11), editor views only: Wireframe draws the meshes'
    // wires alone (the editor's line overlay, SceneRender), Shaded Wireframe both, LOD Level
    // tints each mesh by the level it draws (the two of a fade dithered: striped), Overdraw adds
    // a constant heat per layer, without depth, Vertex Color draws each mesh unlit in its raw
    // vertex colour (V1 R102: ViewVertexColorChannel)
    enum class ViewDrawMode : uint8_t{
        SHADED,
        WIREFRAME,
        SHADED_WIREFRAME,
        LOD_LEVEL,
        OVERDRAW,
        VERTEX_COLOR
    };

    // What the Vertex Color Draw Mode shows: the colour, or one channel as grey (A over a
    // checker, so where it is 0 shows). Stored as ints: append only
    enum class ViewVertexColorChannel : uint8_t{
        RGB,
        R,
        G,
        B,
        A
    };

}

#endif //VIEWDRAWMODE_H
