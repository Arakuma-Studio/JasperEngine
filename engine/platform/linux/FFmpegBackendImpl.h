// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The FFmpeg decoder backend, compiled once per FFmpeg ABI major: FFmpegBackend<NN>.cpp defines
// FFMPEG_NAMESPACE (ffmpegNN), includes that major's vendored headers
// (engine/libs/ffmpeg-headers/<NN>, its include path) and then this file. Every FFmpeg function
// is called through a pointer resolved with dlsym from the libraries FFmpegBackend.cpp loaded,
// typed by that major's own declarations, so each translation unit matches the ABI it calls.
// The old channel-layout API is deprecated in the headers of 5.x and 6.x and used there only when
// the run-time libraries predate the new one: those translation units silence the deprecations.

#ifndef FFMPEG_NAMESPACE
#error "define FFMPEG_NAMESPACE and include the FFmpeg headers of one major before FFmpegBackendImpl.h"
#endif

#include "FFmpegBackend.h"

#include <dlfcn.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// the channel layout API (AVChannelLayout, swr_alloc_set_opts2) came with FFmpeg 5.1 (avutil
// 57.24); 7.0 (avutil 59) removed the old one
#define JASPER_FF_NEW_LAYOUT (LIBAVUTIL_VERSION_MAJOR >= 57)
#define JASPER_FF_OLD_LAYOUT (LIBAVUTIL_VERSION_MAJOR < 59)

namespace jasperengine {
namespace FFMPEG_NAMESPACE {

    namespace {

        struct Api {
            // libavformat
            decltype(&::avformat_alloc_context) avformat_alloc_context = nullptr;
            decltype(&::avformat_open_input) avformat_open_input = nullptr;
            decltype(&::avformat_find_stream_info) avformat_find_stream_info = nullptr;
            decltype(&::av_find_best_stream) av_find_best_stream = nullptr;
            decltype(&::av_read_frame) av_read_frame = nullptr;
            decltype(&::av_seek_frame) av_seek_frame = nullptr;
            decltype(&::avformat_close_input) avformat_close_input = nullptr;
            decltype(&::avformat_free_context) avformat_free_context = nullptr;
            decltype(&::avio_alloc_context) avio_alloc_context = nullptr;
            decltype(&::avio_context_free) avio_context_free = nullptr;
            // libavcodec
            decltype(&::avcodec_find_decoder) avcodec_find_decoder = nullptr;
            decltype(&::avcodec_alloc_context3) avcodec_alloc_context3 = nullptr;
            decltype(&::avcodec_parameters_to_context) avcodec_parameters_to_context = nullptr;
            decltype(&::avcodec_open2) avcodec_open2 = nullptr;
            decltype(&::avcodec_send_packet) avcodec_send_packet = nullptr;
            decltype(&::avcodec_receive_frame) avcodec_receive_frame = nullptr;
            decltype(&::avcodec_flush_buffers) avcodec_flush_buffers = nullptr;
            decltype(&::avcodec_free_context) avcodec_free_context = nullptr;
            decltype(&::avcodec_get_name) avcodec_get_name = nullptr;
            decltype(&::avcodec_descriptor_get_by_name) avcodec_descriptor_get_by_name = nullptr;
            decltype(&::av_packet_alloc) av_packet_alloc = nullptr;
            decltype(&::av_packet_free) av_packet_free = nullptr;
            decltype(&::av_packet_unref) av_packet_unref = nullptr;
            // libavutil
            decltype(&::av_frame_alloc) av_frame_alloc = nullptr;
            decltype(&::av_frame_free) av_frame_free = nullptr;
            decltype(&::av_frame_unref) av_frame_unref = nullptr;
            decltype(&::av_malloc) av_malloc = nullptr;
            decltype(&::av_freep) av_freep = nullptr;
            decltype(&::av_strerror) av_strerror = nullptr;
            decltype(&::av_log_set_level) av_log_set_level = nullptr;
            decltype(&::avutil_version) avutil_version = nullptr;
            decltype(&::av_get_pix_fmt_name) av_get_pix_fmt_name = nullptr;
#if JASPER_FF_NEW_LAYOUT
            decltype(&::av_channel_layout_default) av_channel_layout_default = nullptr;
            decltype(&::swr_alloc_set_opts2) swr_alloc_set_opts2 = nullptr;
#endif
#if JASPER_FF_OLD_LAYOUT
            decltype(&::av_get_default_channel_layout) av_get_default_channel_layout = nullptr;
            decltype(&::swr_alloc_set_opts) swr_alloc_set_opts = nullptr;
#endif
            // libswresample
            decltype(&::swr_init) swr_init = nullptr;
            decltype(&::swr_convert) swr_convert = nullptr;
            decltype(&::swr_free) swr_free = nullptr;
            // libswscale (optional)
            decltype(&::sws_getCachedContext) sws_getCachedContext = nullptr;
            decltype(&::sws_scale) sws_scale = nullptr;
            decltype(&::sws_freeContext) sws_freeContext = nullptr;

