// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pixelart/PixelPerfectLayout.h"

#include <algorithm>
#include <cmath>

namespace jasperengine::pixelart {

    namespace {
        // floor division that rounds towards minus infinity (a crop's negative margin)
        int floorHalf(int value){
            return value >= 0 ? value / 2 : -((-value + 1) / 2);
        }
    }

    bool PixelPerfectLayout::operator==(const PixelPerfectLayout& other) const{
        return scale == other.scale && baseWidth == other.baseWidth && baseHeight == other.baseHeight &&
               targetWidth == other.targetWidth && targetHeight == other.targetHeight &&
               viewport.getX() == other.viewport.getX() && viewport.getY() == other.viewport.getY() &&
               viewport.getWidth() == other.viewport.getWidth() && viewport.getHeight() == other.viewport.getHeight() &&
               canvasWidth == other.canvasWidth && canvasHeight == other.canvasHeight && cropped == other.cropped;
    }

    int fitScale(int screenWidth, int screenHeight, int referenceWidth, int referenceHeight){
        if (referenceWidth <= 0 || referenceHeight <= 0 || screenWidth <= 0 || screenHeight <= 0)
            return 1;
        return std::max(1, std::min(screenWidth / referenceWidth, screenHeight / referenceHeight));
    }

    PixelPerfectLayout computePixelPerfectLayout(int screenWidth, int screenHeight, int referenceWidth,
            int referenceHeight, PixelUpscaleMode mode, int forcedScale, bool smoothing, int pixelsPerUnit){
        PixelPerfectLayout layout;
        layout.viewport = Rect(0.0f, 0.0f, 0.0f, 0.0f);
        if (referenceWidth <= 0 || referenceHeight <= 0 || screenWidth <= 0 || screenHeight <= 0)
            return layout;
        const int ppu = std::max(1, pixelsPerUnit);

        int k = fitScale(screenWidth, screenHeight, referenceWidth, referenceHeight);
        if (forcedScale > 0)
            k = std::min(k, forcedScale);
        layout.scale = k;
        layout.cropped = screenWidth < referenceWidth || screenHeight < referenceHeight;

        layout.baseWidth = referenceWidth;
        layout.baseHeight = referenceHeight;
        if (mode == PixelUpscaleMode::INTEGER_EXPAND){
            layout.baseWidth = std::max(referenceWidth, screenWidth / k);
            layout.baseHeight = std::max(referenceHeight, screenHeight / k);
        }

        if (mode == PixelUpscaleMode::STRETCH_FILL && !layout.cropped){
            // the aspect-kept fit of the reference, in whole screen pixels
            const float fit = std::min((float)screenWidth / (float)referenceWidth, (float)screenHeight / (float)referenceHeight);
            const int width = std::min(screenWidth, (int)std::lround((float)referenceWidth * fit));
            const int height = std::min(screenHeight, (int)std::lround((float)referenceHeight * fit));
            layout.viewport = Rect((float)((screenWidth - width) / 2), (float)((screenHeight - height) / 2), (float)width, (float)height);
        }else{
            const int width = k * layout.baseWidth;
            const int height = k * layout.baseHeight;
            layout.viewport = Rect((float)floorHalf(screenWidth - width), (float)floorHalf(screenHeight - height), (float)width, (float)height);
        }

        layout.targetWidth = layout.baseWidth + (smoothing ? 2 : 0);
        layout.targetHeight = layout.baseHeight + (smoothing ? 2 : 0);
        layout.canvasWidth = (float)layout.baseWidth / (float)ppu;
        layout.canvasHeight = (float)layout.baseHeight / (float)ppu;
        return layout;
    }

}
