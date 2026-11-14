// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Windows video decoders (docs/superpowers/specs/2026-10-31-video-mp4-design.md §6.6
// "Windows"): Media Foundation's source reader (IMFSourceReader, synchronous ReadSample on the
// source's worker) over a custom IMFByteStream that reads the engine's MediaByteSource, so
// assets in the resource pack and files still downloading play too. H.264 and AAC come with
// Windows; HEVC, VP9 and AV1 with Microsoft's Store extensions. Video processing turns any
// decoder output into NV12 (P010 for 10-bit), copied into the engine's I420 planes; audio is
// float at the stream's rate and channels, resampled to 48 kHz stereo.
//
// Not buildable on the Linux host: written against Microsoft's documented APIs and reviewed line
// by line (MSVC and MinGW-w64).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propidl.h>

#include "media/PlatformVideoBackend.h"

#include "Log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <string>

#if defined(_MSC_VER)
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#endif

using namespace jasperengine;

namespace {

    // subtypes MinGW's headers may lack (FourCC media types: {FOURCC-0000-0010-8000-00AA00389B71})
    constexpr GUID mediaSubtype(DWORD fourcc){
        return GUID{fourcc, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
    }
    constexpr DWORD fourcc(char a, char b, char c, char d){
        return (DWORD)(unsigned char)a | ((DWORD)(unsigned char)b << 8) | ((DWORD)(unsigned char)c << 16) | ((DWORD)(unsigned char)d << 24);
    }
    const GUID SUBTYPE_NV12 = mediaSubtype(fourcc('N', 'V', '1', '2'));
    const GUID SUBTYPE_P010 = mediaSubtype(fourcc('P', '0', '1', '0'));
    const GUID SUBTYPE_H264 = mediaSubtype(fourcc('H', '2', '6', '4'));
    const GUID SUBTYPE_HEVC = mediaSubtype(fourcc('H', 'E', 'V', 'C'));
    const GUID SUBTYPE_VP80 = mediaSubtype(fourcc('V', 'P', '8', '0'));
    const GUID SUBTYPE_VP90 = mediaSubtype(fourcc('V', 'P', '9', '0'));
    const GUID SUBTYPE_AV1 = mediaSubtype(fourcc('A', 'V', '0', '1'));
    const GUID SUBTYPE_MP4V = mediaSubtype(fourcc('M', 'P', '4', 'V'));
    // audio: WAVE_FORMAT tags in the first field
    constexpr GUID audioSubtype(DWORD tag){
        return GUID{tag, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
    }
    const GUID SUBTYPE_FLOAT = audioSubtype(0x0003);        // WAVE_FORMAT_IEEE_FLOAT
    const GUID SUBTYPE_AAC = audioSubtype(0x1610);          // WAVE_FORMAT_MPEG_HEAAC
    const GUID SUBTYPE_MP3 = audioSubtype(0x0055);          // WAVE_FORMAT_MPEGLAYER3
    const GUID SUBTYPE_FLAC = audioSubtype(0xF1AC);
    const GUID SUBTYPE_OPUS = audioSubtype(0x704F);
    const GUID SUBTYPE_ALAC = audioSubtype(0x6C61);
    const GUID SUBTYPE_AC3 = audioSubtype(0x2000);
    const GUID SUBTYPE_EAC3 = {0xa7fb87af, 0x2d02, 0x42fb, {0xa4, 0xd4, 0x05, 0xcd, 0x93, 0x84, 0x3b, 0xdd}};   // MFAudioFormat_Dolby_DDPlus

    template<typename T>
    void release(T*& pointer){
        if (pointer){
            pointer->Release();
            pointer = nullptr;
        }
    }

    // Media Foundation's functions, looked up at run time: Windows N editions have no
    // mfplat.dll / mfreadwrite.dll without the Media Feature Pack, and a load-time import would
    // stop every game (and the editor) from starting there
    struct MfApi {
        bool ok = false;
        decltype(&::MFStartup) Startup = nullptr;
        decltype(&::MFCreateAttributes) CreateAttributes = nullptr;
        decltype(&::MFCreateAsyncResult) CreateAsyncResult = nullptr;
        decltype(&::MFPutWorkItemEx) PutWorkItemEx = nullptr;
        decltype(&::MFCreateMediaType) CreateMediaType = nullptr;
        decltype(&::MFTEnumEx) TEnumEx = nullptr;
        decltype(&::MFCreateSourceReaderFromByteStream) CreateSourceReaderFromByteStream = nullptr;
    };

    template<typename T>
    void lookUp(HMODULE module, const char* name, T& out){
        out = module ? reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(module, name))) : nullptr;
    }

