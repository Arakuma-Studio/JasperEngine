// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Android video decoders (docs/superpowers/specs/2026-10-31-video-mp4-design.md §6.6
// "Android"): the NDK's AMediaExtractor and AMediaCodec (libmediandk, API 21) in ByteBuffer mode
// (no surface: the planes are copied into the engine's I420 frames on the source's worker).
// Bytes: API 28+ reads every source through an AMediaDataSource over MediaByteSource (resource
// pack entries, files still downloading); below it the extractor takes a file descriptor (a user
// file, or an uncompressed APK asset through AAsset_openFileDescriptor64; the exporter keeps
// .mp4/.m4v/.mov/.webm uncompressed), else the bytes are extracted once to the cache. The API 28
// functions are looked up with dlsym so the minSdk 21 build links. Vendor-tiled output formats
// are retried with Android's software decoder. Audio: a second extractor and codec (PCM16 or
// float out), resampled to 48 kHz stereo.

#include "media/PlatformVideoBackend.h"

#include "Log.h"
#include "MediaCodecBackend.h"
#include "System.h"
#include "htmlui/compat/Hash.h"

#include <android/api-level.h>
#include <android/asset_manager.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaDataSource.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <mutex>
#include <string>

using namespace jasperengine;

namespace {

    // the APK's assets (NativeEngine, mediaCodecSetAssetManager)
    AAssetManager* assetManager = nullptr;

    // COLOR_Format* of MediaCodecInfo.CodecCapabilities
    constexpr int32_t COLOR_FormatYUV420Planar = 19;
    constexpr int32_t COLOR_FormatYUV420SemiPlanar = 21;
    constexpr int32_t COLOR_FormatYUV420Flexible = 0x7F420888;
    constexpr int32_t COLOR_QCOM_FormatYUV420SemiPlanar = 0x7FA30C00;
    constexpr int32_t COLOR_FormatYUVP010 = 54;
    // AudioFormat.ENCODING_PCM_FLOAT
    constexpr int32_t PCM_ENCODING_FLOAT = 4;

    // the API 28 data source, found at run time
    struct DataSourceApi {
        AMediaDataSource* (*create)() = nullptr;
        void (*destroy)(AMediaDataSource*) = nullptr;
        void (*setUserdata)(AMediaDataSource*, void*) = nullptr;
        void (*setReadAt)(AMediaDataSource*, AMediaDataSourceReadAt) = nullptr;
        void (*setGetSize)(AMediaDataSource*, AMediaDataSourceGetSize) = nullptr;
        void (*setClose)(AMediaDataSource*, AMediaDataSourceClose) = nullptr;
        media_status_t (*setDataSourceCustom)(AMediaExtractor*, AMediaDataSource*) = nullptr;
        bool ok = false;
    };

    const DataSourceApi& dataSourceApi(){
        static DataSourceApi api;
        static std::once_flag once;
        std::call_once(once, []{
            if (android_get_device_api_level() < 28)
                return;
            void* library = dlopen("libmediandk.so", RTLD_NOW);
            if (!library)
                return;
            api.create = reinterpret_cast<AMediaDataSource* (*)()>(dlsym(library, "AMediaDataSource_new"));
            api.destroy = reinterpret_cast<void (*)(AMediaDataSource*)>(dlsym(library, "AMediaDataSource_delete"));
            api.setUserdata = reinterpret_cast<void (*)(AMediaDataSource*, void*)>(dlsym(library, "AMediaDataSource_setUserdata"));
            api.setReadAt = reinterpret_cast<void (*)(AMediaDataSource*, AMediaDataSourceReadAt)>(dlsym(library, "AMediaDataSource_setReadAt"));
            api.setGetSize = reinterpret_cast<void (*)(AMediaDataSource*, AMediaDataSourceGetSize)>(dlsym(library, "AMediaDataSource_setGetSize"));
            api.setClose = reinterpret_cast<void (*)(AMediaDataSource*, AMediaDataSourceClose)>(dlsym(library, "AMediaDataSource_setClose"));
            api.setDataSourceCustom = reinterpret_cast<media_status_t (*)(AMediaExtractor*, AMediaDataSource*)>(
                dlsym(library, "AMediaExtractor_setDataSourceCustom"));
            api.ok = api.create && api.destroy && api.setUserdata && api.setReadAt && api.setGetSize && api.setClose && api.setDataSourceCustom;
        });
        return api;
    }

