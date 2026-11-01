// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VideoDecoder.h"
#include "thread/ThreadUtil.h"

#include "Log.h"

#include <dav1d/dav1d.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace jasperengine;

// dav1d's CPU dispatch (src/cpu.h, not in the public header): the feature mask and the features
// it detected
extern "C" {
    void dav1d_set_cpu_flags_mask(unsigned mask);
    extern unsigned dav1d_cpu_flags;
}

namespace {

    std::atomic<int> forcedThreads{0};
    std::atomic<size_t> livePictureBytes{0};

    // dav1d's picture buffers (its default allocator's layout, src/picture.c: 128-pixel aligned
    // sizes, strides padded off multiples of 1024, DAV1D_PICTURE_ALIGNMENT-aligned planes and
    // padding), from a pool of freed buffers of the same size
    struct BufferHeader {
        size_t size;      // the pool size class (what was asked for)
        void* raw;        // what malloc returned
    };

    uint8_t* poolPop(Av1Decoder::PicturePool* pool, size_t size){
        {
            std::lock_guard<std::mutex> lock(pool->mutex);
            for (size_t i = 0; i < pool->free.size(); i++){
                if (pool->free[i].first == size){
                    uint8_t* buffer = pool->free[i].second;
                    pool->free.erase(pool->free.begin() + (std::ptrdiff_t)i);
                    return buffer;
                }
            }
        }
        const size_t total = size + DAV1D_PICTURE_ALIGNMENT + sizeof(BufferHeader) + DAV1D_PICTURE_ALIGNMENT;
        void* raw = std::malloc(total);
        if (!raw)
            return nullptr;
        uintptr_t start = reinterpret_cast<uintptr_t>(raw) + sizeof(BufferHeader);
        start = (start + DAV1D_PICTURE_ALIGNMENT - 1) & ~(uintptr_t)(DAV1D_PICTURE_ALIGNMENT - 1);
        uint8_t* buffer = reinterpret_cast<uint8_t*>(start);
        BufferHeader* header = reinterpret_cast<BufferHeader*>(buffer) - 1;
        header->size = size;
        header->raw = raw;
        pool->bytes += total;
        livePictureBytes += total;
        return buffer;
    }

    void bufferFree(Av1Decoder::PicturePool* pool, uint8_t* buffer){
        BufferHeader* header = reinterpret_cast<BufferHeader*>(buffer) - 1;
        const size_t total = header->size + DAV1D_PICTURE_ALIGNMENT + sizeof(BufferHeader) + DAV1D_PICTURE_ALIGNMENT;
        pool->bytes -= total;
        livePictureBytes -= total;
        std::free(header->raw);
    }

    void poolPush(Av1Decoder::PicturePool* pool, uint8_t* buffer){
        BufferHeader* header = reinterpret_cast<BufferHeader*>(buffer) - 1;
        std::lock_guard<std::mutex> lock(pool->mutex);
        // a few of each size are kept: the reference frames and those in flight come back
        size_t same = 0;
        for (const auto& entry : pool->free)
            same += entry.first == header->size ? 1 : 0;
        if (same >= 4){
            bufferFree(pool, buffer);
            return;
        }
        pool->free.emplace_back(header->size, buffer);
    }

    int allocPicture(Dav1dPicture* p, void* cookie){
        Av1Decoder::PicturePool* pool = static_cast<Av1Decoder::PicturePool*>(cookie);
        const int hbd = p->p.bpc > 8;
        const int alignedW = (p->p.w + 127) & ~127;
        const int alignedH = (p->p.h + 127) & ~127;
        const int hasChroma = p->p.layout != DAV1D_PIXEL_LAYOUT_I400;
        const int ssVer = p->p.layout == DAV1D_PIXEL_LAYOUT_I420;
        const int ssHor = p->p.layout != DAV1D_PIXEL_LAYOUT_I444;
        ptrdiff_t yStride = (ptrdiff_t)alignedW << hbd;
        ptrdiff_t uvStride = hasChroma ? yStride >> ssHor : 0;
        if (!(yStride & 1023))
            yStride += DAV1D_PICTURE_ALIGNMENT;
        if (!(uvStride & 1023) && hasChroma)
            uvStride += DAV1D_PICTURE_ALIGNMENT;
        p->stride[0] = yStride;
        p->stride[1] = uvStride;
        const size_t ySize = (size_t)yStride * (size_t)alignedH;
        const size_t uvSize = (size_t)uvStride * (size_t)(alignedH >> ssVer);
        uint8_t* buffer = poolPop(pool, ySize + 2 * uvSize);
        if (!buffer)
            return DAV1D_ERR(ENOMEM);
        p->allocator_data = buffer;
        p->data[0] = buffer;
        p->data[1] = hasChroma ? buffer + ySize : nullptr;
        p->data[2] = hasChroma ? buffer + ySize + uvSize : nullptr;
        return 0;
    }

