// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Util's file kinds: what hot reload, auto-sync and the asset tools take for an image.

#include <gtest/gtest.h>

#include "util/Util.h"

namespace UtilTest_cpp {

using namespace jasperengine::editor;

// S94 F50: .exr, .tif, .tiff and .psb are images, next to the formats stb_image reads
TEST(UtilTest, ImageExtensions) {
    for (const char* path : {"a.png", "a.jpg", "a.jpeg", "a.bmp", "a.tga", "a.gif", "a.hdr", "a.psd", "a.pic", "a.pnm", "a.svg",
                             "a.exr", "a.tif", "a.tiff", "a.psb", "dir/B.TIF", "C.PSB", "env.EXR"})
        EXPECT_TRUE(Util::isImageFile(path)) << path;
    for (const char* path : {"a.ktx2", "a.txt", "a.tif.meta", "tif", "a.psbx", ""})
        EXPECT_FALSE(Util::isImageFile(path)) << path;
}

}  // namespace UtilTest_cpp