            bool newLayout = false;     // the run-time libraries have AVChannelLayout fields
            bool ok = false;
            std::string error;
        };

        template<typename T>
        bool resolve(void* library, const char* name, T& out, bool required, std::string& error){
            out = library ? reinterpret_cast<T>(dlsym(library, name)) : nullptr;
            if (!out && required && error.empty())
                error = std::string("the FFmpeg libraries have no ") + name;
            return out || !required;
        }

        const Api& api(const FFmpegLibraries& libs){
            static Api value;
            static std::once_flag once;
            std::call_once(once, [&libs]{
                Api& a = value;
                std::string& e = a.error;
#define JASPER_FF_REQUIRE(lib, name) resolve(libs.lib, #name, a.name, true, e)
#define JASPER_FF_OPTIONAL(lib, name) resolve(libs.lib, #name, a.name, false, e)
                JASPER_FF_REQUIRE(avformat, avformat_alloc_context);
                JASPER_FF_REQUIRE(avformat, avformat_open_input);
                JASPER_FF_REQUIRE(avformat, avformat_find_stream_info);
                JASPER_FF_REQUIRE(avformat, av_find_best_stream);
                JASPER_FF_REQUIRE(avformat, av_read_frame);
                JASPER_FF_REQUIRE(avformat, av_seek_frame);
                JASPER_FF_REQUIRE(avformat, avformat_close_input);
                JASPER_FF_REQUIRE(avformat, avformat_free_context);
                JASPER_FF_REQUIRE(avformat, avio_alloc_context);
                JASPER_FF_REQUIRE(avformat, avio_context_free);
                JASPER_FF_REQUIRE(avcodec, avcodec_find_decoder);
                JASPER_FF_REQUIRE(avcodec, avcodec_alloc_context3);
                JASPER_FF_REQUIRE(avcodec, avcodec_parameters_to_context);
                JASPER_FF_REQUIRE(avcodec, avcodec_open2);
                JASPER_FF_REQUIRE(avcodec, avcodec_send_packet);
                JASPER_FF_REQUIRE(avcodec, avcodec_receive_frame);
                JASPER_FF_REQUIRE(avcodec, avcodec_flush_buffers);
                JASPER_FF_REQUIRE(avcodec, avcodec_free_context);
                JASPER_FF_REQUIRE(avcodec, avcodec_get_name);
                JASPER_FF_REQUIRE(avcodec, avcodec_descriptor_get_by_name);
                JASPER_FF_REQUIRE(avcodec, av_packet_alloc);
                JASPER_FF_REQUIRE(avcodec, av_packet_free);
                JASPER_FF_REQUIRE(avcodec, av_packet_unref);
                JASPER_FF_REQUIRE(avutil, av_frame_alloc);
                JASPER_FF_REQUIRE(avutil, av_frame_free);
                JASPER_FF_REQUIRE(avutil, av_frame_unref);
                JASPER_FF_REQUIRE(avutil, av_malloc);
                JASPER_FF_REQUIRE(avutil, av_freep);
                JASPER_FF_REQUIRE(avutil, av_strerror);
                JASPER_FF_REQUIRE(avutil, av_log_set_level);
                JASPER_FF_REQUIRE(avutil, avutil_version);
                JASPER_FF_OPTIONAL(avutil, av_get_pix_fmt_name);
                JASPER_FF_REQUIRE(swresample, swr_init);
                JASPER_FF_REQUIRE(swresample, swr_convert);
                JASPER_FF_REQUIRE(swresample, swr_free);
                JASPER_FF_OPTIONAL(swscale, sws_getCachedContext);
                JASPER_FF_OPTIONAL(swscale, sws_scale);
                JASPER_FF_OPTIONAL(swscale, sws_freeContext);
#if JASPER_FF_NEW_LAYOUT
                JASPER_FF_OPTIONAL(avutil, av_channel_layout_default);
                JASPER_FF_OPTIONAL(swresample, swr_alloc_set_opts2);
#endif
#if JASPER_FF_OLD_LAYOUT
                JASPER_FF_OPTIONAL(avutil, av_get_default_channel_layout);
                JASPER_FF_OPTIONAL(swresample, swr_alloc_set_opts);
#endif
#undef JASPER_FF_REQUIRE
#undef JASPER_FF_OPTIONAL
                if (!e.empty())
                    return;
#if JASPER_FF_NEW_LAYOUT
                // 5.0 shares major 59 with 5.1 but has no ch_layout fields yet (avutil 57.24)
                const unsigned version = a.avutil_version();
                a.newLayout = a.av_channel_layout_default && a.swr_alloc_set_opts2 && version >= ((57u << 16) | (24u << 8));
#endif
#if JASPER_FF_OLD_LAYOUT
                if (!a.newLayout && (!a.av_get_default_channel_layout || !a.swr_alloc_set_opts)){
                    e = "the FFmpeg libraries have no channel layout API";
                    return;
                }
#else
                if (!a.newLayout){
                    e = "the FFmpeg libraries have no channel layout API";
                    return;
                }
#endif
                a.av_log_set_level(AV_LOG_QUIET);
                a.ok = true;
            });
            return value;
        }

