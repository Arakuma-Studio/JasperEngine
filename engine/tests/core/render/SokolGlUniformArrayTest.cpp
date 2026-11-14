// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// On GL the engine declares a uniform block as one flattened array (SokolShader.cpp) and sokol
// uploads it with glUniform*v(location, declared count). A driver may size the array to the
// elements the linked program reads (the built-in Lit's u_fs_texCoordSets without a normal map
// reads only its first ivec4) or report it as no array, and then clamps or rejects the upload,
// the whole block with it (Doriax ebd4b64e7). sokol uploads the count the driver reports active
// (engine/libs/sokol/PATCHES.md, "Flattened uniform arrays"), which this checks against the
// driver the tests run on (the count, not an upload: a driver that clamps silently raises no GL
// error); MeshInputsGpu.TwoUvSetsWithoutANormalMapSampleTheirSets draws the case.

#include "GpuEnvironment.h"

#include "sokol_gfx.h"

#include <gtest/gtest.h>

// (not on Windows, whose GL headers declare GL 1.1 alone: sokol loads the rest itself)
#if defined(SOKOL_GLCORE) && !defined(_WIN32)
    #define GL_GLEXT_PROTOTYPES
    #include <GL/gl.h>
    #include <GL/glext.h>
#endif

// GL program state: a Vulkan or Metal build of the tests has no such test (a skip fails the unit
// gate, which allows none)
#if defined(SOKOL_GLCORE) && !defined(_WIN32)
TEST(SokolGlUniformArray, AFlattenedArrayWithAnUnusedTailUploadsWhatTheDriverKeeps){
    JASPER_REQUIRE_GPU();
    sg_shader_desc desc = {};
    desc.vertex_func.source =
        "#version 410\n"
        "void main(){ gl_Position = vec4(float(gl_VertexID & 1), float(gl_VertexID >> 1), 0.0, 1.0); }\n";
    // three ivec4, of which the program reads only the first
    desc.fragment_func.source =
        "#version 410\n"
        "uniform ivec4 u_rc_block[3];\n"
        "out vec4 frag_color;\n"
        "void main(){ frag_color = vec4(u_rc_block[0]) / 255.0; }\n";
    desc.uniform_blocks[0].stage = SG_SHADERSTAGE_FRAGMENT;
    desc.uniform_blocks[0].size = 48;
    desc.uniform_blocks[0].layout = SG_UNIFORMLAYOUT_STD140;
    desc.uniform_blocks[0].glsl_uniforms[0].glsl_name = "u_rc_block";
    desc.uniform_blocks[0].glsl_uniforms[0].type = SG_UNIFORMTYPE_INT4;
    desc.uniform_blocks[0].glsl_uniforms[0].array_count = 3;
    desc.label = "rc-uniform-array";
    const sg_shader shader = sg_make_shader(&desc);
    ASSERT_EQ(sg_query_shader_state(shader), SG_RESOURCESTATE_VALID);

    const GLuint program = sg_gl_query_shader_info(shader).prog;
    const GLchar* name = "u_rc_block";
    GLuint index = GL_INVALID_INDEX;
    glGetUniformIndices(program, 1, &name, &index);
    ASSERT_NE(index, GL_INVALID_INDEX);
    GLint active = 0;
    glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_SIZE, &active);
    ASSERT_GT(active, 0);
    // what this driver keeps of the array (recorded for the spec's Deviations: llvmpipe trims it)
    RecordProperty("active_size", active);

    const int uploaded = sgj_query_gl_uniform_count(shader, 0, 0);
    EXPECT_EQ(uploaded, active < 3 ? active : 3) << "sokol uploads more elements than the program has";
    sg_destroy_shader(shader);
}
#endif
