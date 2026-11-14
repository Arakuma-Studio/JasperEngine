// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The iOS and macOS video decoders (docs/superpowers/specs/2026-10-31-video-mp4-design.md §6.6
// "iOS/macOS"): AVFoundation's AVAssetReader over an AVURLAsset (pull model: one sample buffer
// per copyNextSampleBuffer), H.264 and HEVC in hardware, AAC. The video comes as NV12
// CVPixelBuffers (8 or 10 bits, video or full range from the track's format description),
// copied into the engine's I420 planes on the source's worker; the audio as 48 kHz interleaved
// stereo float. A seek makes a new reader whose time range starts at the target (AVFoundation
// decodes from the keyframe before and returns from the target). An asset in the resource pack
// (no file of its own) is written once to the caches folder. Objective-C++ with ARC.
//
// Not buildable on the Linux host: written against Apple's documented APIs and reviewed line by
// line; the macOS/iOS fixture runs on Apple CI.

#include "media/PlatformVideoBackend.h"

#include "Log.h"
#include "htmlui/compat/Hash.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;

// The synchronous asset API (loadValuesAsynchronouslyForKeys, tracksWithMediaType,
// CVBufferGetAttachment) is what a worker thread that waits wants; its async replacements
// (macOS 13, iOS 16) need Swift concurrency or nested callbacks. Still supported, so the
// deprecations are not warnings here.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

// the Objective-C objects here (assets, readers, outputs, NSError) are ARC's: CMSampleBuffers from
// copyNextSampleBuffer (Core Foundation) are released by hand
#if !__has_feature(objc_arc)
#error "AVFoundationBackend.mm is compiled with ARC (-fobjc-arc)"
#endif

namespace {

    std::string fourccOf(FourCharCode code){
        char text[5] = {(char)((code >> 24) & 0xFF), (char)((code >> 16) & 0xFF), (char)((code >> 8) & 0xFF), (char)(code & 0xFF), 0};
        return text;
    }

    std::string utf8(NSString* text){
        return text ? std::string([text UTF8String]) : std::string();
    }

    std::string lower(std::string text){
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return text;
    }

    // the extension of a path when it names a container AVFoundation reads, else ""
    std::string knownExtension(const std::string& path){
        const size_t slash = path.find_last_of('/');
        const size_t dot = path.find_last_of('.');
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
            return "";
        const std::string extension = lower(path.substr(dot + 1));
        static const char* const known[] = {"mp4", "m4v", "mov", "3gp", "m4a"};
        for (const char* name : known){
            if (extension == name)
                return extension;
        }
        return "";
    }

    // the container by its first box (ftyp brand "qt  ": QuickTime), for a file whose name says
    // nothing (a media cache file of an extensionless URL is .bin): AVFoundation goes by the
    // extension
    std::string sniffedExtension(MediaByteSource& source){
        uint8_t head[12] = {};
        if (source.readAt(0, head, sizeof(head)) >= 12 && std::memcmp(head + 4, "ftyp", 4) == 0)
            return std::memcmp(head + 8, "qt  ", 4) == 0 ? "mov" : "mp4";
        return "mp4";
    }

    NSURL* cacheFolder(){
        NSArray<NSURL*>* caches = [[NSFileManager defaultManager] URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask];
        NSURL* folder = caches.count ? [caches.firstObject URLByAppendingPathComponent:@"jasperengine-video" isDirectory:YES]
                                     : [NSURL fileURLWithPath:NSTemporaryDirectory() isDirectory:YES];
        [[NSFileManager defaultManager] createDirectoryAtURL:folder withIntermediateDirectories:YES attributes:nil error:nil];
        return folder;
    }

