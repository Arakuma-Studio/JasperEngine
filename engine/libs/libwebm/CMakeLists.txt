# libwebm 1.0.0.32 - https://chromium.googlesource.com/webm/libwebm (BSD-3-Clause, LICENSE.TXT;
# patent grant, PATENTS.TXT)
#
# The WebM/Matroska parser of the HTML UI's <video> (engine/core/media/VideoDemuxer,
# docs/html-ui/phase-6-video.md §3.2): mkvparser only (no muxer, no WebVTT, no tools).

add_library(webm STATIC mkvparser/mkvparser.cc)

target_include_directories(webm SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

if(MSVC)
    target_compile_definitions(webm PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    # keep mkvparser:: inside libjasperengine when the editor builds it shared
    target_compile_options(webm PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
endif()
if(EMSCRIPTEN AND EMSCRIPTEN_THREAD_SUPPORT)
    target_compile_options(webm PRIVATE -pthread)
endif()

set_target_properties(webm PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