    const char* mimeOf(const std::string& codec){
        static const std::map<std::string, const char*> mimes = {
            {"h264", "video/avc"}, {"hevc", "video/hevc"}, {"vp8", "video/x-vnd.on2.vp8"}, {"vp9", "video/x-vnd.on2.vp9"},
            {"av1", "video/av01"}, {"mpeg4", "video/mp4v-es"}, {"aac", "audio/mp4a-latm"}, {"mp3", "audio/mpeg"},
            {"opus", "audio/opus"}, {"vorbis", "audio/vorbis"}, {"flac", "audio/flac"}, {"ac3", "audio/ac3"}, {"eac3", "audio/eac3"},
            {"alac", "audio/alac"},
        };
        auto it = mimes.find(codec);
        return it == mimes.end() ? nullptr : it->second;
    }

    // Android's own software decoders (Codec2 names, then the older OMX ones)
    std::vector<const char*> softwareDecoders(const std::string& mime){
        if (mime == "video/avc") return {"c2.android.avc.decoder", "OMX.google.h264.decoder"};
        if (mime == "video/hevc") return {"c2.android.hevc.decoder", "OMX.google.hevc.decoder"};
        if (mime == "video/x-vnd.on2.vp9") return {"c2.android.vp9.decoder", "OMX.google.vp9.decoder"};
        if (mime == "video/x-vnd.on2.vp8") return {"c2.android.vp8.decoder", "OMX.google.vp8.decoder"};
        if (mime == "video/av01") return {"c2.android.av1.decoder"};
        return {};
    }

    std::string codecNameOf(const std::string& mime){
        static const std::map<std::string, const char*> names = {
            {"video/avc", "avc1"}, {"video/hevc", "hvc1"}, {"video/x-vnd.on2.vp8", "V_VP8"}, {"video/x-vnd.on2.vp9", "V_VP9"},
            {"video/av01", "av01"}, {"audio/mp4a-latm", "mp4a"}, {"audio/mpeg", "mp3 "}, {"audio/opus", "Opus"}, {"audio/vorbis", "A_VORBIS"},
            {"audio/flac", "fLaC"},
        };
        auto it = names.find(mime);
        return it == names.end() ? mime : it->second;
    }

    int32_t getInt(AMediaFormat* format, const char* key, int32_t fallback){
        int32_t value = 0;
        return format && AMediaFormat_getInt32(format, key, &value) ? value : fallback;
    }

    // the MIME types a decoder was made for in this process: when one cannot be made again, the
    // device's instances are in use (a limit), not missing
    std::mutex createdMutex;
    std::set<std::string> createdMimes;

    void noteCreated(const std::string& mime){
        std::lock_guard<std::mutex> lock(createdMutex);
        createdMimes.insert(mime);
    }

    bool hasDecoderFor(const std::string& mime){
        std::lock_guard<std::mutex> lock(createdMutex);
        return createdMimes.count(mime) > 0;
    }

    class Backend : public PlatformDecoderBackend {
    public:
        ~Backend() override{ close(); }

