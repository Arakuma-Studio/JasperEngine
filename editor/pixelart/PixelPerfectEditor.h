// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The editor's side of the Pixel Perfect Camera (docs/specs/S41-pixel-perfect-camera.md R19-R23):
// its layout for the Game view's size outside Play (the Inspector's Current Pixel Ratio, MCP's
// get_entity), the readout text, and the warnings the Inspector and agents see.

#include "Engine.h"
#include "ecs/Entity.h"
#include "pixelart/PixelPerfectLayout.h"

#include <string>
#include <vector>

namespace jasperengine {
    class Scene;
}

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::pixelart {

    // the size the Game view lays the game out at (the project canvas when no Game view drew yet)
    void gameViewSize(const Project& project, int& width, int& height);

    // the layout of the camera's Pixel Perfect Camera at the Game view's size and Pixel Scale;
    // false without an applicable component (none, disabled, a perspective camera)
    bool computeEditLayout(const Project& project, Scene* scene, Entity camera, jasperengine::pixelart::PixelPerfectLayout& layout);

    // outside Play, writes that layout into the component's runtime values (what get_entity and
    // the Inspector read); in Play the renderer's own are kept
    void refreshEditLayout(const Project& project, Scene* scene, Entity camera, bool playing);

    // "4x - 320x180 in 1280x720, bars 0x0"
    std::string describeLayout(const jasperengine::pixelart::PixelPerfectLayout& layout, int screenWidth, int screenHeight);

    // what is wrong with the component on that camera (the Inspector's warnings, in its order)
    std::vector<std::string> pixelPerfectWarnings(Scene* scene, Entity camera);

    // ---- the Scene view's Pixel Zoom (R21)

    // the scene's main camera has an applicable Pixel Perfect Camera: Pixel Zoom, the grid, Snap
    // Preview and Pixel Move Snap apply
    bool scenePixelPerfect(Scene* scene);
    // view pixels per base pixel of a 2D zoom (world units per view pixel), and back
    float pixelZoomScale(float zoom, int pixelsPerUnit);
    float zoomForPixelScale(float scale, int pixelsPerUnit);
    // the step of 1/8, 1/4, 1/2, 1, 2, 3 ... 32 after scale (direction > 0: in, < 0: out); a scale
    // between steps goes to the next one that way
    float stepPixelZoom(float scale, int direction);
    // the nearest step (the readout, the list)
    float nearestPixelZoom(float scale);
    // the steps, in order
    const std::vector<float>& pixelZoomSteps();
    // "3x", "1/2x"
    std::string pixelZoomLabel(float scale);
    // a camera position on whole view pixels of that zoom (panning without shimmer)
    float snapToViewPixels(float position, float zoom);

}
