# libopus 1.6.1 - https://opus-codec.org (BSD-3-Clause, COPYING)
#
# The Opus decoder of the HTML UI's <video> audio (engine/core/htmlui/media/OpusAudioDecoder,
# docs/html-ui/phase-6-video.md §3.2). This file replaces upstream's build: the floating-point
# decoder only (mono/stereo and multistream), no encoder, no DNN (deep PLC, DRED, OSCE), no
# intrinsics. The sources are the objects a decoder-only program links (README.jasper.md).

add_library(

    opus

    STATIC

    # celt
    celt/bands.c
    celt/celt.c
    celt/celt_decoder.c
    celt/celt_lpc.c
    celt/cwrs.c
    celt/entcode.c
    celt/entdec.c
    celt/entenc.c
    celt/kiss_fft.c
    celt/laplace.c
    celt/mathops.c
    celt/mdct.c
    celt/modes.c
    celt/pitch.c
    celt/quant_bands.c
    celt/rate.c
    celt/vq.c
    # silk
    silk/CNG.c
    silk/LPC_analysis_filter.c
    silk/LPC_fit.c
    silk/LPC_inv_pred_gain.c
    silk/NLSF2A.c
    silk/NLSF_decode.c
    silk/NLSF_stabilize.c
    silk/NLSF_unpack.c
    silk/PLC.c
    silk/bwexpander.c
    silk/bwexpander_32.c
    silk/code_signs.c
    silk/dec_API.c
    silk/decode_core.c
    silk/decode_frame.c
    silk/decode_indices.c
    silk/decode_parameters.c
    silk/decode_pitch.c
    silk/decode_pulses.c
    silk/decoder_set_fs.c
    silk/gain_quant.c
    silk/init_decoder.c
    silk/lin2log.c
    silk/log2lin.c
    silk/pitch_est_tables.c
    silk/resampler.c
    silk/resampler_private_AR2.c
    silk/resampler_private_IIR_FIR.c
    silk/resampler_private_down_FIR.c
    silk/resampler_private_up2_HQ.c
    silk/resampler_rom.c
    silk/shell_coder.c
    silk/sort.c
    silk/stereo_MS_to_LR.c
    silk/stereo_decode_pred.c
    silk/sum_sqr_shift.c
    silk/table_LSF_cos.c
    silk/tables_LTP.c
    silk/tables_NLSF_CB_NB_MB.c
    silk/tables_NLSF_CB_WB.c
    silk/tables_gain.c
    silk/tables_other.c
    silk/tables_pitch_lag.c
    silk/tables_pulses_per_block.c
    # src
    src/extensions.c
    src/opus.c
    src/opus_decoder.c
    src/opus_multistream.c
    src/opus_multistream_decoder.c
)

target_include_directories(opus SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(opus PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/celt" "${CMAKE_CURRENT_SOURCE_DIR}/silk")
# USE_ALLOCA: MSVC has no variable-length arrays; OPUS_EXPORT empty keeps opus_* out of the
# editor's shared engine
target_compile_definitions(opus PRIVATE OPUS_BUILD USE_ALLOCA HAVE_LRINT HAVE_LRINTF PACKAGE_VERSION="1.6.1")
target_compile_definitions(opus PUBLIC OPUS_EXPORT=)

if(MSVC)
    target_compile_definitions(opus PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(opus PRIVATE -fvisibility=hidden)
    if(NOT APPLE AND NOT EMSCRIPTEN)
        target_link_libraries(opus PRIVATE m)
    endif()
endif()
if(EMSCRIPTEN AND EMSCRIPTEN_THREAD_SUPPORT)
    target_compile_options(opus PRIVATE -pthread)
endif()

set_target_properties(opus PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