        bool open(MediaByteSource& bytes, Stream which, VideoInfo& info, MediaError& error) override{
            source = &bytes;
            stream = which;
            extractor = AMediaExtractor_new();
            if (!extractor)
                return fail(error, MediaErrorCode::DECODE, "the device's media extractor could not be made");
            if (!attachSource(error))
                return false;
            // the first track of the stream's kind
            const size_t tracks = AMediaExtractor_getTrackCount(extractor);
            const char* prefix = which == Stream::VIDEO ? "video/" : "audio/";
            AMediaFormat* format = nullptr;
            for (size_t i = 0; i < tracks; i++){
                AMediaFormat* candidate = AMediaExtractor_getTrackFormat(extractor, i);
                const char* mime = nullptr;
                if (candidate && AMediaFormat_getString(candidate, "mime", &mime) && mime && std::strncmp(mime, prefix, 6) == 0){
                    track = (int)i;
                    this->mime = mime;
                    format = candidate;
                    break;
                }
                if (candidate)
                    AMediaFormat_delete(candidate);
            }
            if (!format)
                return fail(error, MediaErrorCode::SRC_NOT_SUPPORTED, which == Stream::VIDEO ? "no video track" : "no audio track");
            AMediaExtractor_selectTrack(extractor, (size_t)track);
            int64_t durationUs = 0;
            if (AMediaFormat_getInt64(format, "durationUs", &durationUs) && durationUs > 0)
                info.durationNs = std::max<int64_t>(info.durationNs, durationUs * 1000);

            if (which == Stream::VIDEO){
                // ByteBuffer output in a YUV 4:2:0 layout the decoder chooses (read back from its output format)
                AMediaFormat_setInt32(format, "color-format", COLOR_FormatYUV420Flexible);
                info.width = getInt(format, "width", 0);
                info.height = getInt(format, "height", 0);
                info.displayWidth = getInt(format, "display-width", info.width);
                info.displayHeight = getInt(format, "display-height", info.height);
                float rate = 0.0f;
                int32_t intRate = 0;
                if (AMediaFormat_getFloat(format, "frame-rate", &rate) && rate > 0.0f)
                    info.frameRate = rate;
                else if (AMediaFormat_getInt32(format, "frame-rate", &intRate) && intRate > 0)
                    info.frameRate = intRate;
                info.videoCodec = codecNameOf(mime);
            }else{
                channels = getInt(format, "channel-count", 2);
                sampleRate = getInt(format, "sample-rate", 48000);
                info.audioChannels = channels;
                info.audioCodec = codecNameOf(mime);
                resampler.reset(sampleRate, channels);
            }
            trackFormat = format;
            if (!startCodec(nullptr, error))
                return false;
            return true;
        }

        bool seek(double seconds) override{
            if (!extractor || !codec)
                return false;
            const int64_t us = (int64_t)std::llround(std::max(seconds, 0.0) * 1e6);
            if (AMediaExtractor_seekTo(extractor, us, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC) != AMEDIA_OK)
                return false;
            AMediaCodec_flush(codec);
            inputEnded = false;
            outputEnded = false;
            drainWaits = 0;
            lastOutputUs = -1;
            skipUntilUs = -1;
            resampler.flush();
            return true;
        }

