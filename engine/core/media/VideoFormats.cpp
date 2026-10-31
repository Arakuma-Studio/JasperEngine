// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VideoFormats.h"

#include "MediaByteSource.h"
#include "Mp4Probe.h"
#include "VideoDemuxer.h"
#include "VideoSourceFactory.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace jasperengine;

namespace {

    std::string lower(std::string text){
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return text;
    }

    std::string trim(const std::string& text){
        const size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return std::string();
        const size_t last = text.find_last_not_of(" \t\r\n");
        return text.substr(first, last - first + 1);
    }

    // The extension of a path's last segment (before a query or fragment) in lower case, in a buffer: every URL of a
    // CSS image asks (Rewriters' resolveUrlIn), so no allocation. Empty when there is none or it is longer than any
    // video extension.
    struct Extension {
        char text[8] = {};
        size_t length = 0;
        bool is(const char* other) const{ return std::strlen(other) == length && std::memcmp(text, other, length) == 0; }
    };

    Extension extensionOf(std::string_view path){
        Extension extension;
        const size_t cut = path.find_first_of("?#");
        if (cut != std::string_view::npos)
            path = path.substr(0, cut);
        const size_t slash = path.find_last_of("/\\");
        const size_t dot = path.find_last_of('.');
        if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash))
            return extension;
        const std::string_view text = path.substr(dot + 1);
        if (text.size() >= sizeof(extension.text))
            return extension;
        for (size_t i = 0; i < text.size(); i++)
            extension.text[i] = (char)std::tolower((unsigned char)text[i]);
        extension.length = text.size();
        return extension;
    }

    // RFC 6381 codec strings of the engine's two codecs
    bool playableCodec(const std::string& codec){
        if (codec == "opus")
            return true;
        if (codec == "av01" || codec.rfind("av01.", 0) == 0)
            return true;
        return false;
    }

    const char* const rejectedExtensions[] = {"qt", "ogv", "ogg", "ogm", "avi", "wmv", "asf", "flv", "f4v",
                                              "mpg", "mpeg", "mpe", "m2v", "m1v", "ts", "m2ts", "mts", "3gp", "3g2", "vob", "divx",
                                              "rm", "rmvb", "y4m", "gifv", "m4a", "mp3", "aac", "wav", "flac"};

}

namespace {

    VideoFormats::CanPlay fromSupport(VideoSourceFactory::Support support){
        switch (support){
            case VideoSourceFactory::Support::YES: return VideoFormats::CanPlay::PROBABLY;
            case VideoSourceFactory::Support::MAYBE: return VideoFormats::CanPlay::MAYBE;
            case VideoSourceFactory::Support::NO: break;
        }
        return VideoFormats::CanPlay::NO;
    }

}

VideoFormats::CanPlay VideoFormats::canPlayType(const std::string& mimeType){
    // type/subtype[; codecs="a, b"] (RFC 6381), other parameters ignored
    const std::string text = trim(mimeType);
    const size_t semicolon = text.find(';');
    const std::string type = lower(trim(text.substr(0, semicolon)));
    const bool webm = type == "video/webm" || type == "audio/webm";
    const bool mp4 = type == "video/mp4" || type == "audio/mp4" || type == "video/quicktime" || type == "video/x-m4v";
    if (!webm && !mp4)
        return CanPlay::NO;
    // without codecs: whether the container is read at all (WebM always: AV1/Opus in software)
    const CanPlay container = webm ? CanPlay::MAYBE : (VideoSourceFactory::readsContainer("mp4") ? CanPlay::MAYBE : CanPlay::NO);
    if (semicolon == std::string::npos)
        return container;

    std::string codecs;
    bool hasCodecs = false;
    size_t position = semicolon + 1;
    while (position <= text.size()){
        size_t end = text.find(';', position);
        if (end == std::string::npos)
            end = text.size();
        const std::string parameter = trim(text.substr(position, end - position));
        const size_t equals = parameter.find('=');
        if (equals != std::string::npos && lower(trim(parameter.substr(0, equals))) == "codecs"){
            codecs = trim(parameter.substr(equals + 1));
            if (codecs.size() >= 2 && (codecs.front() == '"' || codecs.front() == '\'') && codecs.back() == codecs.front())
                codecs = codecs.substr(1, codecs.size() - 2);
            hasCodecs = true;
        }
        position = end + 1;
    }
    if (!hasCodecs)
        return container;

    size_t start = 0;
    int count = 0;
    CanPlay result = CanPlay::PROBABLY;
    while (start <= codecs.size()){
        size_t comma = codecs.find(',', start);
        if (comma == std::string::npos)
            comma = codecs.size();
        const std::string codec = lower(trim(codecs.substr(start, comma - start)));
        if (codec.empty())
            return CanPlay::NO;
        CanPlay one = CanPlay::NO;
        // AV1 and Opus in WebM: the engine's own decoders, on every platform
        if (webm && playableCodec(codec))
            one = CanPlay::PROBABLY;
        else
            one = fromSupport(VideoSourceFactory::codecSupport(codec, webm ? "webm" : "mp4"));
        if (one == CanPlay::NO)
            return CanPlay::NO;
        if (one == CanPlay::MAYBE)
            result = CanPlay::MAYBE;
        count++;
        start = comma + 1;
    }
    return count > 0 ? result : CanPlay::NO;
}