    // a file AVFoundation can open: the source's own (through a link named by its container when
    // its own name has no known extension), else its bytes written once to the caches folder
    NSURL* fileUrlOf(MediaByteSource& source, MediaError& error){
        const std::string own = source.systemPath();
        if (!own.empty()){
            NSURL* file = [NSURL fileURLWithPath:[NSString stringWithUTF8String:own.c_str()]];
            if (!knownExtension(own).empty())
                return file;
            const std::string name = htmlui::compat::sha256Hex(own).substr(0, 24) + "." + sniffedExtension(source);
            NSURL* link = [cacheFolder() URLByAppendingPathComponent:[NSString stringWithUTF8String:name.c_str()]];
            [[NSFileManager defaultManager] removeItemAtURL:link error:nil];
            if ([[NSFileManager defaultManager] createSymbolicLinkAtURL:link withDestinationURL:file error:nil])
                return link;
            // no link possible: the copy below
        }
        const int64_t size = source.size();
        if (size <= 0){
            error.code = MediaErrorCode::NETWORK;
            error.message = "'" + source.path() + "' could not be read";
            return nil;
        }
        NSURL* folder = cacheFolder();
        // the extension tells AVFoundation the container
        const std::string& path = source.path();
        std::string extension = knownExtension(path);
        if (extension.empty())
            extension = sniffedExtension(source);
        const std::string name = htmlui::compat::sha256Hex(path + ":" + std::to_string(size)).substr(0, 24) + "." + extension;
        NSURL* file = [folder URLByAppendingPathComponent:[NSString stringWithUTF8String:name.c_str()]];
        NSNumber* existing = nil;
        [file getResourceValue:&existing forKey:NSURLFileSizeKey error:nil];
        if (existing && existing.longLongValue == size)
            return file;
        // copied in 1 MiB parts into a temporary file renamed into place (never the whole video in memory)
        const std::string target = utf8(file.path);
        const std::string temp = target + ".tmp";
        FILE* out = std::fopen(temp.c_str(), "wb");
        if (!out){
            error.code = MediaErrorCode::NETWORK;
            error.message = "the video could not be written for the decoder";
            return nil;
        }
        std::vector<uint8_t> buffer((size_t)1 << 20);
        int64_t done = 0;
        bool ok = true;
        while (ok && done < size){
            const int64_t got = source.readAt(done, buffer.data(), (size_t)std::min<int64_t>((int64_t)buffer.size(), size - done));
            ok = got > 0 && std::fwrite(buffer.data(), 1, (size_t)got, out) == (size_t)got;
            done += std::max<int64_t>(got, 0);
        }
        ok = std::fclose(out) == 0 && ok;
        if (!ok || std::rename(temp.c_str(), target.c_str()) != 0){
            std::remove(temp.c_str());
            error.code = MediaErrorCode::NETWORK;
            error.message = "'" + source.path() + "' could not be copied for the decoder";
            return nil;
        }
        return file;
    }

    class Backend : public PlatformDecoderBackend {
    public:
        ~Backend() override{ close(); }

