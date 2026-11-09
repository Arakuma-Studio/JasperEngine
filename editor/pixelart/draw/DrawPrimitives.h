// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The integer geometry of edit_sprite's paint ops (docs/specs/S47-agent-sprite-drawing.md R16,
// R18, R19): Bresenham lines with Pixelorama's pixel-perfect filter, Zingl's rectangle-bounded
// ellipse, Pixelorama's rounded rectangles, even-odd polygons, the scanline flood fill, the
// Bayer matrices and the outline neighbourhoods. Shapes come out as pixel lists or masks; the
// ops decide colours. Pure, any thread, deterministic.
//
// Ported from Pixelorama (MIT, (c) 2019-present Orama Interactive and contributors):
// src/Classes/Drawers.gd (PixelPerfectDrawer), src/Autoload/DrawingAlgos.gd
// (get_ellipse_points, get_rounded_rect_points[_filled]), src/Classes/FloodFillObject.gd (the
// scanline fill of Allegro 4.2.1's flood.c by Shawn Hargreaves) and
// src/Shaders/Effects/OutlineInline.gdshaderinc (the neighbourhoods).

#pragma once

#include "pixelart/PixelImage.h"

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace jasperengine::editor::pixelart::draw {

    // Bresenham from a to b, both ends included, in drawing order
    void bresenham(IntPoint a, IntPoint b, std::vector<IntPoint>& out);
    // A polyline's pixels in stroke order, each joint once. pixelPerfect removes every L-corner
    // (Drawers.gd PixelPerfectDrawer: when the newest pixel is diagonal to the one two back and
    // orthogonal to the one before, that one goes)
    std::vector<IntPoint> polyline(std::span<const IntPoint> points, bool pixelPerfect);
    // The pixel-perfect filter alone, on pixels in stroke order
    std::vector<IntPoint> pixelPerfectFilter(std::span<const IntPoint> stroke);

    // Zingl's ellipse inside a rect (DrawingAlgos.gd get_ellipse_points): its border pixels
    std::vector<IntPoint> ellipseBorder(const IntRect& rect);
    // The filled ellipse as spans between its border pixels, one row at a time
    std::vector<IntPoint> ellipseFilled(const IntRect& rect);
    // A ring `thickness` wide: the filled ellipse less the one inset by thickness (1: the border)
    std::vector<IntPoint> ellipseRing(const IntRect& rect, int thickness);

    // DrawingAlgos.gd get_rounded_rect_points_filled; radius 0 is the plain rect
    std::vector<IntPoint> roundedRectFilled(const IntRect& rect, int radius);
    // The outline `thickness` wide (the filled shape less the inner one, radius - thickness)
    std::vector<IntPoint> roundedRectOutline(const IntRect& rect, int radius, int thickness);

    // Polygon edges by the line rule (closed), and the even-odd interior at pixel centres
    std::vector<IntPoint> polygonEdges(std::span<const IntPoint> points);
    std::vector<IntPoint> polygonFilled(std::span<const IntPoint> points, const IntRect& clip);

    // A square brush `thickness` wide stamped on each pixel (thickness 1: the pixels)
    std::vector<IntPoint> stamp(std::span<const IntPoint> pixels, int thickness);

    // Scanline flood fill over a w x h grid from `seed`: `similar(x, y)` says whether a pixel
    // belongs to the area. 8-connectivity also joins diagonal neighbours. Returns the area as a
    // w*h mask (1 = in the area).
    std::vector<uint8_t> floodFill(int w, int h, IntPoint seed, bool eightConnected, const std::function<bool(int, int)>& similar);

    // The Bayer matrix of side n (2, 4, 8, 16): M2n = [[4M, 4M+2], [4M+3, 4M+1]], values 0..n*n-1,
    // row-major as m[x * n + y] (M[x][y], the spec's indexing)
    std::vector<int> bayerMatrix(int n);

    enum class NeighbourShape : uint8_t { Diamond, Circle, Square };
    // The offsets within `width` of a pixel (the pixel itself left out): diamond w - |i|, circle
    // floor(sqrt((w + 0.5)^2 - i^2)), square w
    std::vector<IntPoint> neighbourhood(int width, NeighbourShape shape);

    // Removes duplicates, keeping the first occurrence's order
    void dedupe(std::vector<IntPoint>& points);

}