        std::string errorText(const Api& a, int code){
            char text[128] = {};
            if (a.av_strerror(code, text, sizeof(text)) < 0)
                std::snprintf(text, sizeof(text), "error %d", code);
            return text;
        }

        VideoColor colorOf(const AVFrame* frame){
            VideoColor color;
            switch (frame->colorspace){
                case AVCOL_SPC_BT709: color.matrix = VideoColor::Matrix::BT709; break;
                case AVCOL_SPC_BT470BG:
                case AVCOL_SPC_SMPTE170M:
                case AVCOL_SPC_FCC: color.matrix = VideoColor::Matrix::BT601; break;
                case AVCOL_SPC_BT2020_NCL:
                case AVCOL_SPC_BT2020_CL: color.matrix = VideoColor::Matrix::BT2020; break;
                case AVCOL_SPC_RGB: color.matrix = VideoColor::Matrix::IDENTITY; break;
                default: color.matrix = VideoColor::Matrix::UNSPECIFIED; break;
            }
            color.fullRange = frame->color_range == AVCOL_RANGE_JPEG || frame->format == AV_PIX_FMT_YUVJ420P;
            color.primaries = (int)frame->color_primaries;
            color.transfer = (int)frame->color_trc;
            return color;
        }

        // the engine's names of FFmpeg's codecs (VideoFormats::codecName shows them)
        std::string codecId(const Api& a, AVCodecID id){
            const char* name = a.avcodec_get_name(id);
            return name ? name : "unknown";
        }

        class Backend : public PlatformDecoderBackend {
        public:
            explicit Backend(const Api& api): a(api){}
            ~Backend() override{ close(); }