        bool open(MediaByteSource& bytes, Stream which, VideoInfo& info, MediaError& error) override{
            stream = which;
            if (bytes.isGrowing()){
                error.code = MediaErrorCode::SRC_NOT_SUPPORTED;
                error.message = "Apple platforms play a downloaded video once it is complete";
                return false;
            }
            @autoreleasepool {
                NSURL* url = fileUrlOf(bytes, error);
                if (!url)
                    return false;
                asset = [AVURLAsset URLAssetWithURL:url options:@{AVURLAssetPreferPreciseDurationAndTimingKey: @YES}];
                // the tracks and duration, waited for here (the worker thread)
                dispatch_semaphore_t loaded = dispatch_semaphore_create(0);
                [asset loadValuesAsynchronouslyForKeys:@[@"tracks", @"duration"] completionHandler:^{
                    dispatch_semaphore_signal(loaded);
                }];
                dispatch_semaphore_wait(loaded, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(20 * NSEC_PER_SEC)));
                NSError* loadError = nil;
                if ([asset statusOfValueForKey:@"tracks" error:&loadError] != AVKeyValueStatusLoaded){
                    error.code = MediaErrorCode::SRC_NOT_SUPPORTED;
                    error.message = "AVFoundation cannot read it" + (loadError ? " (" + utf8(loadError.localizedDescription) + ")" : std::string());
                    return false;
                }
                NSArray<AVAssetTrack*>* tracks = [asset tracksWithMediaType:which == Stream::VIDEO ? AVMediaTypeVideo : AVMediaTypeAudio];
                if (tracks.count == 0){
                    error.code = MediaErrorCode::SRC_NOT_SUPPORTED;
                    error.message = which == Stream::VIDEO ? "no video track" : "no audio track";
                    return false;
                }
                track = tracks.firstObject;
                const CMTime duration = asset.duration;
                if (CMTIME_IS_NUMERIC(duration))
                    info.durationNs = std::max<int64_t>(info.durationNs, (int64_t)std::llround(CMTimeGetSeconds(duration) * 1e9));

                FourCharCode subtype = 0;
                bool fullRange = false;
                int bits = 8;
                if (track.formatDescriptions.count > 0){
                    CMFormatDescriptionRef description = (__bridge CMFormatDescriptionRef)track.formatDescriptions.firstObject;
                    subtype = CMFormatDescriptionGetMediaSubType(description);
                    if (which == Stream::VIDEO){
                        CFTypeRef range = CMFormatDescriptionGetExtension(description, kCMFormatDescriptionExtension_FullRangeVideo);
                        fullRange = range && CFGetTypeID(range) == CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)range);
                        CFTypeRef depth = CMFormatDescriptionGetExtension(description, kCMFormatDescriptionExtension_Depth);
                        if (depth && CFGetTypeID(depth) == CFNumberGetTypeID()){
                            int value = 0;
                            CFNumberGetValue((CFNumberRef)depth, kCFNumberIntType, &value);
                            // the extension is the pixel depth (24 for 8-bit 4:2:0); 10-bit HEVC reports 30
                            bits = value >= 30 ? 10 : 8;
                        }
                    }
                }
                if (which == Stream::VIDEO){
                    const CGSize natural = track.naturalSize;
                    info.width = (int)std::lround(natural.width);
                    info.height = (int)std::lround(natural.height);
                    info.displayWidth = info.width;
                    info.displayHeight = info.height;
                    if (track.nominalFrameRate > 0.0f)
                        info.frameRate = track.nominalFrameRate;
                    info.videoCodec = fourccOf(subtype);
                    pixelFormat = bits > 8 ? (fullRange ? kCVPixelFormatType_420YpCbCr10BiPlanarFullRange : kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange)
                                           : (fullRange ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
                }else{
                    info.audioCodec = fourccOf(subtype);
                    info.audioChannels = 2;
                    if (track.formatDescriptions.count > 0){
                        CMAudioFormatDescriptionRef description = (__bridge CMAudioFormatDescriptionRef)track.formatDescriptions.firstObject;
                        if (const AudioStreamBasicDescription* basic = CMAudioFormatDescriptionGetStreamBasicDescription(description))
                            info.audioChannels = (int)basic->mChannelsPerFrame;
                    }
                }
                codecName = which == Stream::VIDEO ? info.videoCodec : info.audioCodec;
                return startReader(kCMTimeZero, error);
            }
        }

        bool seek(double seconds) override{
            MediaError error;
            lastPts = kCMTimeInvalid;
            interrupted = false;
            return startReader(CMTimeMakeWithSeconds(std::max(seconds, 0.0), 600000), error);
        }

        Step decodeNext(VideoFrame& frame, std::vector<float>& pcm, double& pcmStart, MediaError& error) override{
            @autoreleasepool {
                if (interrupted){
                    // a new reader after the last sample given out; while the app stays in the
                    // background it fails the same way: a try every 100 ms (the decoding thread)
                    if (restarts++ > 0)
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    const CMTime resume = CMTIME_IS_NUMERIC(lastPts) ? CMTimeAdd(lastPts, CMTimeMake(1, 600000)) : kCMTimeZero;
                    MediaError restartError;
                    if (!startReader(resume, restartError)){
                        if (isInterruption(lastStartError))
                            return Step::AGAIN;
                        interrupted = false;
                        error = restartError;
                        return Step::FAILURE;
                    }
                    interrupted = false;
                }
                if (!reader || !output)
                    return Step::END;
                CMSampleBufferRef sample = [output copyNextSampleBuffer];
                if (!sample){
                    if (reader.status == AVAssetReaderStatusFailed){
                        NSError* failure = reader.error;
                        // iOS stops readers when the app goes to the background (and on a media
                        // services reset): read on from just after the last sample, in sync
                        if (isInterruption(failure)){
                            interrupted = true;
                            return Step::AGAIN;
                        }
                        error.code = isCodecFailure(failure) ? MediaErrorCode::SRC_NOT_SUPPORTED : MediaErrorCode::DECODE;
                        error.message = isCodecFailure(failure) ? "this device has no " + codecLabel(codecName) + " decoder (" + utf8(failure.localizedDescription) + ")"
                                                                : "AVFoundation stopped decoding (" + utf8(failure.localizedDescription) + ")";
                        return Step::FAILURE;
                    }
                    return Step::END;
                }
                const CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample);
                if (CMTIME_IS_NUMERIC(pts))
                    lastPts = pts;
                Step step = Step::AGAIN;
                if (stream == Stream::VIDEO)
                    step = takePicture(sample, frame);
                else
                    step = takeAudio(sample, pcm, pcmStart);
                CFRelease(sample);
                if (step != Step::AGAIN)
                    restarts = 0;
                return step;
            }
        }

        void close() override{
            @autoreleasepool {
                if (reader && reader.status == AVAssetReaderStatusReading)
                    [reader cancelReading];
                reader = nil;
                output = nil;
                track = nil;
                asset = nil;
            }
        }

    private:
        bool startReader(CMTime start, MediaError& error){
            @autoreleasepool {
                if (reader && reader.status == AVAssetReaderStatusReading)
                    [reader cancelReading];
                reader = nil;
                output = nil;
                NSError* readerError = nil;
                AVAssetReader* made = [[AVAssetReader alloc] initWithAsset:asset error:&readerError];
                if (!made){
                    error.code = MediaErrorCode::DECODE;
                    error.message = "AVFoundation could not read it (" + utf8(readerError.localizedDescription) + ")";
                    return false;
                }
                NSDictionary* settings = nil;
                if (stream == Stream::VIDEO){
                    settings = @{(id)kCVPixelBufferPixelFormatTypeKey: @(pixelFormat)};
                }else{
                    settings = @{AVFormatIDKey: @(kAudioFormatLinearPCM), AVLinearPCMIsFloatKey: @YES, AVLinearPCMBitDepthKey: @32,
                                 AVLinearPCMIsNonInterleaved: @NO, AVLinearPCMIsBigEndianKey: @NO, AVSampleRateKey: @48000,
                                 AVNumberOfChannelsKey: @2};
                }
                AVAssetReaderTrackOutput* trackOutput = nil;
                @try {
                    trackOutput = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track outputSettings:settings];
                } @catch (NSException* exception) {
                    error.code = MediaErrorCode::SRC_NOT_SUPPORTED;
                    error.message = "this device has no " + codecLabel(codecName) + " decoder (" + utf8(exception.reason) + ")";
                    return false;
                }
                trackOutput.alwaysCopiesSampleData = NO;
                if (![made canAddOutput:trackOutput]){
                    error.code = MediaErrorCode::SRC_NOT_SUPPORTED;
                    error.message = "this device has no " + codecLabel(codecName) + " decoder";
                    return false;
                }
                [made addOutput:trackOutput];
                if (CMTimeCompare(start, kCMTimeZero) > 0)
                    made.timeRange = CMTimeRangeMake(start, kCMTimePositiveInfinity);
                lastStartError = nil;
                if (![made startReading]){
                    lastStartError = made.error;
                    error.code = isCodecFailure(made.error) || !made.error ? MediaErrorCode::SRC_NOT_SUPPORTED : MediaErrorCode::DECODE;
                    error.message = isCodecFailure(made.error) ? "this device has no " + codecLabel(codecName) + " decoder (" + utf8(made.error.localizedDescription) + ")"
                                                               : "AVFoundation cannot decode its " + codecLabel(codecName) + " (" + utf8(made.error.localizedDescription) + ")";
                    return false;
                }
                reader = made;
                output = trackOutput;
                return true;
            }
        }

        // AVErrorOperationInterrupted (-11847: the app left the foreground) and
        // AVErrorMediaServicesWereReset (-11819), by value: the latter is iOS-only API
        static bool isInterruption(NSError* failure){
            return failure && [failure.domain isEqualToString:AVFoundationErrorDomain] && (failure.code == -11847 || failure.code == -11819);
        }

        // AVErrorDecoderNotFound (-11833), AVErrorFileFormatNotRecognized (-11828), AVErrorDecodeFailed
        // (-11821): no decoder for it here (the codec hint applies)
        static bool isCodecFailure(NSError* failure){
            return failure && [failure.domain isEqualToString:AVFoundationErrorDomain] &&
                   (failure.code == -11833 || failure.code == -11828 || failure.code == -11821);
        }

        static std::string codecLabel(const std::string& code){
            if (code == "avc1" || code == "avc3") return "H.264";
            if (code == "hvc1" || code == "hev1") return "HEVC";
            if (code == "mp4a") return "AAC";
            return code.empty() ? std::string("video") : code;
        }

        Step takePicture(CMSampleBufferRef sample, VideoFrame& frame){
            CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample);
            if (!image)
                return Step::AGAIN;   // a sample without a picture (an empty edit)
            CVPixelBufferRef pixels = (CVPixelBufferRef)image;
            if (CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
                return Step::AGAIN;
            const int width = (int)CVPixelBufferGetWidth(pixels);
            const int height = (int)CVPixelBufferGetHeight(pixels);
            const OSType format = CVPixelBufferGetPixelFormatType(pixels);
            const uint8_t* y = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pixels, 0);
            const uint8_t* uv = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pixels, 1);
            const int yStride = (int)CVPixelBufferGetBytesPerRowOfPlane(pixels, 0);
            const int uvStride = (int)CVPixelBufferGetBytesPerRowOfPlane(pixels, 1);
            const bool tenBit = format == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange || format == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
            if (y && uv && width > 0 && height > 0){
                if (tenBit)
                    VideoFramePlanes::copyP01x(frame, width, height, 10, y, yStride, uv, uvStride);
                else
                    VideoFramePlanes::copyNV12(frame, width, height, y, yStride, uv, uvStride);
            }
            CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
            if (frame.y.empty())
                return Step::AGAIN;
            VideoColor color;
            color.fullRange = format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange || format == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
            // (the Get rule: not released)
            CFTypeRef matrix = CVBufferGetAttachment(image, kCVImageBufferYCbCrMatrixKey, nullptr);
            if (matrix){
                if (CFEqual(matrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2))
                    color.matrix = VideoColor::Matrix::BT709;
                else if (CFEqual(matrix, kCVImageBufferYCbCrMatrix_ITU_R_601_4))
                    color.matrix = VideoColor::Matrix::BT601;
                else if (CFEqual(matrix, kCVImageBufferYCbCrMatrix_ITU_R_2020))
                    color.matrix = VideoColor::Matrix::BT2020;
            }
            frame.color = color;
            const CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample);
            frame.ptsNs = CMTIME_IS_NUMERIC(pts) ? (int64_t)std::llround(CMTimeGetSeconds(pts) * 1e9) : 0;
            return Step::FRAME;
        }

        Step takeAudio(CMSampleBufferRef sample, std::vector<float>& pcm, double& pcmStart){
            CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
            if (!block)
                return Step::AGAIN;
            const size_t length = CMBlockBufferGetDataLength(block);
            pcm.resize(length / sizeof(float));
            if (CMBlockBufferCopyDataBytes(block, 0, length, pcm.data()) != kCMBlockBufferNoErr){
                pcm.clear();
                return Step::AGAIN;
            }
            // whole stereo frames
            pcm.resize(pcm.size() - pcm.size() % 2);
            const CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample);
            pcmStart = CMTIME_IS_NUMERIC(pts) ? CMTimeGetSeconds(pts) : 0.0;
            return pcm.empty() ? Step::AGAIN : Step::AUDIO;
        }

        Stream stream = Stream::VIDEO;
        AVURLAsset* asset = nil;
        AVAssetTrack* track = nil;
        AVAssetReader* reader = nil;
        AVAssetReaderTrackOutput* output = nil;
        OSType pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        std::string codecName;
        CMTime lastPts = kCMTimeInvalid;   // the last sample given out (an interrupted reader resumes after it)
        bool interrupted = false;          // the reader stopped by an interruption: a new one next
        int restarts = 0;                  // readers restarted since the last sample
        NSError* lastStartError = nil;     // why the last startReading failed
    };

}

