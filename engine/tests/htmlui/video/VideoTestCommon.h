// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Shared helpers of the <video> tests (docs/html-ui/phase-6-video.md §5.1): the committed clips of
// tools/htmlui/gen_video_fixtures.py and the frame-index pattern every test-card frame carries.

#ifndef VIDEOTESTCOMMON_H
#define VIDEOTESTCOMMON_H

#include "media/VideoTypes.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::test {

    inline std::string videoFixture(const std::string& name){
        return std::string(JASPER_HTMLUI_TEST_SOURCE_DIR) + "/fixtures/video/" + name;
    }

    // the test card's layout (gen_video_fixtures.py)
    constexpr int CARD_WIDTH = 320;
    constexpr int CARD_HEIGHT = 180;
    constexpr int CARD_FRAMES = 60;
    constexpr int CARD_FPS = 30;
    constexpr int CARD_BIT_Y = 124;
    constexpr int CARD_BIT_BLOCK = 32;
    constexpr int CARD_BIT_X0 = 16;
    constexpr int CARD_BIT_STEP = 36;

    // the frame index the 8 blocks of a decoded test-card frame show (luma: black 16, white 235)
    inline int cardIndexOfLuma(const std::vector<uint8_t>& y, int width){
        const int row = CARD_BIT_Y + CARD_BIT_BLOCK / 2;
        int value = 0;
        for (int bit = 0; bit < 8; bit++){
            const int x = CARD_BIT_X0 + CARD_BIT_STEP * bit + CARD_BIT_BLOCK / 2;
            value = (value << 1) | (y[(size_t)row * (size_t)width + (size_t)x] > 125 ? 1 : 0);
        }
        return value;
    }

    inline int cardIndexOf(const VideoFrame& frame){
        return cardIndexOfLuma(frame.y, frame.width);
    }

    // the same pattern in an RGBA8 image (rows top first, `stride` bytes per row; the test card
    // at `scale` x its size from (x0, y0))
    inline int cardIndexOfRgba(const uint8_t* rgba, int stride, int x0, int y0, double scale = 1.0){
        const int row = y0 + (int)std::lround((CARD_BIT_Y + CARD_BIT_BLOCK / 2) * scale);
        int value = 0;
        for (int bit = 0; bit < 8; bit++){
            const int x = x0 + (int)std::lround((CARD_BIT_X0 + CARD_BIT_STEP * bit + CARD_BIT_BLOCK / 2) * scale);
            const uint8_t green = rgba[(size_t)row * (size_t)stride + (size_t)x * 4 + 1];
            value = (value << 1) | (green > 128 ? 1 : 0);
        }
        return value;
    }

    // the pts the WebM muxer gave frame k (millisecond timecodes)
    inline int64_t cardPtsNs(int frame){
        return (int64_t)std::llround(frame * 1000.0 / CARD_FPS) * 1000000LL;
    }

    inline double cardTime(int frame){
        return (double)cardPtsNs(frame) / 1e9;
    }

}

#endif //VIDEOTESTCOMMON_H