const char* VideoFormats::toString(CanPlay value){
    switch (value){
        case CanPlay::PROBABLY: return "probably";
        case CanPlay::MAYBE: return "maybe";
        case CanPlay::NO: return "";
    }
    return "";
}

namespace {

    bool mp4Extension(const Extension& extension){
        return extension.is("mp4") || extension.is("m4v") || extension.is("mov");
    }

    bool playable(const Extension& extension){
        return extension.is("webm") || extension.is("mkv") || mp4Extension(extension);
    }

    bool rejected(const Extension& extension){
        if (extension.length == 0)
            return false;
        for (const char* other : rejectedExtensions){
            if (extension.is(other))
                return true;
        }
        return false;
    }

}

bool VideoFormats::isPlayableExtension(const std::string& path){
    return playable(extensionOf(path));
}

bool VideoFormats::isMp4Extension(const std::string& path){
    return mp4Extension(extensionOf(path));
}

VideoFormats::CanPlay VideoFormats::canPlayExtension(const std::string& path){
    const Extension extension = extensionOf(path);
    if (extension.is("webm") || extension.is("mkv"))
        return CanPlay::PROBABLY;
    if (mp4Extension(extension))
        return fromSupport(VideoSourceFactory::codecSupport("h264", "mp4"));
    return CanPlay::NO;
}

bool VideoFormats::isRejectedExtension(const std::string& path){
    return rejected(extensionOf(path));
}

bool VideoFormats::isVideoPath(const std::string& path){
    const Extension extension = extensionOf(path);
    return playable(extension) || rejected(extension);
}

namespace {

    VideoProbe probeWith(VideoDemuxer& demuxer, bool opened);
    VideoProbe probeMp4(MediaByteSource& source);

    bool isEbml(const uint8_t* head, size_t size){
        return size >= 4 && head[0] == 0x1A && head[1] == 0x45 && head[2] == 0xDF && head[3] == 0xA3;
    }

    // which decoder plays it here: the software one for AV1/Opus WebM, else the platform's
    void decide(VideoProbe& probe){
        std::string reason;
        const VideoSourceFactory::Support support = VideoSourceFactory::probeSupport(probe, &reason, &probe.backend);
        if (support != VideoSourceFactory::Support::NO){
            probe.playable = true;
            probe.error = MediaError();
            return;
        }
        probe.playable = false;
        if (!probe.error){
            probe.error.code = MediaErrorCode::SRC_NOT_SUPPORTED;
            probe.error.message = reason;
        }
    }

}

VideoProbe VideoFormats::probeFile(const std::string& path){
    // MP4 by its content (the extension says nothing for sure); everything else as before
    std::string error;
    if (std::unique_ptr<MediaByteSource> source = MediaByteSource::openFile(path, error)){
        uint8_t head[12] = {};
        const int64_t got = source->readAt(0, head, sizeof(head));
        if (got > 0 && !isEbml(head, (size_t)got) && Mp4Probe::sniff(head, (size_t)got))
            return probeMp4(*source);
    }
    VideoDemuxer demuxer;
    const bool opened = demuxer.open(path, true);
    return probeWith(demuxer, opened);
}

VideoProbe VideoFormats::probeMemory(const uint8_t* data, size_t size, const std::string& name){
    if (data && size >= 8 && !isEbml(data, size) && Mp4Probe::sniff(data, size)){
        std::unique_ptr<MediaByteSource> source = MediaByteSource::openMemory(std::vector<uint8_t>(data, data + size), name);
        return probeMp4(*source);
    }
    VideoDemuxer demuxer;
    const bool opened = demuxer.openMemory(data, size, name, true);
    return probeWith(demuxer, opened);
}

