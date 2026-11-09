// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S49 AC1 (R2, D4): a pixel particle's box sits on the grid by its lower-left corner, snapped with
// S41's rule, so a box of n pixels covers exactly n x n whole pixels wherever its centre falls;
// for an even n that is S41's centre snap. Also AC5 (R11): Streak and Shard draw pixel-perfect
// lines (one pixel per step of the major axis, no L-corner) at every angle.

#include "particles/ParticlePixel.h"
#include "pixelart/PixelSnap.h"

#include <gtest/gtest.h>

#include <cmath>
#include <random>
#include <set>
#include <utility>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::particles;

namespace {

    PixelShapeSpec dotSpec(){
        PixelShapeSpec spec;
        spec.shape = (int)PixelShape::Dot;
        return spec;
    }

    // the pixels a rasterised box covers, by the pixel centre each fragment of its quad samples
    std::set<std::pair<int, int>> coveredPixels(const PixelLayout& l){
        std::set<std::pair<int, int>> out;
        const int x0 = (int)std::floor(l.cornerX) - 1, y0 = (int)std::floor(l.cornerY) - 1;
        for (int y = y0; y < y0 + (int)l.boxH + 3; y++){
            for (int x = x0; x < x0 + (int)l.boxW + 3; x++){
                const float cx = (float)x + 0.5f, cy = (float)y + 0.5f;
                if (cx >= l.cornerX && cx < l.cornerX + l.boxW && cy >= l.cornerY && cy < l.cornerY + l.boxH)
                    out.insert({x, y});
            }
        }
        return out;
    }

}

TEST(PixelSnapParticle, CornerSnapCoversWholePixels){
    std::mt19937 rng(49);
    std::uniform_real_distribution<float> where(-200.0f, 200.0f);
    for (int n = 1; n <= 9; n++){
        for (int i = 0; i < 1000; i++){
            const Vector2 centre(where(rng), where(rng));
            const PixelLayout l = ParticlePixel::layout(dotSpec(), centre, Vector2((float)n, (float)n), Vector2(0.0f, 0.0f), 0.0f, true);
            // the corner is a whole pixel and the box n x n of them
            ASSERT_EQ(l.cornerX, std::round(l.cornerX)) << n << " at " << centre.x;
            ASSERT_EQ(l.cornerY, std::round(l.cornerY)) << n << " at " << centre.y;
            const std::set<std::pair<int, int>> covered = coveredPixels(l);
            ASSERT_EQ(covered.size(), (size_t)(n * n)) << "a box of " << n << " covers " << covered.size() << " pixels";
            // S41's rule, bit for bit: snap(centre - n / 2)
            EXPECT_EQ(l.cornerX, pixelart::snap(centre.x - (float)n * 0.5f));
            EXPECT_EQ(l.cornerY, pixelart::snap(centre.y - (float)n * 0.5f));
            if (n % 2 == 0){
                // even: the same box as S41's centre snap
                EXPECT_EQ(l.cornerX + (float)n * 0.5f, pixelart::snap(centre.x));
                EXPECT_EQ(l.cornerY + (float)n * 0.5f, pixelart::snap(centre.y));
            }
        }
    }
}

TEST(PixelSnapParticle, UnsnappedBoxMovesBySubPixels){
    // Snap off: the box keeps the sub-pixel centre (the fragments still test whole pixels)
    const PixelLayout l = ParticlePixel::layout(dotSpec(), Vector2(10.3f, 4.6f), Vector2(3.0f, 3.0f), Vector2(0.0f, 0.0f), 0.0f, false);
    EXPECT_FLOAT_EQ(l.cornerX, 8.8f);
    EXPECT_FLOAT_EQ(l.cornerY, 3.1f);
}

TEST(PixelLine, StreakAndShardArePixelPerfectAtEveryAngle){
    for (int shape : {(int)PixelShape::Streak, (int)PixelShape::Shard}){
        PixelShapeSpec spec;
        spec.shape = shape;
        spec.streakLengthScale = 1.0f;
        spec.streakMaxLength = 64.0f;
        spec.rotationSteps = 360;
        for (int length = 1; length <= 16; length++){
            for (int degrees = 0; degrees < 360; degrees++){
                const float a = (float)degrees * 3.14159265358979f / 180.0f;
                Vector2 velocity(0.0f, 0.0f);
                float n = (float)length, rotation = 0.0f;
                if (shape == (int)PixelShape::Streak){
                    // L = 1 + speed * lengthScale = length
                    const float speed = (float)(length - 1);
                    velocity = Vector2(std::cos(a) * speed, std::sin(a) * speed);
                    n = 1.0f;
                }else{
                    rotation = -a;   // clockwise as seen: the shape's angle is counter-clockwise
                }
                const PixelLayout l = ParticlePixel::layout(spec, Vector2(20.37f, 20.61f), Vector2(n, n), velocity, rotation, true);
                std::vector<PixelCell> cells;
                ParticlePixel::rasterize(spec, l, 0.0f, 0, cells);
                ASSERT_FALSE(cells.empty());
                std::set<std::pair<int, int>> pixels;
                for (const PixelCell& c : cells) pixels.insert({c.x, c.y});
                const int dx = l.tailX - l.headX, dy = l.tailY - l.headY;
                const bool xMajor = std::abs(dx) >= std::abs(dy);
                // one pixel per step of the major axis
                std::set<int> majors;
                for (const auto& p : pixels) majors.insert(xMajor ? p.first : p.second);
                ASSERT_EQ(majors.size(), pixels.size()) << "shape " << shape << " length " << length << " at " << degrees << " degrees";
                ASSERT_EQ((int)pixels.size(), std::max(std::abs(dx), std::abs(dy)) + 1);
                // no L-corner: no pixel with both a horizontal and a vertical neighbour on the line
                for (const auto& p : pixels){
                    const bool horizontal = pixels.count({p.first + 1, p.second}) || pixels.count({p.first - 1, p.second});
                    const bool vertical = pixels.count({p.first, p.second + 1}) || pixels.count({p.first, p.second - 1});
                    ASSERT_FALSE(horizontal && vertical) << "an L-corner: shape " << shape << " length " << length << " at " << degrees;
                }
            }
        }
    }
}