    void releasePicture(Dav1dPicture* p, void* cookie){
        poolPush(static_cast<Av1Decoder::PicturePool*>(cookie), static_cast<uint8_t*>(p->allocator_data));
    }

    void logMessage(void*, const char* format, va_list args){
        char buffer[512];
        std::vsnprintf(buffer, sizeof(buffer), format, args);
        size_t length = std::strlen(buffer);
        while (length > 0 && (buffer[length - 1] == '\n' || buffer[length - 1] == '\r'))
            buffer[--length] = '\0';
        Log::verbose("[HTML UI] dav1d: %s", buffer);
    }

    VideoColor::Matrix matrixOf(enum Dav1dMatrixCoefficients mtrx){
        switch (mtrx){
            case DAV1D_MC_IDENTITY: return VideoColor::Matrix::IDENTITY;
            case DAV1D_MC_BT709: return VideoColor::Matrix::BT709;
            case DAV1D_MC_BT470BG:
            case DAV1D_MC_BT601:
            case DAV1D_MC_FCC: return VideoColor::Matrix::BT601;
            case DAV1D_MC_BT2020_NCL:
            case DAV1D_MC_BT2020_CL: return VideoColor::Matrix::BT2020;
            default: return VideoColor::Matrix::UNSPECIFIED;
        }
    }

    // one plane into a tightly packed 8-bit buffer
    void packPlane(const void* source, ptrdiff_t stride, int width, int height, int bitDepth, std::vector<uint8_t>& out){
        out.resize((size_t)width * (size_t)height);
        if (bitDepth <= 8){
            const uint8_t* src = static_cast<const uint8_t*>(source);
            for (int y = 0; y < height; y++)
                std::memcpy(out.data() + (size_t)y * (size_t)width, src + (ptrdiff_t)y * stride, (size_t)width);
            return;
        }
        // 10/12-bit: rounded to 8 bits (stride is in bytes)
        const int shift = bitDepth - 8;
        const int round = 1 << (shift - 1);
        for (int y = 0; y < height; y++){
            const uint16_t* row = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(source) + (ptrdiff_t)y * stride);
            uint8_t* dst = out.data() + (size_t)y * (size_t)width;
            for (int x = 0; x < width; x++){
                const int value = (row[x] + round) >> shift;
                dst[x] = (uint8_t)(value > 255 ? 255 : value);
            }
        }
    }

}

Av1Decoder::PicturePool::~PicturePool(){
    for (auto& entry : free)
        bufferFree(this, entry.second);
    free.clear();
}

Av1Decoder::Av1Decoder() = default;

Av1Decoder::~Av1Decoder(){
    close();
}

void Av1Decoder::setThreadCountForTesting(int count){
    forcedThreads = std::max(count, 0);
}

void Av1Decoder::setCpuFlagsMaskForTesting(unsigned mask){
    dav1d_set_cpu_flags_mask(mask);
}

unsigned Av1Decoder::getCpuFlags(){
    return dav1d_cpu_flags;
}

size_t Av1Decoder::getLivePictureBytes(){
    return livePictureBytes;
}

int Av1Decoder::getThreadCount() const{
    return threads;
}

void Av1Decoder::close(){
    if (context)
        dav1d_close(&context);
    context = nullptr;
    // every picture is back in the pool once the context is closed
    pool.reset();
}

bool Av1Decoder::open(const VideoTrackInfo& track, std::string& error){
    close();
    pool = std::make_unique<PicturePool>();
    containerColor = track.color;

    const long long pixels = (long long)track.width * (long long)track.height;
    threads = pixels <= 640LL * 360LL ? 1 : (pixels <= 1280LL * 720LL ? 2 : 4);
    if (forcedThreads > 0)
        threads = forcedThreads;
    // dav1d's threads are outside the job system: capped where cores are few (mobile: 2) or
    // the pthread pool is fixed (every web build: 1, the decoder's own thread)
    switch (thread::currentPlatform()){
        case thread::PlatformClass::Android:
        case thread::PlatformClass::IOS:
            threads = std::min(threads, 2);
            break;
        case thread::PlatformClass::WebThreads:
        case thread::PlatformClass::WebSingle:
            threads = 1;
            break;
        case thread::PlatformClass::Desktop:
            break;
    }

    Dav1dSettings settings;
    dav1d_default_settings(&settings);
    settings.n_threads = threads;
    settings.max_frame_delay = 1;
    settings.apply_grain = 1;
    settings.allocator.cookie = pool.get();
    settings.allocator.alloc_picture_callback = allocPicture;
    settings.allocator.release_picture_callback = releasePicture;
    settings.logger.cookie = nullptr;
    settings.logger.callback = logMessage;

    const int result = dav1d_open(&context, &settings);
    if (result < 0 || !context){
        context = nullptr;
        error = "dav1d could not start (error " + std::to_string(result) + ")";
        return false;
    }
    return true;
}