    const MfApi& mf(){
        static MfApi api;
        static std::once_flag once;
        std::call_once(once, []{
            HMODULE plat = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            HMODULE readwrite = LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            lookUp(plat, "MFStartup", api.Startup);
            lookUp(plat, "MFCreateAttributes", api.CreateAttributes);
            lookUp(plat, "MFCreateAsyncResult", api.CreateAsyncResult);
            lookUp(plat, "MFPutWorkItemEx", api.PutWorkItemEx);
            lookUp(plat, "MFCreateMediaType", api.CreateMediaType);
            lookUp(plat, "MFTEnumEx", api.TEnumEx);
            lookUp(readwrite, "MFCreateSourceReaderFromByteStream", api.CreateSourceReaderFromByteStream);
            api.ok = api.Startup && api.CreateAttributes && api.CreateAsyncResult && api.PutWorkItemEx && api.CreateMediaType &&
                     api.TEnumEx && api.CreateSourceReaderFromByteStream;
            if (!api.ok)
                Log::warn("[Video] Media Foundation is not installed (Windows N/KN: the Media Feature Pack): video other than WebM AV1 does not play");
        });
        return api;
    }

    // MFStartup once for the process (never shut down while a backend may live)
    bool startMediaFoundation(){
        static std::once_flag once;
        static bool ok = false;
        std::call_once(once, []{
            if (!mf().ok)
                return;
            ok = SUCCEEDED(mf().Startup(MF_VERSION, MFSTARTUP_LITE));
            if (!ok)
                Log::error("[Video] Media Foundation could not start: video other than WebM AV1 does not play");
        });
        return ok;
    }

    // COM on the calling (worker) thread, for as long as it lives
    void ensureCom(){
        thread_local bool done = false;
        if (!done){
            const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            // RPC_E_CHANGED_MODE: already initialized in another model, which works too
            (void)hr;
            done = true;
        }
    }

    std::string hresultText(HRESULT hr){
        char text[32];
        std::snprintf(text, sizeof(text), "0x%08lX", (unsigned long)hr);
        return text;
    }

    std::string nameOf(const GUID& subtype){
        if (subtype == SUBTYPE_H264) return "avc1";
        if (subtype == SUBTYPE_HEVC) return "hvc1";
        if (subtype == SUBTYPE_VP90) return "V_VP9";
        if (subtype == SUBTYPE_VP80) return "V_VP8";
        if (subtype == SUBTYPE_AV1) return "av01";
        if (subtype == SUBTYPE_AAC) return "mp4a";
        if (subtype == SUBTYPE_MP3) return "mp3 ";
        if (subtype == SUBTYPE_OPUS) return "Opus";
        if (subtype == SUBTYPE_FLAC) return "fLaC";
        return "";
    }

    std::string missingHint(const std::string& codec){
        if (codec == "hevc" || codec == "hvc1") return "install the HEVC Video Extensions (Microsoft Store)";
        if (codec == "vp9" || codec == "V_VP9") return "install the VP9 Video Extensions (Microsoft Store)";
        if (codec == "av1" || codec == "av01") return "install the AV1 Video Extension (Microsoft Store)";
        return "install the Media Feature Pack (Windows N editions)";
    }

    // ---------------------------------------------------------------- IMFByteStream over MediaByteSource

    class ByteStream : public IMFByteStream {
    public:
        explicit ByteStream(MediaByteSource& source): source(source){
            mf().CreateAttributes(&attributes, 1);
        }
        virtual ~ByteStream(){
            release(attributes);
        }

