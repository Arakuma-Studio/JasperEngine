// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SokolTraceHub.h"

#include "SokolPassRecorder.h"
#if JASPERENGINE_PROFILER
#include "SokolVramTracker.h"
#endif
#include "sokol_gfx.h"

using namespace jasperengine;

namespace {

    bool installed = false;
    sg_trace_hooks previous = {};

    // ---- the pass recorder

    void beginPass(const sg_pass* pass, void* userData){
        SokolPassRecorder::onBeginPass(pass);
        if (previous.begin_pass) previous.begin_pass(pass, previous.user_data);
    }

    void endPass(void* userData){
        SokolPassRecorder::onEndPass();
        if (previous.end_pass) previous.end_pass(previous.user_data);
    }

    void commit(void* userData){
        SokolPassRecorder::onCommit();
        if (previous.commit) previous.commit(previous.user_data);
    }

#if JASPERENGINE_PROFILER
    // the rest of the pass recorder: counts, draws, pipeline and shader labels

    void applyPipeline(sg_pipeline pipeline, void* userData){
        SokolPassRecorder::onApplyPipeline(pipeline.id);
        if (previous.apply_pipeline) previous.apply_pipeline(pipeline, previous.user_data);
    }

    void applyBindings(const sg_bindings* bindings, void* userData){
        SokolPassRecorder::onApplyBindings();
        if (previous.apply_bindings) previous.apply_bindings(bindings, previous.user_data);
    }

    void applyUniforms(int slot, const sg_range* data, void* userData){
        SokolPassRecorder::onApplyUniforms(data ? data->size : 0);
        if (previous.apply_uniforms) previous.apply_uniforms(slot, data, previous.user_data);
    }

    void draw(int baseElement, int numElements, int numInstances, void* userData){
        SokolPassRecorder::onDraw(baseElement, numElements, numInstances);
        if (previous.draw) previous.draw(baseElement, numElements, numInstances, previous.user_data);
    }

    void drawEx(int baseElement, int numElements, int numInstances, int baseVertex, int baseInstance, void* userData){
        SokolPassRecorder::onDraw(baseElement, numElements, numInstances);
        if (previous.draw_ex) previous.draw_ex(baseElement, numElements, numInstances, baseVertex, baseInstance, previous.user_data);
    }

    void makePipeline(const sg_pipeline_desc* desc, sg_pipeline result, void* userData){
        SokolPassRecorder::onMakePipeline(result.id, desc);
        if (previous.make_pipeline) previous.make_pipeline(desc, result, previous.user_data);
    }

    void initPipeline(sg_pipeline pipeline, const sg_pipeline_desc* desc, void* userData){
        SokolPassRecorder::onMakePipeline(pipeline.id, desc);
        if (previous.init_pipeline) previous.init_pipeline(pipeline, desc, previous.user_data);
    }

    void destroyPipeline(sg_pipeline pipeline, void* userData){
        SokolPassRecorder::onDestroyPipeline(pipeline.id);
        if (previous.destroy_pipeline) previous.destroy_pipeline(pipeline, previous.user_data);
    }

    void uninitPipeline(sg_pipeline pipeline, void* userData){
        SokolPassRecorder::onDestroyPipeline(pipeline.id);
        if (previous.uninit_pipeline) previous.uninit_pipeline(pipeline, previous.user_data);
    }

    void makeShader(const sg_shader_desc* desc, sg_shader result, void* userData){
        SokolPassRecorder::onMakeShader(result.id, desc);
        if (previous.make_shader) previous.make_shader(desc, result, previous.user_data);
    }

    void initShader(sg_shader shader, const sg_shader_desc* desc, void* userData){
        SokolPassRecorder::onMakeShader(shader.id, desc);
        if (previous.init_shader) previous.init_shader(shader, desc, previous.user_data);
    }

    void destroyShader(sg_shader shader, void* userData){
        SokolPassRecorder::onDestroyShader(shader.id);
        if (previous.destroy_shader) previous.destroy_shader(shader, previous.user_data);
    }