void Av1Decoder::flush(){
    if (context)
        dav1d_flush(context);
}

size_t Av1Decoder::getPictureBytes() const{
    return pool ? pool->bytes.load() : 0;
}

bool Av1Decoder::takePicture(VideoFrame& frame, bool& gotFrame, std::string& error){
    Dav1dPicture picture;
    std::memset(&picture, 0, sizeof(picture));
    const int result = dav1d_get_picture(context, &picture);
    if (result == DAV1D_ERR(EAGAIN))
        return true;
    if (result < 0){
        error = "the AV1 data is damaged (dav1d error " + std::to_string(result) + ")";
        return false;
    }

    frame.width = picture.p.w;
    frame.height = picture.p.h;
    frame.bitDepth = picture.p.bpc;
    frame.ptsNs = picture.m.timestamp;
    frame.durationNs = picture.m.duration;
    const bool hasChroma = picture.p.layout != DAV1D_PIXEL_LAYOUT_I400;
    frame.chromaShiftX = picture.p.layout == DAV1D_PIXEL_LAYOUT_I444 ? 0 : 1;
    frame.chromaShiftY = picture.p.layout == DAV1D_PIXEL_LAYOUT_I420 ? 1 : 0;
    frame.color = containerColor;
    if (picture.seq_hdr){
        const VideoColor::Matrix matrix = matrixOf(picture.seq_hdr->mtrx);
        if (matrix != VideoColor::Matrix::UNSPECIFIED)
            frame.color.matrix = matrix;
        frame.color.fullRange = picture.seq_hdr->color_range != 0;
        if (picture.seq_hdr->pri != DAV1D_COLOR_PRI_UNKNOWN)
            frame.color.primaries = (int)picture.seq_hdr->pri;
        if (picture.seq_hdr->trc != DAV1D_TRC_UNKNOWN)
            frame.color.transfer = (int)picture.seq_hdr->trc;
    }

    packPlane(picture.data[0], picture.stride[0], frame.width, frame.height, frame.bitDepth, frame.y);
    if (hasChroma){
        packPlane(picture.data[1], picture.stride[1], frame.chromaWidth(), frame.chromaHeight(), frame.bitDepth, frame.u);
        packPlane(picture.data[2], picture.stride[1], frame.chromaWidth(), frame.chromaHeight(), frame.bitDepth, frame.v);
    }else{
        frame.u.clear();
        frame.v.clear();
    }
    dav1d_picture_unref(&picture);
    gotFrame = true;
    return true;
}

bool Av1Decoder::decode(const uint8_t* data, size_t size, int64_t ptsNs, VideoFrame& frame, bool& gotFrame, std::string& error){
    gotFrame = false;
    if (!context){
        error = "the AV1 decoder is not open";
        return false;
    }
    Dav1dData input;
    uint8_t* buffer = dav1d_data_create(&input, size);
    if (!buffer){
        error = "out of memory for an AV1 frame of " + std::to_string(size) + " bytes";
        return false;
    }
    std::memcpy(buffer, data, size);
    input.m.timestamp = ptsNs;

    // dav1d may refuse data until a picture is taken out (EAGAIN); with max_frame_delay 1 one
    // frame in gives at most one picture out
    while (input.sz > 0){
        const int result = dav1d_send_data(context, &input);
        if (result < 0 && result != DAV1D_ERR(EAGAIN)){
            dav1d_data_unref(&input);
            error = "the AV1 data is damaged (dav1d error " + std::to_string(result) + ")";
            return false;
        }
        if (result == DAV1D_ERR(EAGAIN) || input.sz > 0){
            if (!takePicture(frame, gotFrame, error)){
                dav1d_data_unref(&input);
                return false;
            }
            if (result == DAV1D_ERR(EAGAIN) && !gotFrame){
                // nothing came out and nothing went in: the decoder is stuck on this data
                dav1d_data_unref(&input);
                error = "the AV1 decoder refused a frame";
                return false;
            }
        }
    }
    if (!gotFrame && !takePicture(frame, gotFrame, error))
        return false;
    return true;
}