        // the content type the source resolver picks a parser by
        void setContentType(const wchar_t* type){
            if (attributes)
                attributes->SetString(MF_BYTESTREAM_CONTENT_TYPE, type);
        }

        // IUnknown
        STDMETHODIMP QueryInterface(REFIID riid, void** object) override{
            if (!object)
                return E_POINTER;
            if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFByteStream)){
                *object = static_cast<IMFByteStream*>(this);
                AddRef();
                return S_OK;
            }
            if (riid == __uuidof(IMFAttributes) && attributes){
                // the attributes the resolver reads (MF_BYTESTREAM_CONTENT_TYPE)
                return attributes->QueryInterface(riid, object);
            }
            *object = nullptr;
            return E_NOINTERFACE;
        }
        STDMETHODIMP_(ULONG) AddRef() override{ return (ULONG)++references; }
        STDMETHODIMP_(ULONG) Release() override{
            const long count = --references;
            if (count == 0)
                delete this;
            return (ULONG)count;
        }

        // IMFByteStream
        STDMETHODIMP GetCapabilities(DWORD* capabilities) override{
            if (!capabilities)
                return E_POINTER;
            *capabilities = MFBYTESTREAM_IS_READABLE | MFBYTESTREAM_IS_SEEKABLE;
            if (source.isGrowing())
                *capabilities |= MFBYTESTREAM_IS_PARTIALLY_DOWNLOADED;
            return S_OK;
        }
        STDMETHODIMP GetLength(QWORD* length) override{
            if (!length)
                return E_POINTER;
            const int64_t size = source.size();
            *length = size >= 0 ? (QWORD)size : (QWORD)-1;
            return S_OK;
        }
        STDMETHODIMP SetLength(QWORD) override{ return E_NOTIMPL; }
        STDMETHODIMP GetCurrentPosition(QWORD* position) override{
            if (!position)
                return E_POINTER;
            *position = (QWORD)this->position;
            return S_OK;
        }
        STDMETHODIMP SetCurrentPosition(QWORD value) override{
            position = (int64_t)value;
            return S_OK;
        }
        STDMETHODIMP IsEndOfStream(BOOL* end) override{
            if (!end)
                return E_POINTER;
            const int64_t size = source.size();
            *end = size >= 0 && position >= size;
            return S_OK;
        }
        STDMETHODIMP Read(BYTE* buffer, ULONG wanted, ULONG* read) override{
            if (!buffer || !read)
                return E_POINTER;
            const int64_t got = source.readAt(position, buffer, wanted);
            if (got < 0){
                *read = 0;
                failed = true;
                return E_FAIL;
            }
            position += got;
            *read = (ULONG)got;
            return S_OK;
        }
        // asynchronous reads: done at once on the calling thread, then the callback through a work item
        STDMETHODIMP BeginRead(BYTE* buffer, ULONG wanted, IMFAsyncCallback* callback, IUnknown* state) override{
            if (!callback || !buffer)
                return E_POINTER;
            ULONG read = 0;
            const HRESULT readResult = Read(buffer, wanted, &read);
            IMFAsyncResult* result = nullptr;
            HRESULT hr = mf().CreateAsyncResult(nullptr, callback, state, &result);
            if (FAILED(hr))
                return hr;
            lastRead = read;
            result->SetStatus(readResult);
            hr = mf().PutWorkItemEx(MFASYNC_CALLBACK_QUEUE_STANDARD, result);
            result->Release();
            return hr;
        }
        STDMETHODIMP EndRead(IMFAsyncResult* result, ULONG* read) override{
            if (!result || !read)
                return E_POINTER;
            *read = lastRead;
            return result->GetStatus();
        }
        STDMETHODIMP Write(const BYTE*, ULONG, ULONG*) override{ return E_NOTIMPL; }
        STDMETHODIMP BeginWrite(const BYTE*, ULONG, IMFAsyncCallback*, IUnknown*) override{ return E_NOTIMPL; }
        STDMETHODIMP EndWrite(IMFAsyncResult*, ULONG*) override{ return E_NOTIMPL; }
        STDMETHODIMP Seek(MFBYTESTREAM_SEEK_ORIGIN origin, LONGLONG offset, DWORD, QWORD* current) override{
            int64_t target = origin == msoBegin ? offset : position + offset;
            if (target < 0)
                return E_INVALIDARG;
            position = target;
            if (current)
                *current = (QWORD)position;
            return S_OK;
        }
        STDMETHODIMP Flush() override{ return S_OK; }
        STDMETHODIMP Close() override{ return S_OK; }

        bool readFailed() const{ return failed; }

    private:
        MediaByteSource& source;
        IMFAttributes* attributes = nullptr;
        std::atomic<long> references{1};
        int64_t position = 0;
        ULONG lastRead = 0;
        bool failed = false;
    };

    // ---------------------------------------------------------------- the backend

    class Backend : public PlatformDecoderBackend {
    public:
        ~Backend() override{ close(); }

        bool open(MediaByteSource& bytes, Stream which, VideoInfo& info, MediaError& error) override{
            stream = which;
            ensureCom();
            if (!startMediaFoundation())
                return fail(error, MediaErrorCode::SRC_NOT_SUPPORTED, "Media Foundation could not start (" + missingHint("") + ")");
            byteStream = new ByteStream(bytes);
            const std::string& path = bytes.path();
            const bool webm = path.size() > 5 && (path.compare(path.size() - 5, 5, ".webm") == 0 || path.compare(path.size() - 4, 4, ".mkv") == 0);
            byteStream->setContentType(webm ? L"video/webm" : L"video/mp4");

            IMFAttributes* attributes = nullptr;
            mf().CreateAttributes(&attributes, 3);
            if (attributes){
                // any decoder output turned into NV12 (a format the engine reads), in hardware where it can
                attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
                attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
            }
            HRESULT hr = mf().CreateSourceReaderFromByteStream(byteStream, attributes, &reader);
            release(attributes);
            if (FAILED(hr))
                return fail(error, byteStream->readFailed() ? MediaErrorCode::NETWORK : MediaErrorCode::SRC_NOT_SUPPORTED,
                            "Media Foundation cannot read it (" + hresultText(hr) + ")");

            const DWORD streamId = which == Stream::VIDEO ? (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM : (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM;
            reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
            hr = reader->SetStreamSelection(streamId, TRUE);
            if (FAILED(hr))
                return fail(error, MediaErrorCode::SRC_NOT_SUPPORTED, which == Stream::VIDEO ? "no video track" : "no audio track");
            this->streamId = streamId;

            // what the file has
            IMFMediaType* native = nullptr;
            if (SUCCEEDED(reader->GetNativeMediaType(streamId, 0, &native))){
                GUID subtype{};
                native->GetGUID(MF_MT_SUBTYPE, &subtype);
                codec = nameOf(subtype);
                release(native);
            }

            // what the engine wants out: NV12, else P010 (10-bit HEVC/VP9 decoders that give no
            // 8-bit output; the planes are shifted down to 8 bits on the worker)
            const GUID videoOut[] = {SUBTYPE_NV12, SUBTYPE_P010};
            const int choices = which == Stream::VIDEO ? 2 : 1;
            for (int choice = 0; choice < choices; choice++){
                IMFMediaType* wanted = nullptr;
                mf().CreateMediaType(&wanted);
                if (!wanted)
                    return fail(error, MediaErrorCode::DECODE, "Media Foundation could not make a media type");
                if (which == Stream::VIDEO){
                    wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                    wanted->SetGUID(MF_MT_SUBTYPE, videoOut[choice]);
                }else{
                    wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                    wanted->SetGUID(MF_MT_SUBTYPE, SUBTYPE_FLOAT);
                }
                hr = reader->SetCurrentMediaType(streamId, nullptr, wanted);
                release(wanted);
                if (SUCCEEDED(hr))
                    break;
            }
            if (FAILED(hr)){
                // no decoder at all, or one that gives neither layout (the extension is installed then)
                const bool noDecoder = hr == MF_E_TOPO_CODEC_NOT_FOUND || hr == MF_E_UNSUPPORTED_BYTESTREAM_TYPE;
                const bool noLayout = hr == MF_E_INVALIDMEDIATYPE;
                const std::string label = codec.empty() ? std::string("codec") : codec;
                return fail(error, noDecoder || noLayout ? MediaErrorCode::SRC_NOT_SUPPORTED : MediaErrorCode::DECODE,
                            noLayout ? "this Windows' " + label + " decoder gives no picture format the engine reads (NV12 or P010); convert the video to "
                                       "8-bit H.264 or WebM AV1 (" + hresultText(hr) + ")"
                                     : "this Windows has no decoder for its " + label + ": " + missingHint(codec) + " (" + hresultText(hr) + ")");
            }
            readCurrentType();

            PROPVARIANT duration;
            PropVariantInit(&duration);
            if (SUCCEEDED(reader->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &duration)) && duration.vt == VT_UI8)
                info.durationNs = std::max<int64_t>(info.durationNs, (int64_t)duration.uhVal.QuadPart * 100);
            PropVariantClear(&duration);

            if (which == Stream::VIDEO){
                info.width = width;
                info.height = height;
                info.displayWidth = (int)std::lround(displayWidth);
                info.displayHeight = displayHeight;
                info.frameRate = frameRate;
                info.videoCodec = codec;
            }else{
                info.audioChannels = channels;
                info.audioCodec = codec;
            }
            return true;
        }

        bool seek(double seconds) override{
            if (!reader)
                return false;
            PROPVARIANT position;
            PropVariantInit(&position);
            position.vt = VT_I8;
            position.hVal.QuadPart = (LONGLONG)std::llround(std::max(seconds, 0.0) * 1e7);
            const HRESULT hr = reader->SetCurrentPosition(GUID_NULL, position);
            PropVariantClear(&position);
            resampler.flush();
            return SUCCEEDED(hr);
        }

        Step decodeNext(VideoFrame& frame, std::vector<float>& pcm, double& pcmStart, MediaError& error) override{
            ensureCom();
            DWORD actualStream = 0, flags = 0;
            LONGLONG timestamp = 0;
            IMFSample* sample = nullptr;
            const HRESULT hr = reader->ReadSample(streamId, 0, &actualStream, &flags, &timestamp, &sample);
            if (FAILED(hr)){
                release(sample);
                error.code = byteStream && byteStream->readFailed() ? MediaErrorCode::NETWORK : MediaErrorCode::DECODE;
                error.message = "Media Foundation stopped decoding (" + hresultText(hr) + ")";
                return Step::FAILURE;
            }
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
                readCurrentType();
            if (flags & MF_SOURCE_READERF_ERROR){
                release(sample);
                error.code = MediaErrorCode::DECODE;
                error.message = "Media Foundation stopped decoding";
                return Step::FAILURE;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM){
                release(sample);
                return Step::END;
            }
            if (!sample)
                return Step::AGAIN;
            Step step = stream == Stream::VIDEO ? takePicture(sample, timestamp, frame) : takeAudio(sample, timestamp, pcm, pcmStart);
            release(sample);
            return step;
        }

        void close() override{
            release(reader);
            release(byteStream);
        }

    private:
        bool fail(MediaError& error, MediaErrorCode code, const std::string& message){
            error.code = code;
            error.message = message;
            close();
            return false;
        }

        void readCurrentType(){
            IMFMediaType* type = nullptr;
            if (FAILED(reader->GetCurrentMediaType(streamId, &type)))
                return;
            if (stream == Stream::VIDEO){
                UINT32 w = 0, h = 0;
                MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h);
                width = (int)w;
                height = (int)h;
                GUID subtype{};
                type->GetGUID(MF_MT_SUBTYPE, &subtype);
                tenBit = subtype == SUBTYPE_P010;
                // the default stride (bytes per row; negative for bottom-up images, never for NV12)
                UINT32 strideValue = 0;
                stride = SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideValue)) ? (int)(INT32)strideValue : 0;
                // the picture inside a padded frame
                MFVideoArea area{};
                UINT32 blobSize = 0;
                if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8*)&area, sizeof(area), &blobSize))){
                    apertureX = area.OffsetX.value;
                    apertureY = area.OffsetY.value;
                    apertureWidth = (int)area.Area.cx;
                    apertureHeight = (int)area.Area.cy;
                }else{
                    apertureX = apertureY = 0;
                    apertureWidth = width;
                    apertureHeight = height;
                }
                UINT32 parNum = 1, parDen = 1;
                MFGetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, &parNum, &parDen);
                displayWidth = apertureWidth * (parDen ? (double)parNum / parDen : 1.0);
                displayHeight = apertureHeight;
                UINT32 rateNum = 0, rateDen = 0;
                if (SUCCEEDED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &rateNum, &rateDen)) && rateDen)
                    frameRate = (double)rateNum / rateDen;
                UINT32 matrix = 0, range = 0;
                color = VideoColor();
                if (SUCCEEDED(type->GetUINT32(MF_MT_YUV_MATRIX, &matrix))){
                    if (matrix == MFVideoTransferMatrix_BT709) color.matrix = VideoColor::Matrix::BT709;
                    else if (matrix == MFVideoTransferMatrix_BT601) color.matrix = VideoColor::Matrix::BT601;
                    else if (matrix == MFVideoTransferMatrix_BT2020_10 || matrix == MFVideoTransferMatrix_BT2020_12) color.matrix = VideoColor::Matrix::BT2020;
                }
                if (SUCCEEDED(type->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &range)))
                    color.fullRange = range == MFNominalRange_0_255;
            }else{
                channels = (int)MFGetAttributeUINT32(type, MF_MT_AUDIO_NUM_CHANNELS, 2);
                sampleRate = (int)MFGetAttributeUINT32(type, MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
                resampler.reset(sampleRate, channels);
            }
            release(type);
        }

        Step takePicture(IMFSample* sample, LONGLONG timestamp, VideoFrame& frame){
            IMFMediaBuffer* buffer = nullptr;
            if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || !buffer)
                return Step::AGAIN;
            BYTE* data = nullptr;
            LONG pitch = 0;
            DWORD length = 0;
            IMF2DBuffer* buffer2d = nullptr;
            bool locked2d = false;
            if (SUCCEEDED(buffer->QueryInterface(__uuidof(IMF2DBuffer), (void**)&buffer2d)) && SUCCEEDED(buffer2d->Lock2D(&data, &pitch))){
                locked2d = true;
            }else{
                release(buffer2d);
                if (FAILED(buffer->Lock(&data, nullptr, &length))){
                    release(buffer);
                    return Step::AGAIN;
                }
                pitch = stride > 0 ? stride : (LONG)(tenBit ? width * 2 : width);
            }
            Step step = Step::AGAIN;
            if (data && pitch > 0 && apertureWidth > 0 && apertureHeight > 0){
                // the chroma plane follows the luma's padded rows
                const int rows = height;
                const uint8_t* yPlane = data + (size_t)apertureY * pitch + (size_t)apertureX * (tenBit ? 2 : 1);
                const uint8_t* uvPlane = data + (size_t)rows * pitch + (size_t)(apertureY / 2) * pitch + (size_t)(apertureX & ~1) * (tenBit ? 2 : 1);
                if (tenBit)
                    VideoFramePlanes::copyP01x(frame, apertureWidth, apertureHeight, 10, yPlane, (int)pitch, uvPlane, (int)pitch);
                else
                    VideoFramePlanes::copyNV12(frame, apertureWidth, apertureHeight, yPlane, (int)pitch, uvPlane, (int)pitch);
                frame.color = color;
                frame.ptsNs = (int64_t)timestamp * 100;
                step = Step::FRAME;
            }
            if (locked2d){
                buffer2d->Unlock2D();
                release(buffer2d);
            }else{
                buffer->Unlock();
            }
            release(buffer);
            return step;
        }

        Step takeAudio(IMFSample* sample, LONGLONG timestamp, std::vector<float>& pcm, double& pcmStart){
            IMFMediaBuffer* buffer = nullptr;
            if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || !buffer)
                return Step::AGAIN;
            BYTE* data = nullptr;
            DWORD length = 0;
            if (FAILED(buffer->Lock(&data, nullptr, &length))){
                release(buffer);
                return Step::AGAIN;
            }
            pcm.clear();
            const size_t frames = length / (sizeof(float) * (size_t)std::max(channels, 1));
            resampler.process(reinterpret_cast<const float*>(data), frames, pcm);
            buffer->Unlock();
            release(buffer);
            pcmStart = (double)timestamp / 1e7;
            return pcm.empty() ? Step::AGAIN : Step::AUDIO;
        }

        Stream stream = Stream::VIDEO;
        ByteStream* byteStream = nullptr;
        IMFSourceReader* reader = nullptr;
        DWORD streamId = 0;
        std::string codec;
        // video
        int width = 0, height = 0, stride = 0;
        int apertureX = 0, apertureY = 0, apertureWidth = 0, apertureHeight = 0;
        double displayWidth = 0.0;
        int displayHeight = 0;
        double frameRate = 0.0;
        bool tenBit = false;
        VideoColor color;
        // audio
        int channels = 2;
        int sampleRate = 48000;
        AudioResampler48k resampler;
    };

    std::mutex supportMutex;
    std::map<std::string, platformvideo::Support> supportCache;

}

