// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LIGHTINGTIERS_H
#define LIGHTINGTIERS_H

#include "Export.h"

namespace jasperengine {

    // Project Settings > Graphics > Light Mode (docs/specs/S57-forge-lighting.md R1): how lit
    // meshes find their pixel lights. Stored as ints: append only.
    enum class LightMode : int {
        AUTOMATIC,   // Tiled on the Medium tier and above, Per Object on Low
        PER_OBJECT,  // each object's own list, capped by Max Lights Per Object (folded into SH past it)
        TILED        // screen-tile lists (Forward+): every light per pixel, up to 32 a tile
    };

    // The device tier the S57 lighting features scale with (screen-tile grid, contact shadows'
    // resolution, Light Mode Automatic): S56's device tier where one applies (Device::getTier),
    // else the platform's default (desktop High, web Medium, Android and iOS Low). Stored as
    // ints: append only.
    enum class LightingTier : int {
        LOW,
        MEDIUM,
        HIGH,
        ULTRA
    };

    class JASPERENGINE_API LightingTiers {
    public:
        // the default tier of a platform (Engine::Platform as an int: MacOS, iOS, Web, Android,
        // Linux, Windows)
        static LightingTier defaultTier(int platform);
        // the tier this build draws at: the one set, else the device's, else this platform's default
        static LightingTier getTier();
        static void setTier(LightingTier tier);
        static void resetTier();

        // the project's Light Mode (graphics.light_mode): set by the editor and by an exported
        // game's startup code, AUTOMATIC until then
        static void setLightMode(LightMode mode);
        static LightMode getLightMode();
        // whether a mode draws Tiled on a tier
        static bool usesTiledLights(LightMode mode, LightingTier tier);
        // this build's (the project's mode at the current tier)
        static bool usesTiledLights();

        // the screen-tile grid of a tier: 16x9, 32x18 at High and above
        static void tileGrid(LightingTier tier, int& columns, int& rows);
        // contact shadows' mask resolution on a tier: 1 full, 2 half, 0 off (Low)
        static int contactShadowDivisor(LightingTier tier);

        // lower-case names, as saved and as MCP and Lua take them
        static const char* getModeName(LightMode mode);   // automatic, per_object, tiled
        static bool modeFromName(const char* name, LightMode& mode);
        static const char* getTierName(LightingTier tier); // low, medium, high, ultra
        static bool tierFromName(const char* name, LightingTier& tier);
    };

}

#endif // LIGHTINGTIERS_H