VideoProbe VideoFormats::probeStdio(FILE* handle, const std::string& name){
    if (handle){
        uint8_t head[12] = {};
        const size_t got = std::fread(head, 1, sizeof(head), handle);
        std::fseek(handle, 0, SEEK_SET);
        if (got >= 8 && !isEbml(head, got) && Mp4Probe::sniff(head, got)){
            std::unique_ptr<MediaByteSource> source = MediaByteSource::openStdio(handle, name);
            if (!source){
                VideoProbe probe;
                probe.error.code = MediaErrorCode::SRC_NOT_SUPPORTED;
                probe.error.message = "'" + name + "' could not be read";
                return probe;
            }
            return probeMp4(*source);
        }
    }
    VideoDemuxer demuxer;
    const bool opened = demuxer.openStdio(handle, name, true);
    return probeWith(demuxer, opened);
}

namespace {

VideoProbe probeMp4(MediaByteSource& source){
    VideoProbe probe;
    const Mp4ProbeResult mp4 = Mp4Probe::probe(source);
    probe.readable = mp4.isoBmff;
    probe.container = mp4.isoBmff ? mp4.container : std::string();
    if (!mp4.ok){
        probe.error.code = MediaErrorCode::SRC_NOT_SUPPORTED;
        probe.error.message = "'" + source.path() + "': " + mp4.error;
        return probe;
    }
    probe.fastStart = mp4.fastStart;
    probe.fragmented = mp4.fragmented;
    probe.duration = mp4.duration;
    if (mp4.video.kind == Mp4Track::Kind::VIDEO){
        const Mp4Track& video = mp4.video;
        probe.videoCodec = video.encrypted ? "encv" : video.fourcc;
        probe.videoCodecString = video.codecString;
        probe.encrypted = probe.encrypted || video.encrypted;
        probe.rotation = video.rotation;
        probe.frameRate = video.frameRate;
        int width = video.presentationWidth > 0.0 ? (int)std::lround(video.presentationWidth) : video.width;
        int height = video.presentationHeight > 0.0 ? (int)std::lround(video.presentationHeight) : video.height;
        if (video.rotation == 90 || video.rotation == 270)
            std::swap(width, height);
        probe.width = width;
        probe.height = height;
    }
    if (mp4.audio.kind == Mp4Track::Kind::AUDIO){
        probe.audioCodec = mp4.audio.encrypted ? "enca" : mp4.audio.fourcc;
        probe.audioCodecString = mp4.audio.codecString;
        probe.audioChannels = mp4.audio.channels;
        probe.encrypted = probe.encrypted || mp4.audio.encrypted;
    }
    decide(probe);
    if (probe.playable && !probe.audioCodec.empty()){
        const std::string audio = VideoFormats::decoderName(probe.audioCodecString.empty() ? probe.audioCodec : probe.audioCodecString);
        probe.audioPlayable = VideoSourceFactory::codecSupport(audio.empty() ? probe.audioCodec : audio, probe.container) !=
                              VideoSourceFactory::Support::NO;
    }
    return probe;
}

VideoProbe probeWith(VideoDemuxer& demuxer, bool opened){
    VideoProbe probe;
    probe.container = demuxer.getDocType();
    probe.videoCodec = demuxer.getVideoCodecId();
    probe.audioCodec = demuxer.getAudioCodecId();
    probe.videoCodecString = probe.videoCodec;
    probe.audioCodecString = probe.audioCodec;
    probe.readable = opened || !probe.container.empty() || demuxer.getError().message.find("could not be read") == std::string::npos;
    if (!opened){
        probe.error = demuxer.getError();
        // another WebM codec (VP8, VP9, Vorbis) a platform decoder may have
        if (!probe.container.empty() && (!probe.videoCodec.empty() || !probe.audioCodec.empty()))
            decide(probe);
        return probe;
    }
    probe.playable = true;
    probe.backend = "webm";
    probe.audioPlayable = probe.audioCodec.empty() || demuxer.hasAudio();
    if (demuxer.hasVideo()){
        probe.width = demuxer.getVideoTrack().displayWidth;
        probe.height = demuxer.getVideoTrack().displayHeight;
    }
    if (demuxer.hasAudio())
        probe.audioChannels = demuxer.getAudioTrack().channels;
    probe.duration = demuxer.getDuration();
    // a VP9 picture with Opus sound: the platform's decoders play both when they can (else the
    // sound alone plays, as it always did)
    if (!probe.videoCodec.empty() && !demuxer.hasVideo()){
        VideoProbe both = probe;
        both.error = MediaError();
        decide(both);
        if (both.playable)
            probe.backend = both.backend;
    }
    return probe;
}

}