            bool open(MediaByteSource& bytes, Stream which, VideoInfo& info, MediaError& error) override{
                source = &bytes;
                stream = which;
                position = 0;
                format = a.avformat_alloc_context();
                unsigned char* buffer = format ? static_cast<unsigned char*>(a.av_malloc(IO_BUFFER)) : nullptr;
                io = buffer ? a.avio_alloc_context(buffer, IO_BUFFER, 0, this, &Backend::readPacket, nullptr, &Backend::seekPacket) : nullptr;
                if (!io){
                    if (buffer)
                        a.av_freep(&buffer);
                    return fail(error, MediaErrorCode::DECODE, "FFmpeg could not allocate its reader");
                }
                format->pb = io;
                format->flags |= AVFMT_FLAG_CUSTOM_IO;
                int result = a.avformat_open_input(&format, bytes.path().c_str(), nullptr, nullptr);
                if (result < 0){
                    format = nullptr;   // freed by avformat_open_input
                    return fail(error, readFailed ? MediaErrorCode::NETWORK : MediaErrorCode::SRC_NOT_SUPPORTED,
                                "FFmpeg cannot read it (" + errorText(a, result) + ")");
                }
                // the stream analysis reads ahead (seconds of packets): a file still downloading
                // plays without it when the headers say enough (an MP4's moov does)
                if (!bytes.isGrowing() || !headersDescribeStreams()){
                    format->max_analyze_duration = 2 * AV_TIME_BASE;
                    result = a.avformat_find_stream_info(format, nullptr);
                    if (result < 0)
                        return fail(error, readFailed ? MediaErrorCode::NETWORK : MediaErrorCode::DECODE, "FFmpeg found no streams (" + errorText(a, result) + ")");
                }
                const AVMediaType type = which == Stream::VIDEO ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
                streamIndex = a.av_find_best_stream(format, type, -1, -1, nullptr, 0);
                if (streamIndex < 0)
                    return fail(error, MediaErrorCode::SRC_NOT_SUPPORTED, which == Stream::VIDEO ? "no video track" : "no audio track");
                for (unsigned i = 0; i < format->nb_streams; i++){
                    if ((int)i != streamIndex)
                        format->streams[i]->discard = AVDISCARD_ALL;
                }
                AVStream* st = format->streams[streamIndex];
                timeBase = st->time_base;
                startTime = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
                const AVCodecParameters* par = st->codecpar;
                const std::string name = codecId(a, par->codec_id);
                const auto* codec = a.avcodec_find_decoder(par->codec_id);
                if (!codec)
                    return fail(error, MediaErrorCode::SRC_NOT_SUPPORTED, "this system's FFmpeg has no " + name + " decoder" +
                                (name == "h264" || name == "hevc" || name == "aac" ? " (Fedora's ffmpeg-free has none: install ffmpeg-libs from RPM Fusion)" : ""));
                context = a.avcodec_alloc_context3(codec);
                if (!context || a.avcodec_parameters_to_context(context, par) < 0)
                    return fail(error, MediaErrorCode::DECODE, "FFmpeg could not set up its " + name + " decoder");
                context->pkt_timebase = st->time_base;
                if (which == Stream::VIDEO){
                    threads = (int)std::clamp(std::thread::hardware_concurrency(), 1u, 4u);
                    context->thread_count = threads;
                }
                result = a.avcodec_open2(context, codec, nullptr);
                if (result < 0)
                    return fail(error, MediaErrorCode::DECODE, "FFmpeg could not open its " + name + " decoder (" + errorText(a, result) + ")");
                packet = a.av_packet_alloc();
                frame = a.av_frame_alloc();
                if (!packet || !frame)
                    return fail(error, MediaErrorCode::DECODE, "FFmpeg could not allocate a frame");

                if (format->duration > 0 && format->duration != AV_NOPTS_VALUE)
                    info.durationNs = std::max<int64_t>(info.durationNs, format->duration * 1000);   // AV_TIME_BASE is microseconds
                if (which == Stream::VIDEO){
                    info.width = par->width;
                    info.height = par->height;
                    double aspect = 1.0;
                    if (par->sample_aspect_ratio.num > 0 && par->sample_aspect_ratio.den > 0)
                        aspect = (double)par->sample_aspect_ratio.num / par->sample_aspect_ratio.den;
                    info.displayWidth = std::max(1, (int)std::lround(par->width * aspect));
                    info.displayHeight = par->height;
                    const AVRational rate = st->avg_frame_rate.num > 0 ? st->avg_frame_rate : st->r_frame_rate;
                    if (rate.num > 0 && rate.den > 0)
                        info.frameRate = (double)rate.num / rate.den;
                    info.videoCodec = name;
                    if (st->duration > 0 && st->duration != AV_NOPTS_VALUE && info.durationNs <= 0)
                        info.durationNs = toNs(st->duration + startTime);
                }else{
                    info.audioChannels = channelsOf(par);
                    info.audioCodec = name;
                }
                return true;
            }

            bool seek(double seconds) override{
                if (!format || !context)
                    return false;
                const int64_t ts = (int64_t)std::llround(std::max(seconds, 0.0) * timeBase.den / std::max(timeBase.num, 1)) + startTime;
                const int result = a.av_seek_frame(format, streamIndex, ts, AVSEEK_FLAG_BACKWARD);
                a.avcodec_flush_buffers(context);
                draining = false;
                lastPts = AV_NOPTS_VALUE;
                if (resampler)
                    a.swr_init(resampler);
                return result >= 0;
            }

