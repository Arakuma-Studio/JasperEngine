// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// BF3: every GL texture sokol makes states its levels as its GL_TEXTURE_MAX_LEVEL. sokol's samplers
// always use a mipmapped min filter; immutable storage should clamp the levels GL looks for to the
// ones it has, but the Android emulator's GLES translator on legacy SwiftShader (-gpu
// swiftshader_indirect) ignores that and samples a one-level texture (a font atlas, an HTML UI
// layer, a render target) as incomplete: zeros. The fix (engine/libs/sokol/sokol_gfx.h,
// _sg_gl_texstorage) sets the max level, which this checks on the desktop GL the tests run on;
// tools/android-emulator-test.sh's render gate checks what the emulator shows (docs/testing.md).

#include "GpuEnvironment.h"

#include "sokol_gfx.h"

#include <gtest/gtest.h>

#include <vector>

// (not on Windows, whose GL headers declare GL 1.1 alone: sokol loads the rest itself)
#if defined(SOKOL_GLCORE) && !defined(_WIN32)
    #define GL_GLEXT_PROTOTYPES
    #include <GL/gl.h>
#endif

namespace {

#if defined(SOKOL_GLCORE) && !defined(_WIN32)
    // the GL_TEXTURE_MAX_LEVEL of an image's GL texture
    GLint maxLevelOf(sg_image image){
        const sg_gl_image_info info = sg_gl_query_image_info(image);
        GLint previous = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
        glBindTexture(GL_TEXTURE_2D, info.tex[info.active_slot]);
        GLint maxLevel = -1;
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &maxLevel);
        glBindTexture(GL_TEXTURE_2D, (GLuint)previous);
        // sokol caches the texture bindings
        sg_reset_state_cache();
        return maxLevel;
    }
#endif

}

// GL texture state: a Vulkan or Metal build of the tests has no such test (a skip fails the unit gate,
// which allows none)
#if defined(SOKOL_GLCORE) && !defined(_WIN32)
TEST(SokolGlTextureLevels, OneLevelTexturesStateTheirMaxLevel){
    JASPER_REQUIRE_GPU();
    // a font atlas: R8, one level, with data
    std::vector<uint8_t> texels(64 * 32, 128);
    sg_image_desc atlas = {};
    atlas.width = 64;
    atlas.height = 32;
    atlas.pixel_format = SG_PIXELFORMAT_R8;
    atlas.data.mip_levels[0].ptr = texels.data();
    atlas.data.mip_levels[0].size = texels.size();
    atlas.label = "bf3-one-level-atlas";
    const sg_image atlasImage = sg_make_image(&atlas);
    ASSERT_EQ(sg_query_image_state(atlasImage), SG_RESOURCESTATE_VALID);
    EXPECT_EQ(maxLevelOf(atlasImage), 0);

    // a render target (an HTML UI layer, a post-processing input): one level, no data
    sg_image_desc target = {};
    target.width = 32;
    target.height = 32;
    target.pixel_format = SG_PIXELFORMAT_RGBA8;
    target.usage.color_attachment = true;
    target.label = "bf3-one-level-target";
    const sg_image targetImage = sg_make_image(&target);
    ASSERT_EQ(sg_query_image_state(targetImage), SG_RESOURCESTATE_VALID);
    EXPECT_EQ(maxLevelOf(targetImage), 0);

    // a mip chain keeps every level: 16, 8, 4, 2, 1
    std::vector<uint8_t> levels[5];
    sg_image_desc chain = {};
    chain.width = 16;
    chain.height = 16;
    chain.num_mipmaps = 5;
    chain.pixel_format = SG_PIXELFORMAT_RGBA8;
    for (int level = 0; level < 5; level++){
        const int size = 16 >> level;
        levels[level].assign((size_t)size * size * 4, 255);
        chain.data.mip_levels[level].ptr = levels[level].data();
        chain.data.mip_levels[level].size = levels[level].size();
    }
    chain.label = "bf3-mip-chain";
    const sg_image chainImage = sg_make_image(&chain);
    ASSERT_EQ(sg_query_image_state(chainImage), SG_RESOURCESTATE_VALID);
    EXPECT_EQ(maxLevelOf(chainImage), 4);

    sg_destroy_image(atlasImage);
    sg_destroy_image(targetImage);
    sg_destroy_image(chainImage);
}
#endif
