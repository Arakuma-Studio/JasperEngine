// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// FFmpeg 6.x (libavcodec 60): FFmpegBackendImpl.h compiled against engine/libs/ffmpeg-headers/60
// (the include path of this file only, engine/CMakeLists.txt)

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

extern "C" {
#include "libavformat/avformat.h"
#include "libavformat/avio.h"
#include "libavcodec/avcodec.h"
#include "libavutil/avutil.h"
#include "libavutil/channel_layout.h"
#include "libavutil/pixdesc.h"
#include "libswresample/swresample.h"
#include "libswscale/swscale.h"
}

static_assert(LIBAVCODEC_VERSION_MAJOR == 60, "FFmpegBackend60.cpp needs the headers of libavcodec 60");

#define FFMPEG_NAMESPACE ffmpeg60
#include "FFmpegBackendImpl.h"
