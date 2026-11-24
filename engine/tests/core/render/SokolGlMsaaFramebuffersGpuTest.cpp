// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// sokol's GL backend mutated one framebuffer object for every offscreen pass. The Android
// emulator's guest GLES encoder kept the 4-sample depth-stencil renderbuffer of an HTML UI pass in
// that framebuffer's depth slot after the next pass attached a 1-sample texture, so every later
// single-sampled pass was GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE and the scene went undrawn. sokol now
// keeps one framebuffer per sample count (engine/libs/sokol/PATCHES.md, "One mutated framebuffer per
// MSAA sample count on GL"). Desktop drivers track attachments correctly, so the pixels cannot show
// the bug here; this checks the invariant that keeps a stale slot from mixing counts, on the GL the
// tests run on. tools/ci/android_packaging.sh checks what the emulator shows.

#include "GpuEnvironment.h"

#include "render/SystemRender.h"
#include "sokol_gfx.h"

#include <gtest/gtest.h>

#include <map>
#include <set>

// (not on Windows, whose GL headers declare GL 1.1 alone: sokol loads the rest itself)
#if defined(SOKOL_GLCORE) && !defined(_WIN32)
    #define GL_GLEXT_PROTOTYPES
    #include <GL/gl.h>
    #include <GL/glext.h>
    #define JASPER_MSAA_FB_TEST_GL 1
#elif defined(SOKOL_GLES3)
    #include <GLES3/gl3.h>
    #define JASPER_MSAA_FB_TEST_GL 1
#endif

using namespace jasperengine;

#if defined(JASPER_MSAA_FB_TEST_GL)
namespace {

    // a colour and a depth-stencil target of one sample count, and their attachment views
    struct MsaaFbTarget {
        sg_image color = {};
        sg_image depth = {};
        sg_view colorView = {};
        sg_view depthView = {};

        bool make(int sampleCount){
            sg_image_desc image = {};
            image.width = 16;
            image.height = 16;
            image.sample_count = sampleCount;
            image.pixel_format = SG_PIXELFORMAT_RGBA8;
            image.usage.color_attachment = true;
            image.label = "msaa-fb-color";
            color = sg_make_image(&image);
            image.pixel_format = SG_PIXELFORMAT_DEPTH_STENCIL;
            image.usage.color_attachment = false;
            image.usage.depth_stencil_attachment = true;
            image.label = "msaa-fb-depth";
            depth = sg_make_image(&image);
            sg_view_desc view = {};
            view.color_attachment.image = color;
            colorView = sg_make_view(&view);
            view = {};
            view.depth_stencil_attachment.image = depth;
            depthView = sg_make_view(&view);
            return sg_query_view_state(colorView) == SG_RESOURCESTATE_VALID &&
                   sg_query_view_state(depthView) == SG_RESOURCESTATE_VALID;
        }

        void destroy(){
            sg_destroy_view(depthView);
            sg_destroy_view(colorView);
            sg_destroy_image(depth);
            sg_destroy_image(color);
        }
    };

    struct MsaaFbBinding {
        GLint framebuffer = 0;
        GLint samples = -1;
        GLenum status = 0;
    };

    // the framebuffer sokol binds for a cleared pass into `target`, and what GL says of it
    MsaaFbBinding msaaFbPass(const MsaaFbTarget& target){
        sg_pass pass = {};
        pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
        pass.action.colors[0].store_action = SG_STOREACTION_DONTCARE;
        pass.action.depth.load_action = SG_LOADACTION_CLEAR;
        pass.action.depth.store_action = SG_STOREACTION_DONTCARE;
        pass.action.stencil.load_action = SG_LOADACTION_CLEAR;
        pass.action.stencil.store_action = SG_STOREACTION_DONTCARE;
        pass.attachments.colors[0] = target.colorView;
        pass.attachments.depth_stencil = target.depthView;
        sg_begin_pass(&pass);
        MsaaFbBinding binding;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &binding.framebuffer);
        glGetIntegerv(GL_SAMPLES, &binding.samples);
        binding.status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        sg_end_pass();
        return binding;
    }

}

// GL framebuffer state: a Vulkan or Metal build of the tests has no such test (a skip fails the unit
// gate, which allows none)
TEST(SokolGlMsaaFramebuffersGpu, EachSampleCountKeepsAFramebufferOfItsOwn){
    JASPER_REQUIRE_GPU();
    MsaaFbTarget single, multi;
    ASSERT_TRUE(single.make(1));
    ASSERT_TRUE(multi.make(4)) << "4 samples are what an HTML UI target takes by default";
    SystemRender::commit();

    // the camera's single-sampled pass, the UI's 4-sample one, and both again as the next frame
    const MsaaFbTarget* order[] = {&single, &multi, &single, &multi};
    std::map<GLint, std::set<GLint>> samplesByFramebuffer;
    GLint firstSingle = 0, firstMulti = 0;
    for (const MsaaFbTarget* target : order){
        const MsaaFbBinding binding = msaaFbPass(*target);
        const bool isMulti = (target == &multi);
        SCOPED_TRACE(isMulti ? "4 samples" : "1 sample");
        EXPECT_EQ(binding.status, (GLenum)GL_FRAMEBUFFER_COMPLETE);
        EXPECT_NE(binding.framebuffer, 0) << "an offscreen pass binds a framebuffer of sokol's";
        EXPECT_EQ(binding.samples, isMulti ? 4 : 0);
        samplesByFramebuffer[binding.framebuffer].insert(binding.samples);
        GLint& first = isMulti ? firstMulti : firstSingle;
        if (first == 0){
            first = binding.framebuffer;
        }else{
            EXPECT_EQ(binding.framebuffer, first) << "a sample count's framebuffer is made once, not per pass";
        }
    }
    SystemRender::commit();

    for (const auto& [framebuffer, samples] : samplesByFramebuffer){
        EXPECT_EQ(samples.size(), 1u) << "framebuffer " << framebuffer
                                      << " held attachments of more than one sample count";
    }
    EXPECT_NE(firstSingle, firstMulti);

    multi.destroy();
    single.destroy();
}
#endif
