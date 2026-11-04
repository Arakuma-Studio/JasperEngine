// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ORDERINDEPENDENTTRANSPARENCY_H
#define ORDERINDEPENDENTTRANSPARENCY_H

#include "Export.h"

namespace jasperengine {

    // Camera > Transparency Mode (docs/specs/S57-forge-lighting.md R4). Stored as ints: append only.
    enum class TransparencyMode : int {
        SORTED,             // blended renderers back to front, one at a time (Unity's)
        ORDER_INDEPENDENT   // alpha-blended materials through weighted blended OIT
    };

    // Weighted blended order-independent transparency (McGuire and Bavoil, JCGT 2013): what a
    // device needs for it and the weight includes/oit.glsl gives a fragment
    class JASPERENGINE_API OrderIndependentTransparency {
    public:
        // RGBA16F and R16F colour attachments rendered and blended into (and multisampled, for a
        // camera drawing with MSAA): without them a camera asking for it draws Sorted
        static bool isSupported(int samples);
        // tests and the editor's device simulation: an answer in place of the device's (null:
        // the device's again)
        static void setSupportOverride(const bool* supported);

        // McGuire and Bavoil's depth weight (equation 7 without alpha) at a view depth in metres,
        // the camera's weight scale multiplying the depth (includes/oit.glsl oitDepthWeight)
        static float depthWeight(float viewDepth, float scale);

        // lower-case names, as saved and as MCP and Lua take them: sorted, order_independent
        static const char* getModeName(TransparencyMode mode);
        static bool modeFromName(const char* name, TransparencyMode& mode);
    };

}

#endif // ORDERINDEPENDENTTRANSPARENCY_H