            Step decodeNext(VideoFrame& out, std::vector<float>& pcm, double& pcmStart, MediaError& error) override{
                // packets until the decoder gives a frame (a few reads per call at most: AGAIN otherwise)
                for (int reads = 0; reads < 64; reads++){
                    int result = a.avcodec_receive_frame(context, frame);
                    if (result == 0){
                        const Step step = stream == Stream::VIDEO ? takePicture(out, error) : takeAudio(pcm, pcmStart, error);
                        a.av_frame_unref(frame);
                        return step;
                    }
                    if (result == AVERROR_EOF)
                        return Step::END;
                    if (result != AVERROR(EAGAIN)){
                        error.code = MediaErrorCode::DECODE;
                        error.message = "the decoder failed (" + errorText(a, result) + ")";
                        return Step::FAILURE;
                    }
                    if (draining)
                        return Step::END;
                    result = a.av_read_frame(format, packet);
                    if (result < 0){
                        if (readFailed && source->isGrowing()){
                            error.code = MediaErrorCode::NETWORK;
                            error.message = "the download stopped";
                            return Step::FAILURE;
                        }
                        if (readFailed){
                            error.code = MediaErrorCode::NETWORK;
                            error.message = "the file could not be read any more";
                            return Step::FAILURE;
                        }
                        // the end (or damage past which nothing is readable): the decoder drains
                        draining = true;
                        a.avcodec_send_packet(context, nullptr);
                        continue;
                    }
                    if (packet->stream_index == streamIndex)
                        result = a.avcodec_send_packet(context, packet);
                    a.av_packet_unref(packet);
                    if (result < 0 && result != AVERROR(EAGAIN) && result != AVERROR_INVALIDDATA){
                        error.code = MediaErrorCode::DECODE;
                        error.message = "the decoder refused a packet (" + errorText(a, result) + ")";
                        return Step::FAILURE;
                    }
                }
                return Step::AGAIN;
            }

            void close() override{
                if (scaler && a.sws_freeContext)
                    a.sws_freeContext(scaler);
                scaler = nullptr;
                if (resampler)
                    a.swr_free(&resampler);
                if (frame)
                    a.av_frame_free(&frame);
                if (packet)
                    a.av_packet_free(&packet);
                if (context)
                    a.avcodec_free_context(&context);
                if (format)
                    a.avformat_close_input(&format);
                if (io){
                    // FFmpeg may have replaced the buffer: free the one it has now
                    a.av_freep(&io->buffer);
                    a.avio_context_free(&io);
                }
            }

            int getDecoderThreads() const override{ return threads; }

        private:
            static constexpr int IO_BUFFER = 64 * 1024;

            static int readPacket(void* opaque, uint8_t* buffer, int size){
                Backend* self = static_cast<Backend*>(opaque);
                const int64_t got = self->source->readAt(self->position, buffer, (size_t)std::max(size, 0));
                if (got < 0){
                    self->readFailed = true;
                    return AVERROR(EIO);
                }
                if (got == 0)
                    return AVERROR_EOF;
                self->position += got;
                return (int)got;
            }

            static int64_t seekPacket(void* opaque, int64_t offset, int whence){
                Backend* self = static_cast<Backend*>(opaque);
                const int64_t size = self->source->size();
                whence &= ~AVSEEK_FORCE;
                if (whence == AVSEEK_SIZE)
                    return size >= 0 ? size : AVERROR(ENOSYS);
                int64_t target = -1;
                if (whence == SEEK_SET)
                    target = offset;
                else if (whence == SEEK_CUR)
                    target = self->position + offset;
                else if (whence == SEEK_END && size >= 0)
                    target = size + offset;
                if (target < 0)
                    return AVERROR(EINVAL);
                self->position = target;
                return target;
            }

            // every stream has its codec and, for video, its size from the container's headers
            bool headersDescribeStreams() const{
                if (!format || format->nb_streams == 0)
                    return false;
                for (unsigned i = 0; i < format->nb_streams; i++){
                    const AVCodecParameters* par = format->streams[i]->codecpar;
                    if (par->codec_id == AV_CODEC_ID_NONE)
                        return false;
                    if (par->codec_type == AVMEDIA_TYPE_VIDEO && (par->width <= 0 || par->height <= 0))
                        return false;
                    if (par->codec_type == AVMEDIA_TYPE_AUDIO && par->sample_rate <= 0)
                        return false;
                }
                return true;
            }