        Step decodeNext(VideoFrame& frame, std::vector<float>& pcm, double& pcmStart, MediaError& error) override{
            for (int round = 0; round < 32; round++){
                // input: the extractor's samples into the codec
                bool queued = false;
                if (!inputEnded){
                    const ssize_t in = AMediaCodec_dequeueInputBuffer(codec, 0);
                    if (in >= 0){
                        size_t capacity = 0;
                        uint8_t* buffer = AMediaCodec_getInputBuffer(codec, (size_t)in, &capacity);
                        const ssize_t size = buffer ? AMediaExtractor_readSampleData(extractor, buffer, capacity) : -1;
                        if (size < 0){
                            if (readFailed){
                                error.code = MediaErrorCode::NETWORK;
                                error.message = source->isGrowing() ? "the download stopped" : "the file could not be read any more";
                                return Step::FAILURE;
                            }
                            AMediaCodec_queueInputBuffer(codec, (size_t)in, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                            inputEnded = true;
                        }else{
                            const int64_t time = AMediaExtractor_getSampleTime(extractor);
                            AMediaCodec_queueInputBuffer(codec, (size_t)in, 0, (size_t)size, (uint64_t)std::max<int64_t>(time, 0), 0);
                            AMediaExtractor_advance(extractor);
                        }
                        queued = true;
                    }else if (in != AMEDIACODEC_INFO_TRY_AGAIN_LATER){
                        // the codec died (reclaimed by the system, a media server restart): a new one
                        if (!recreateCodec(error, (int)in))
                            return Step::FAILURE;
                        continue;
                    }
                }
                // output: no wait while there is input to give, else up to 10 ms (the worker
                // sleeps here instead of spinning)
                AMediaCodecBufferInfo bufferInfo;
                const ssize_t out = AMediaCodec_dequeueOutputBuffer(codec, &bufferInfo, queued ? 0 : 10000);
                if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED){
                    readOutputFormat();
                    if (stream == Stream::VIDEO && tiled && !retriedSoftware){
                        // a vendor-tiled layout the engine cannot read: Android's software decoder instead
                        retriedSoftware = true;
                        if (!restartWithSoftware(error))
                            return Step::FAILURE;
                    }
                    continue;
                }
                if (out == AMEDIACODEC_INFO_TRY_AGAIN_LATER || out == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED){
                    if (outputEnded)
                        return Step::END;
                    // the end queued, nothing more for a second: done
                    if (inputEnded && out == AMEDIACODEC_INFO_TRY_AGAIN_LATER && ++drainWaits > 100)
                        return Step::END;
                    if (queued || out == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
                        continue;
                    // waited 10 ms with no input to give: the source asks again (its queue, a seek)
                    return Step::AGAIN;
                }
                if (out < 0){
                    if (!recreateCodec(error, (int)out))
                        return Step::FAILURE;
                    continue;
                }
                drainWaits = 0;
                const bool end = (bufferInfo.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
                // after a new codec: what was given out already comes again from the keyframe
                const bool repeat = skipUntilUs >= 0 && bufferInfo.presentationTimeUs <= skipUntilUs;
                Step step = Step::AGAIN;
                if (bufferInfo.size > 0 && !repeat){
                    size_t capacity = 0;
                    uint8_t* data = AMediaCodec_getOutputBuffer(codec, (size_t)out, &capacity);
                    if (data){
                        const uint8_t* start = data + bufferInfo.offset;
                        step = stream == Stream::VIDEO ? takePicture(start, (size_t)bufferInfo.size, bufferInfo.presentationTimeUs, frame)
                                                       : takeAudio(start, (size_t)bufferInfo.size, bufferInfo.presentationTimeUs, pcm, pcmStart);
                        if (step != Step::AGAIN)
                            lastOutputUs = bufferInfo.presentationTimeUs;
                    }
                }
                if (!repeat)
                    skipUntilUs = -1;
                AMediaCodec_releaseOutputBuffer(codec, (size_t)out, false);
                if (end)
                    outputEnded = true;
                if (step != Step::AGAIN)
                    return step;
                if (end)
                    return Step::END;
            }
            return Step::AGAIN;
        }

        void close() override{
            stopCodec();
            if (trackFormat)
                AMediaFormat_delete(trackFormat);
            trackFormat = nullptr;
            if (extractor)
                AMediaExtractor_delete(extractor);
            extractor = nullptr;
            if (dataSource)
                dataSourceApi().destroy(dataSource);
            dataSource = nullptr;
            if (fd >= 0)
                ::close(fd);
            fd = -1;
        }

        bool wasResourceLimited() const override{ return resourceLimited; }

    private:
        bool fail(MediaError& error, MediaErrorCode code, const std::string& message){
            error.code = code;
            error.message = message;
            close();
            return false;
        }

        static ssize_t readAt(void* userdata, off64_t offset, void* buffer, size_t size){
            Backend* self = static_cast<Backend*>(userdata);
            // a size-0 read asks whether the source is still there
            if (size == 0)
                return 0;
            const int64_t got = self->source->readAt((int64_t)offset, buffer, size);
            if (got < 0){
                self->readFailed = true;
                return -1;
            }
            return (ssize_t)got;
        }

        static ssize_t getSize(void* userdata){
            Backend* self = static_cast<Backend*>(userdata);
            const int64_t size = self->source->size();
            return size >= 0 ? (ssize_t)size : -1;
        }

        static void closeSource(void*){
        }

        bool attachSource(MediaError& error){
            const DataSourceApi& api = dataSourceApi();
            if (api.ok){
                dataSource = api.create();
                if (dataSource){
                    api.setUserdata(dataSource, this);
                    api.setReadAt(dataSource, &Backend::readAt);
                    api.setGetSize(dataSource, &Backend::getSize);
                    api.setClose(dataSource, &Backend::closeSource);
                    if (api.setDataSourceCustom(extractor, dataSource) == AMEDIA_OK)
                        return true;
                }
                return fail(error, readFailed ? MediaErrorCode::NETWORK : MediaErrorCode::SRC_NOT_SUPPORTED,
                            "the device cannot read this video's container");
            }
            if (source->isGrowing())
                return fail(error, MediaErrorCode::SRC_NOT_SUPPORTED, "Android before 9 plays a downloaded video once it is complete");
            off64_t start = 0, length = 0;
            const std::string systemPath = source->systemPath();
            if (!systemPath.empty()){
                fd = ::open(systemPath.c_str(), O_RDONLY | O_CLOEXEC);
                struct stat st{};
                if (fd >= 0 && fstat(fd, &st) == 0)
                    length = st.st_size;
            }else if (assetManager){
                // an APK asset: uncompressed ones have a descriptor of their own
                std::string assetPath = source->path();
                if (!assetPath.empty() && assetPath[0] == '/')
                    assetPath = assetPath.substr(1);
                if (AAsset* asset = AAssetManager_open(assetManager, assetPath.c_str(), AASSET_MODE_UNKNOWN)){
                    fd = AAsset_openFileDescriptor64(asset, &start, &length);
                    AAsset_close(asset);
                }
            }
            if (fd < 0 && !extractToCache(error))
                return false;
            if (fd < 0)
                return fail(error, MediaErrorCode::NETWORK, "'" + source->path() + "' could not be opened");
            if (length <= 0){
                struct stat st{};
                if (fstat(fd, &st) == 0)
                    length = st.st_size - start;
            }
            if (AMediaExtractor_setDataSourceFd(extractor, fd, start, length) != AMEDIA_OK)
                return fail(error, MediaErrorCode::SRC_NOT_SUPPORTED, "the device cannot read this video's container");
            return true;
        }

        // a compressed asset or a pack entry below API 28: its bytes once into the cache folder
        bool extractToCache(MediaError& error){
            const int64_t size = source->size();
            if (size <= 0)
                return fail(error, MediaErrorCode::NETWORK, "'" + source->path() + "' could not be read");
            const std::string dir = System::instance().getUserDataPath() + "/.cache/media/extracted";
            ::mkdir((System::instance().getUserDataPath() + "/.cache").c_str(), 0700);
            ::mkdir((System::instance().getUserDataPath() + "/.cache/media").c_str(), 0700);
            ::mkdir(dir.c_str(), 0700);
            const std::string file = dir + "/" + htmlui::compat::sha256Hex(source->path() + ":" + std::to_string(size)).substr(0, 24);
            struct stat st{};
            if (::stat(file.c_str(), &st) != 0 || st.st_size != size){
                const std::string temp = file + ".tmp";
                FILE* out = std::fopen(temp.c_str(), "wb");
                if (!out)
                    return fail(error, MediaErrorCode::NETWORK, "the video could not be extracted for the decoder");
                std::vector<uint8_t> buffer(1 << 20);
                int64_t done = 0;
                bool ok = true;
                while (done < size && ok){
                    const int64_t got = source->readAt(done, buffer.data(), buffer.size());
                    ok = got > 0 && std::fwrite(buffer.data(), 1, (size_t)got, out) == (size_t)got;
                    done += std::max<int64_t>(got, 0);
                }
                std::fclose(out);
                if (!ok || std::rename(temp.c_str(), file.c_str()) != 0){
                    std::remove(temp.c_str());
                    return fail(error, MediaErrorCode::NETWORK, "the video could not be extracted for the decoder");
                }
            }
            fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
            return true;
        }

        bool startCodec(const char* name, MediaError& error){
            codec = name ? AMediaCodec_createCodecByName(name) : AMediaCodec_createDecoderByType(mime.c_str());
            if (!codec){
                // a type the device decodes elsewhere in the app (another video) but no free instance
                // looks the same as none: one retry by the source, then "no decoder"
                resourceLimited = !name && hasDecoderFor(mime);
                return fail(error, resourceLimited ? MediaErrorCode::DECODE : MediaErrorCode::SRC_NOT_SUPPORTED,
                            resourceLimited ? "the " + mime + " decoder could not be created" : "this device has no " + mime + " decoder");
            }
            media_status_t status = AMediaCodec_configure(codec, trackFormat, nullptr, nullptr, 0);
            if (status == AMEDIA_OK)
                status = AMediaCodec_start(codec);
            if (status != AMEDIA_OK){
                // the device's decoder instances are all in use (or its memory): the source retries once
                resourceLimited = status == AMEDIACODEC_ERROR_INSUFFICIENT_RESOURCE;
                AMediaCodec_delete(codec);
                codec = nullptr;
                return fail(error, MediaErrorCode::DECODE, "the " + mime + " decoder could not start (" + std::to_string(status) + ")");
            }
            inputEnded = outputEnded = false;
            colorFormat = COLOR_FormatYUV420Flexible;
            noteCreated(mime);
            return true;
        }

        void stopCodec(){
            if (codec){
                AMediaCodec_stop(codec);
                AMediaCodec_delete(codec);
            }
            codec = nullptr;
        }

        // a dead codec (reclaimed for a foreground app, the media server restarted): a new one,
        // the extractor back to the keyframe before the last output (what was given out already
        // is skipped); twice at most, then the error
        bool recreateCodec(MediaError& error, int status){
            if (recreated >= 2 || !extractor){
                error.code = MediaErrorCode::DECODE;
                error.message = "the decoder failed (" + std::to_string(status) + ")";
                return false;
            }
            recreated++;
            Log::warn("[Video] the %s decoder stopped (%d): a new one resumes the video", mime.c_str(), status);
            stopCodec();
            if (!startCodec(softwareName.empty() ? nullptr : softwareName.c_str(), error))
                return false;
            AMediaExtractor_seekTo(extractor, std::max<int64_t>(lastOutputUs, 0), AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
            skipUntilUs = lastOutputUs;
            drainWaits = 0;
            resampler.flush();
            return true;
        }

        bool restartWithSoftware(MediaError& error){
            stopCodec();
            const int64_t position = AMediaExtractor_getSampleTime(extractor);
            AMediaExtractor_seekTo(extractor, std::max<int64_t>(position, 0), AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
            for (const char* name : softwareDecoders(mime)){
                codec = AMediaCodec_createCodecByName(name);
                if (!codec)
                    continue;
                if (AMediaCodec_configure(codec, trackFormat, nullptr, nullptr, 0) == AMEDIA_OK && AMediaCodec_start(codec) == AMEDIA_OK){
                    tiled = false;
                    softwareName = name;
                    inputEnded = outputEnded = false;
                    Log::verbose("[Video] the device's %s decoder gives a tiled layout: %s decodes instead", mime.c_str(), name);
                    return true;
                }
                AMediaCodec_delete(codec);
                codec = nullptr;
            }
            error.code = MediaErrorCode::DECODE;
            error.message = "this device's " + mime + " decoder gives a pixel layout the engine cannot read, and Android's software decoder is missing";
            return false;
        }

        void readOutputFormat(){
            AMediaFormat* format = AMediaCodec_getOutputFormat(codec);
            if (!format)
                return;
            if (stream == Stream::VIDEO){
                outWidth = getInt(format, "width", outWidth);
                outHeight = getInt(format, "height", outHeight);
                colorFormat = getInt(format, "color-format", colorFormat);
                stride = getInt(format, "stride", outWidth);
                sliceHeight = getInt(format, "slice-height", outHeight);
                cropLeft = getInt(format, "crop-left", 0);
                cropTop = getInt(format, "crop-top", 0);
                cropRight = getInt(format, "crop-right", outWidth - 1);
                cropBottom = getInt(format, "crop-bottom", outHeight - 1);
                if (stride <= 0) stride = outWidth;
                if (sliceHeight <= 0) sliceHeight = outHeight;
                const int32_t standard = getInt(format, "color-standard", 0);
                const int32_t range = getInt(format, "color-range", 0);
                color = VideoColor();
                if (standard == 1) color.matrix = VideoColor::Matrix::BT709;
                else if (standard == 2 || standard == 4) color.matrix = VideoColor::Matrix::BT601;
                else if (standard == 6) color.matrix = VideoColor::Matrix::BT2020;
                color.fullRange = range == 1;
                tiled = colorFormat != COLOR_FormatYUV420Planar && colorFormat != COLOR_FormatYUV420SemiPlanar &&
                        colorFormat != COLOR_FormatYUV420Flexible && colorFormat != COLOR_QCOM_FormatYUV420SemiPlanar &&
                        colorFormat != COLOR_FormatYUVP010;
            }else{
                channels = getInt(format, "channel-count", channels);
                sampleRate = getInt(format, "sample-rate", sampleRate);
                pcmFloat = getInt(format, "pcm-encoding", 2) == PCM_ENCODING_FLOAT;
                resampler.reset(sampleRate, channels);
            }
            AMediaFormat_delete(format);
        }

        Step takePicture(const uint8_t* data, size_t size, int64_t timeUs, VideoFrame& frame){
            if (outWidth <= 0)
                readOutputFormat();
            const int width = cropRight - cropLeft + 1, height = cropBottom - cropTop + 1;
            if (width <= 0 || height <= 0 || tiled)
                return Step::AGAIN;
            const size_t lumaBytes = (size_t)stride * (size_t)sliceHeight;
            if (colorFormat == COLOR_FormatYUVP010){
                // 16-bit samples: stride counts bytes
                if (size < lumaBytes + lumaBytes / 2)
                    return Step::AGAIN;
                const uint8_t* y = data + (size_t)cropTop * stride + (size_t)cropLeft * 2;
                const uint8_t* uv = data + lumaBytes + (size_t)(cropTop / 2) * stride + (size_t)cropLeft * 2;
                VideoFramePlanes::copyP01x(frame, width, height, 10, y, stride, uv, stride);
            }else if (colorFormat == COLOR_FormatYUV420Planar){
                const int chromaStride = (stride + 1) / 2;
                const size_t chromaBytes = (size_t)chromaStride * (size_t)((sliceHeight + 1) / 2);
                if (size < lumaBytes + 2 * chromaBytes)
                    return Step::AGAIN;
                const uint8_t* y = data + (size_t)cropTop * stride + cropLeft;
                const uint8_t* u = data + lumaBytes + (size_t)(cropTop / 2) * chromaStride + cropLeft / 2;
                const uint8_t* v = data + lumaBytes + chromaBytes + (size_t)(cropTop / 2) * chromaStride + cropLeft / 2;
                VideoFramePlanes::copyI420(frame, width, height, y, stride, u, chromaStride, v, chromaStride);
            }else{
                // semi-planar (Flexible decoders answer with 21 in ByteBuffer mode)
                if (size < lumaBytes + lumaBytes / 2 - (size_t)stride / 2)
                    return Step::AGAIN;
                const uint8_t* y = data + (size_t)cropTop * stride + cropLeft;
                const uint8_t* uv = data + lumaBytes + (size_t)(cropTop / 2) * stride + (cropLeft & ~1);
                VideoFramePlanes::copyNV12(frame, width, height, y, stride, uv, stride);
            }
            frame.color = color;
            frame.ptsNs = timeUs * 1000;
            return Step::FRAME;
        }

        Step takeAudio(const uint8_t* data, size_t size, int64_t timeUs, std::vector<float>& pcm, double& pcmStart){
            pcm.clear();
            if (pcmFloat){
                const size_t frames = size / (sizeof(float) * (size_t)std::max(channels, 1));
                resampler.process(reinterpret_cast<const float*>(data), frames, pcm);
            }else{
                const size_t frames = size / (sizeof(int16_t) * (size_t)std::max(channels, 1));
                resampler.processInt16(reinterpret_cast<const int16_t*>(data), frames, pcm);
            }
            pcmStart = (double)timeUs / 1e6;
            return pcm.empty() ? Step::AGAIN : Step::AUDIO;
        }

        MediaByteSource* source = nullptr;
        Stream stream = Stream::VIDEO;
        AMediaExtractor* extractor = nullptr;
        AMediaDataSource* dataSource = nullptr;
        AMediaCodec* codec = nullptr;
        AMediaFormat* trackFormat = nullptr;
        int fd = -1;
        int track = -1;
        std::string mime;
        bool inputEnded = false;
        bool outputEnded = false;
        int drainWaits = 0;
        bool readFailed = false;
        bool resourceLimited = false;
        // video output layout
        int32_t outWidth = 0, outHeight = 0, colorFormat = COLOR_FormatYUV420Flexible, stride = 0, sliceHeight = 0;
        int32_t cropLeft = 0, cropTop = 0, cropRight = -1, cropBottom = -1;
        bool tiled = false;
        bool retriedSoftware = false;
        std::string softwareName;       // the software decoder in use ("": the device's)
        int recreated = 0;
        int64_t lastOutputUs = -1;      // the last frame or audio chunk given out
        int64_t skipUntilUs = -1;       // after a new codec: outputs up to this are repeats
        VideoColor color;
        // audio
        int channels = 2;
        int sampleRate = 48000;
        bool pcmFloat = false;
        AudioResampler48k resampler;
    };

    std::mutex supportMutex;
    std::map<std::string, platformvideo::Support> supportCache;

}

void jasperengine::mediaCodecSetAssetManager(AAssetManager* manager){
    assetManager = manager;
}

const char* platformvideo::backendName(){
    return "mediacodec";
}

bool platformvideo::isAvailable(std::string*){
    return true;
}

std::unique_ptr<PlatformDecoderBackend> platformvideo::createBackend(){
    return std::make_unique<Backend>();
}

platformvideo::Support platformvideo::supports(const std::string& codec){
    const char* mime = mimeOf(codec);
    if (!mime)
        return Support::No;
    std::lock_guard<std::mutex> lock(supportMutex);
    auto it = supportCache.find(codec);
    if (it != supportCache.end())
        return it->second;
    // a decoder of that type can be made (asked once: made and freed)
    AMediaCodec* probe = AMediaCodec_createDecoderByType(mime);
    const Support support = probe ? Support::Yes : Support::No;
    if (probe)
        AMediaCodec_delete(probe);
    supportCache[codec] = support;
    return support;
}

bool platformvideo::readsContainer(const std::string& container){
    return container == "mp4" || container == "mov" || container == "3gp" || container == "webm" || container == "matroska";
}

bool platformvideo::supportsGrowingSources(){
    return dataSourceApi().ok;
}

std::string platformvideo::unsupportedHint(const std::string& codec){
    return "this device has no " + codec + " decoder (HEVC and AV1 depend on the device); convert the video to WebM AV1, which plays everywhere";
}