const char* platformvideo::backendName(){
    return "avfoundation";
}

bool platformvideo::isAvailable(std::string*){
    return true;
}

std::unique_ptr<PlatformDecoderBackend> platformvideo::createBackend(){
    return std::make_unique<Backend>();
}

platformvideo::Support platformvideo::supports(const std::string& codec){
    // what AVFoundation decodes on every supported OS (macOS 12+, iOS 15+); AV1 only with the
    // hardware of newer devices; WebM's codecs not at all (AVFoundation reads no WebM)
    static const char* const always[] = {"h264", "hevc", "aac", "mp3", "alac", "flac", "ac3", "eac3", "mpeg4"};
    for (const char* name : always){
        if (codec == name)
            return Support::Yes;
    }
    if (codec == "av1"){
        if (@available(macOS 14.0, iOS 17.0, *))
            return Support::Maybe;
        return Support::No;
    }
    return Support::No;
}

bool platformvideo::readsContainer(const std::string& container){
    return container == "mp4" || container == "mov" || container == "3gp";
}

bool platformvideo::supportsGrowingSources(){
    return false;
}

std::string platformvideo::unsupportedHint(const std::string& codec){
    if (codec == "vp8" || codec == "vp9" || codec == "opus" || codec == "vorbis")
        return "Apple platforms play no " + codec + " (AVFoundation reads no WebM); convert the video to WebM AV1 (software, everywhere) or H.264 MP4";
    return "this device has no " + codec + " decoder; convert the video to WebM AV1 or H.264 MP4";
}