            bool fail(MediaError& error, MediaErrorCode code, const std::string& message){
                error.code = code;
                error.message = message;
                close();
                return false;
            }

            int64_t toNs(int64_t ticks) const{
                return (int64_t)std::llround((long double)(ticks - startTime) * timeBase.num * 1e9L / std::max(timeBase.den, 1));
            }

            int channelsOf(const AVCodecParameters* par) const{
#if JASPER_FF_NEW_LAYOUT
                if (a.newLayout)
                    return par->ch_layout.nb_channels;
#endif
#if JASPER_FF_OLD_LAYOUT
                return par->channels;
#else
                return 0;
#endif
            }

            int64_t framePts(){
                int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
                if (pts == AV_NOPTS_VALUE)
                    pts = lastPts != AV_NOPTS_VALUE ? lastPts : startTime;
                lastPts = pts;
                return pts;
            }

            Step takePicture(VideoFrame& out, MediaError& error){
                const int width = frame->width, height = frame->height;
                if (width <= 0 || height <= 0)
                    return Step::AGAIN;
                switch (frame->format){
                    case AV_PIX_FMT_YUV420P:
                    case AV_PIX_FMT_YUVJ420P:
                        VideoFramePlanes::copyI420(out, width, height, frame->data[0], frame->linesize[0], frame->data[1], frame->linesize[1],
                                                   frame->data[2], frame->linesize[2]);
                        break;
                    case AV_PIX_FMT_NV12:
                    case AV_PIX_FMT_NV21:
                        VideoFramePlanes::copyNV12(out, width, height, frame->data[0], frame->linesize[0], frame->data[1], frame->linesize[1],
                                                   frame->format == AV_PIX_FMT_NV21);
                        break;
                    case AV_PIX_FMT_YUV420P10LE:
                    case AV_PIX_FMT_YUV420P12LE:
                        VideoFramePlanes::copyI420x16(out, width, height, frame->format == AV_PIX_FMT_YUV420P10LE ? 10 : 12, false,
                                                      frame->data[0], frame->linesize[0], frame->data[1], frame->linesize[1],
                                                      frame->data[2], frame->linesize[2]);
                        break;
                    case AV_PIX_FMT_P010LE:
                        VideoFramePlanes::copyP01x(out, width, height, 10, frame->data[0], frame->linesize[0], frame->data[1], frame->linesize[1]);
                        break;
                    default:
                        if (!convertWithScaler(out, width, height)){
                            const char* name = a.av_get_pix_fmt_name ? a.av_get_pix_fmt_name((AVPixelFormat)frame->format) : nullptr;
                            error.code = MediaErrorCode::SRC_NOT_SUPPORTED;
                            error.message = std::string("its pixel format ") + (name ? name : std::to_string(frame->format).c_str()) +
                                            " needs libswscale (install it with FFmpeg, or convert the video to WebM AV1)";
                            return Step::FAILURE;
                        }
                        break;
                }
                out.color = colorOf(frame);
                out.ptsNs = toNs(framePts());
                return Step::FRAME;
            }