std::string VideoFormats::codecName(const std::string& id){
    static const std::pair<const char*, const char*> names[] = {
        {"V_AV1", "AV1"}, {"V_VP8", "VP8"}, {"V_VP9", "VP9"}, {"V_MPEG4/ISO/AVC", "H.264"}, {"V_MPEGH/ISO/HEVC", "HEVC"},
        {"V_THEORA", "Theora"}, {"V_MPEG1", "MPEG-1"}, {"V_MPEG2", "MPEG-2"}, {"V_MPEG4/ISO/ASP", "MPEG-4 Part 2"},
        {"V_MS/VFW/FOURCC", "VfW"}, {"V_PRORES", "ProRes"}, {"V_FFV1", "FFV1"}, {"A_OPUS", "Opus"}, {"A_VORBIS", "Vorbis"},
        {"A_AAC", "AAC"}, {"A_MPEG/L3", "MP3"}, {"A_FLAC", "FLAC"}, {"A_AC3", "AC-3"}, {"A_EAC3", "E-AC-3"}, {"A_PCM/INT/LIT", "PCM"},
        // FFmpeg's names (the Linux decoders report these)
        {"h264", "H.264"}, {"hevc", "HEVC"}, {"vp8", "VP8"}, {"vp9", "VP9"}, {"av1", "AV1"}, {"aac", "AAC"}, {"mp3", "MP3"},
        {"opus", "Opus"}, {"vorbis", "Vorbis"}, {"flac", "FLAC"}, {"ac3", "AC-3"}, {"eac3", "E-AC-3"}, {"alac", "ALAC"},
        {"mpeg4", "MPEG-4 Part 2"}, {"prores", "ProRes"},
    };
    for (const auto& name : names){
        if (id == name.first)
            return name.second;
    }
    if (id.rfind("A_AAC", 0) == 0)
        return "AAC";
    if (id.empty())
        return "none";
    // MP4 sample entries ("avc1", "mp4a", ...) and RFC 6381 strings ("avc1.42E01E")
    const std::string fourcc = id.size() > 4 && id[4] == '.' ? id.substr(0, 4) : id;
    const std::string name = Mp4Probe::codecName(fourcc);
    return name;
}

std::string VideoFormats::decoderName(const std::string& id){
    const std::string text = lower(trim(id));
    if (text.empty())
        return std::string();
    const std::string head = text.substr(0, text.find('.'));
    static const std::pair<const char*, const char*> names[] = {
        // Matroska ids (lower-cased)
        {"v_av1", "av1"}, {"v_vp8", "vp8"}, {"v_vp9", "vp9"}, {"v_mpeg4/iso/avc", "h264"}, {"v_mpegh/iso/hevc", "hevc"},
        {"a_opus", "opus"}, {"a_vorbis", "vorbis"}, {"a_mpeg/l3", "mp3"}, {"a_flac", "flac"}, {"a_ac3", "ac3"}, {"a_eac3", "eac3"},
        // MP4 sample entries and RFC 6381 codec strings
        {"avc1", "h264"}, {"avc3", "h264"}, {"hvc1", "hevc"}, {"hev1", "hevc"}, {"vp08", "vp8"}, {"vp09", "vp9"}, {"av01", "av1"},
        {"mp3 ", "mp3"}, {".mp3", "mp3"}, {"flac", "flac"}, {"ac-3", "ac3"}, {"ec-3", "eac3"}, {"alac", "alac"}, {"mp4v", "mpeg4"},
        // FFmpeg's own and the common short names
        {"h264", "h264"}, {"hevc", "hevc"}, {"h265", "hevc"}, {"vp8", "vp8"}, {"vp9", "vp9"}, {"av1", "av1"}, {"aac", "aac"},
        {"mp3", "mp3"}, {"opus", "opus"}, {"vorbis", "vorbis"}, {"ac3", "ac3"}, {"eac3", "eac3"},
    };
    for (const auto& name : names){
        if (text == name.first || head == name.first)
            return name.second;
    }
    if (text.rfind("a_aac", 0) == 0)
        return "aac";
    // mp4a.40.x is AAC, mp4a.69 / mp4a.6b MP3
    if (head == "mp4a"){
        if (text.rfind("mp4a.69", 0) == 0 || text.rfind("mp4a.6b", 0) == 0)
            return "mp3";
        return "aac";
    }
    return std::string();
}

const char* VideoFormats::conversionHint(){
    return "convert it to AV1 + Opus WebM: import it with import_files (it converts with FFmpeg), or run "
           "ffmpeg -i <file> -c:v libsvtav1 -crf 32 -c:a libopus <file>.webm";
}