    void uninitShader(sg_shader shader, void* userData){
        SokolPassRecorder::onDestroyShader(shader.id);
        if (previous.uninit_shader) previous.uninit_shader(shader, previous.user_data);
    }

    // ---- the VRAM tracker: make_* and init_* create (deduplicated by id), destroy_* and
    // uninit_* free

    void makeImage(const sg_image_desc* desc, sg_image result, void* userData){
        SokolVramTracker::onImageCreated(result.id, desc);
        if (previous.make_image) previous.make_image(desc, result, previous.user_data);
    }

    void initImage(sg_image image, const sg_image_desc* desc, void* userData){
        SokolVramTracker::onImageCreated(image.id, desc);
        if (previous.init_image) previous.init_image(image, desc, previous.user_data);
    }

    void destroyImage(sg_image image, void* userData){
        SokolVramTracker::onImageDestroyed(image.id);
        if (previous.destroy_image) previous.destroy_image(image, previous.user_data);
    }

    void uninitImage(sg_image image, void* userData){
        SokolVramTracker::onImageDestroyed(image.id);
        if (previous.uninit_image) previous.uninit_image(image, previous.user_data);
    }

    void makeBuffer(const sg_buffer_desc* desc, sg_buffer result, void* userData){
        SokolVramTracker::onBufferCreated(result.id, desc);
        if (previous.make_buffer) previous.make_buffer(desc, result, previous.user_data);
    }

    void initBuffer(sg_buffer buffer, const sg_buffer_desc* desc, void* userData){
        SokolVramTracker::onBufferCreated(buffer.id, desc);
        if (previous.init_buffer) previous.init_buffer(buffer, desc, previous.user_data);
    }

    void destroyBuffer(sg_buffer buffer, void* userData){
        SokolVramTracker::onBufferDestroyed(buffer.id);
        if (previous.destroy_buffer) previous.destroy_buffer(buffer, previous.user_data);
    }

    void uninitBuffer(sg_buffer buffer, void* userData){
        SokolVramTracker::onBufferDestroyed(buffer.id);
        if (previous.uninit_buffer) previous.uninit_buffer(buffer, previous.user_data);
    }
#endif

}

void SokolTraceHub::install(bool frameStats){
    if (installed)
        return;
#if !JASPERENGINE_PROFILER
    // a release game: only the frame stats' pass list, only when asked for
    if (!frameStats)
        return;
#endif
    sg_trace_hooks hooks = {};
    hooks.begin_pass = beginPass;
    hooks.end_pass = endPass;
    hooks.commit = commit;
#if JASPERENGINE_PROFILER
    hooks.apply_pipeline = applyPipeline;
    hooks.apply_bindings = applyBindings;
    hooks.apply_uniforms = applyUniforms;
    hooks.draw = draw;
    hooks.draw_ex = drawEx;
    hooks.make_pipeline = makePipeline;
    hooks.init_pipeline = initPipeline;
    hooks.destroy_pipeline = destroyPipeline;
    hooks.uninit_pipeline = uninitPipeline;
    hooks.make_shader = makeShader;
    hooks.init_shader = initShader;
    hooks.destroy_shader = destroyShader;
    hooks.uninit_shader = uninitShader;
    hooks.make_image = makeImage;
    hooks.init_image = initImage;
    hooks.destroy_image = destroyImage;
    hooks.uninit_image = uninitImage;
    hooks.make_buffer = makeBuffer;
    hooks.init_buffer = initBuffer;
    hooks.destroy_buffer = destroyBuffer;
    hooks.uninit_buffer = uninitBuffer;
    SokolPassRecorder::setEnabled(true);
#endif
    previous = sg_install_trace_hooks(&hooks);
    installed = true;
}

void SokolTraceHub::uninstall(){
    if (!installed)
        return;
    sg_install_trace_hooks(&previous);
    previous = {};
    installed = false;
    SokolPassRecorder::setEnabled(false);
    SokolPassRecorder::clear();
}

bool SokolTraceHub::isInstalled(){
    return installed;
}