const char* platformvideo::backendName(){
    return "mediafoundation";
}

bool platformvideo::isAvailable(std::string* reason){
    const bool ok = startMediaFoundation();
    if (!ok && reason)
        *reason = "Media Foundation could not start: " + missingHint("");
    return ok;
}

std::unique_ptr<PlatformDecoderBackend> platformvideo::createBackend(){
    return std::make_unique<Backend>();
}

platformvideo::Support platformvideo::supports(const std::string& codec){
    static const std::map<std::string, std::pair<GUID, bool>> types = {
        {"h264", {SUBTYPE_H264, true}}, {"hevc", {SUBTYPE_HEVC, true}}, {"vp8", {SUBTYPE_VP80, true}}, {"vp9", {SUBTYPE_VP90, true}},
        {"av1", {SUBTYPE_AV1, true}}, {"mpeg4", {SUBTYPE_MP4V, true}}, {"aac", {SUBTYPE_AAC, false}}, {"mp3", {SUBTYPE_MP3, false}},
        {"flac", {SUBTYPE_FLAC, false}}, {"opus", {SUBTYPE_OPUS, false}}, {"alac", {SUBTYPE_ALAC, false}}, {"ac3", {SUBTYPE_AC3, false}},
        {"eac3", {SUBTYPE_EAC3, false}},
    };
    auto type = types.find(codec);
    if (type == types.end() || !startMediaFoundation())
        return Support::No;
    std::lock_guard<std::mutex> lock(supportMutex);
    auto cached = supportCache.find(codec);
    if (cached != supportCache.end())
        return cached->second;
    ensureCom();
    MFT_REGISTER_TYPE_INFO input = {type->second.second ? MFMediaType_Video : MFMediaType_Audio, type->second.first};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    const HRESULT hr = mf().TEnumEx(type->second.second ? MFT_CATEGORY_VIDEO_DECODER : MFT_CATEGORY_AUDIO_DECODER,
                                 MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                                 &input, nullptr, &activates, &count);
    for (UINT32 i = 0; i < count; i++)
        activates[i]->Release();
    if (activates)
        CoTaskMemFree(activates);
    const Support support = SUCCEEDED(hr) && count > 0 ? Support::Yes : Support::No;
    supportCache[codec] = support;
    return support;
}

bool platformvideo::readsContainer(const std::string& container){
    // WebM through Windows' own WebM source (Windows 10 1809+; VP9 with its extension)
    return container == "mp4" || container == "mov" || container == "3gp" || container == "webm" || container == "matroska";
}

bool platformvideo::supportsGrowingSources(){
    return true;
}

std::string platformvideo::unsupportedHint(const std::string& codec){
    return "this Windows has no " + codec + " decoder: " + missingHint(codec) + ", or convert the video to WebM AV1";
}