            bool convertWithScaler(VideoFrame& out, int width, int height){
                if (!a.sws_getCachedContext || !a.sws_scale)
                    return false;
                scaler = a.sws_getCachedContext(scaler, width, height, (AVPixelFormat)frame->format, width, height, AV_PIX_FMT_YUV420P,
                                                SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (!scaler)
                    return false;
                VideoFramePlanes::prepare(out, width, height);
                uint8_t* planes[4] = {out.y.data(), out.u.data(), out.v.data(), nullptr};
                const int strides[4] = {width, out.chromaWidth(), out.chromaWidth(), 0};
                a.sws_scale(scaler, frame->data, frame->linesize, 0, height, planes, strides);
                return true;
            }

            bool ensureResampler(){
                const int rate = frame->sample_rate;
                const int format = frame->format;
                int channels = 0;
#if JASPER_FF_NEW_LAYOUT
                if (a.newLayout)
                    channels = frame->ch_layout.nb_channels;
#endif
#if JASPER_FF_OLD_LAYOUT
                if (!a.newLayout){
                    channels = frame->channels;
                }
#endif
                if (resampler && rate == resamplerRate && format == resamplerFormat && channels == resamplerChannels)
                    return true;
                if (resampler)
                    a.swr_free(&resampler);
                resamplerRate = rate;
                resamplerFormat = format;
                resamplerChannels = channels;
                if (rate <= 0 || channels <= 0)
                    return false;
#if JASPER_FF_NEW_LAYOUT
                if (a.newLayout){
                    AVChannelLayout output{};
                    a.av_channel_layout_default(&output, 2);
                    AVChannelLayout input{};
                    if (frame->ch_layout.order == AV_CHANNEL_ORDER_NATIVE || frame->ch_layout.order == AV_CHANNEL_ORDER_AMBISONIC)
                        input = frame->ch_layout;
                    else
                        a.av_channel_layout_default(&input, channels);
                    if (a.swr_alloc_set_opts2(&resampler, &output, AV_SAMPLE_FMT_FLT, VideoAudioRing::SAMPLE_RATE, &input, (AVSampleFormat)format,
                                              rate, 0, nullptr) < 0)
                        resampler = nullptr;
                }
#endif
#if JASPER_FF_OLD_LAYOUT
                if (!a.newLayout){
                    const int64_t layout = frame->channel_layout ? (int64_t)frame->channel_layout : a.av_get_default_channel_layout(channels);
                    resampler = a.swr_alloc_set_opts(nullptr, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_FLT, VideoAudioRing::SAMPLE_RATE, layout,
                                                     (AVSampleFormat)format, rate, 0, nullptr);
                }
#endif
                if (!resampler)
                    return false;
                if (a.swr_init(resampler) < 0){
                    a.swr_free(&resampler);
                    return false;
                }
                return true;
            }

            Step takeAudio(std::vector<float>& pcm, double& pcmStart, MediaError& error){
                if (frame->nb_samples <= 0)
                    return Step::AGAIN;
                if (!ensureResampler()){
                    error.code = MediaErrorCode::DECODE;
                    error.message = "FFmpeg could not convert the audio to 48 kHz stereo";
                    return Step::FAILURE;
                }
                const int capacity = (int)((int64_t)frame->nb_samples * VideoAudioRing::SAMPLE_RATE / std::max(resamplerRate, 1)) + 256;
                pcm.resize((size_t)capacity * 2);
                uint8_t* output[1] = {reinterpret_cast<uint8_t*>(pcm.data())};
                const int converted = a.swr_convert(resampler, output, capacity, (const uint8_t**)frame->extended_data, frame->nb_samples);
                if (converted < 0){
                    error.code = MediaErrorCode::DECODE;
                    error.message = "FFmpeg could not convert the audio";
                    return Step::FAILURE;
                }
                pcm.resize((size_t)converted * 2);
                pcmStart = (double)toNs(framePts()) / 1e9;
                return converted > 0 ? Step::AUDIO : Step::AGAIN;
            }

            const Api& a;
            MediaByteSource* source = nullptr;
            Stream stream = Stream::VIDEO;
            int64_t position = 0;
            bool readFailed = false;
            AVFormatContext* format = nullptr;
            AVIOContext* io = nullptr;
            AVCodecContext* context = nullptr;
            AVPacket* packet = nullptr;
            AVFrame* frame = nullptr;
            SwrContext* resampler = nullptr;
            int resamplerRate = 0, resamplerFormat = -1, resamplerChannels = 0;
            SwsContext* scaler = nullptr;
            int streamIndex = -1;
            AVRational timeBase{1, 1};
            int64_t startTime = 0;
            int64_t lastPts = AV_NOPTS_VALUE;
            bool draining = false;
            int threads = 0;
        };

    }

    std::unique_ptr<PlatformDecoderBackend> create(const FFmpegLibraries& libs, std::string& error){
        const Api& a = api(libs);
        if (!a.ok){
            error = a.error;
            return nullptr;
        }
        return std::make_unique<Backend>(a);
    }

    bool hasDecoder(const FFmpegLibraries& libs, const char* codec){
        const Api& a = api(libs);
        if (!a.ok)
            return false;
        const AVCodecDescriptor* descriptor = a.avcodec_descriptor_get_by_name(codec);
        return descriptor && a.avcodec_find_decoder(descriptor->id) != nullptr;
    }

}
}
