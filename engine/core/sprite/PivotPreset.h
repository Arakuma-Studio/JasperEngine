// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PIVOT_PRESET_H
#define PIVOT_PRESET_H

// The sprite pivot presets alone: the sheet and import settings that only store a preset include
// this, not component/SpriteComponent.h (tools/check_fanout.py)

namespace jasperengine{

    enum class PivotPreset{
        CENTER,
        TOP_CENTER,
        BOTTOM_CENTER,
        LEFT_CENTER,
        RIGHT_CENTER,
        TOP_LEFT,
        BOTTOM_LEFT,
        TOP_RIGHT,
        BOTTOM_RIGHT,
        // the frame's own pivot (sheet frames: the Aseprite slice, the import settings); appended
        FRAME
    };

}

#endif //PIVOT_PRESET_H
