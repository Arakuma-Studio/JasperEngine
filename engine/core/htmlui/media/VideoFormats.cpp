// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VideoFormats.h"

#include "VideoDemuxer.h"

#include <algorithm>
#include <cctype>
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

    std::string extensionOf(std::string path){
        const size_t cut = path.find_first_of("?#");
        if (cut != std::string::npos)
            path = path.substr(0, cut);
        const size_t slash = path.find_last_of("/\\");
        const size_t dot = path.find_last_of('.');
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
            return std::string();
        return lower(path.substr(dot + 1));
    }

    // RFC 6381 codec strings of the engine's two codecs
    bool playableCodec(const std::string& codec){
        if (codec == "opus")
            return true;
        if (codec == "av01" || codec.rfind("av01.", 0) == 0)
            return true;
        return false;
    }

    const char* const rejectedExtensions[] = {"mp4", "m4v", "mov", "qt", "ogv", "ogg", "ogm", "avi", "wmv", "asf", "flv", "f4v",
                                              "mpg", "mpeg", "mpe", "m2v", "m1v", "ts", "m2ts", "mts", "3gp", "3g2", "vob", "divx",
                                              "rm", "rmvb", "y4m", "gifv", "m4a", "mp3", "aac", "wav", "flac"};

}

VideoFormats::CanPlay VideoFormats::canPlayType(const std::string& mimeType){
    // type/subtype[; codecs="a, b"] (RFC 6381), other parameters ignored
    const std::string text = trim(mimeType);
    const size_t semicolon = text.find(';');
    const std::string type = lower(trim(text.substr(0, semicolon)));
    if (type != "video/webm" && type != "audio/webm")
        return CanPlay::NO;
    if (semicolon == std::string::npos)
        return CanPlay::MAYBE;

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
        return CanPlay::MAYBE;

    size_t start = 0;
    int count = 0;
    while (start <= codecs.size()){
        size_t comma = codecs.find(',', start);
        if (comma == std::string::npos)
            comma = codecs.size();
        const std::string codec = lower(trim(codecs.substr(start, comma - start)));
        if (codec.empty() || !playableCodec(codec))
            return CanPlay::NO;
        count++;
        start = comma + 1;
    }
    return count > 0 ? CanPlay::PROBABLY : CanPlay::NO;
}

const char* VideoFormats::toString(CanPlay value){
    switch (value){
        case CanPlay::PROBABLY: return "probably";
        case CanPlay::MAYBE: return "maybe";
        case CanPlay::NO: return "";
    }
    return "";
}

bool VideoFormats::isPlayableExtension(const std::string& path){
    const std::string extension = extensionOf(path);
    return extension == "webm" || extension == "mkv";
}

bool VideoFormats::isRejectedExtension(const std::string& path){
    const std::string extension = extensionOf(path);
    for (const char* rejected : rejectedExtensions){
        if (extension == rejected)
            return true;
    }
    return false;
}

bool VideoFormats::isVideoPath(const std::string& path){
    return isPlayableExtension(path) || isRejectedExtension(path);
}

namespace {

    VideoProbe probeWith(VideoDemuxer& demuxer, bool opened);

}

VideoProbe VideoFormats::probeFile(const std::string& path){
    VideoDemuxer demuxer;
    const bool opened = demuxer.open(path, true);
    return probeWith(demuxer, opened);
}

VideoProbe VideoFormats::probeMemory(const uint8_t* data, size_t size, const std::string& name){
    VideoDemuxer demuxer;
    const bool opened = demuxer.openMemory(data, size, name, true);
    return probeWith(demuxer, opened);
}

VideoProbe VideoFormats::probeStdio(FILE* handle, const std::string& name){
    VideoDemuxer demuxer;
    const bool opened = demuxer.openStdio(handle, name, true);
    return probeWith(demuxer, opened);
}

namespace {

VideoProbe probeWith(VideoDemuxer& demuxer, bool opened){
    VideoProbe probe;
    probe.container = demuxer.getDocType();
    probe.videoCodec = demuxer.getVideoCodecId();
    probe.audioCodec = demuxer.getAudioCodecId();
    probe.readable = opened || !probe.container.empty() || demuxer.getError().message.find("could not be read") == std::string::npos;
    if (!opened){
        probe.error = demuxer.getError();
        return probe;
    }
    probe.playable = true;
    probe.audioPlayable = probe.audioCodec.empty() || demuxer.hasAudio();
    if (demuxer.hasVideo()){
        probe.width = demuxer.getVideoTrack().displayWidth;
        probe.height = demuxer.getVideoTrack().displayHeight;
    }
    if (demuxer.hasAudio())
        probe.audioChannels = demuxer.getAudioTrack().channels;
    probe.duration = demuxer.getDuration();
    return probe;
}

}

std::string VideoFormats::codecName(const std::string& id){
    static const std::pair<const char*, const char*> names[] = {
        {"V_AV1", "AV1"}, {"V_VP8", "VP8"}, {"V_VP9", "VP9"}, {"V_MPEG4/ISO/AVC", "H.264"}, {"V_MPEGH/ISO/HEVC", "HEVC"},
        {"V_THEORA", "Theora"}, {"V_MPEG1", "MPEG-1"}, {"V_MPEG2", "MPEG-2"}, {"V_MPEG4/ISO/ASP", "MPEG-4 Part 2"},
        {"V_MS/VFW/FOURCC", "VfW"}, {"V_PRORES", "ProRes"}, {"V_FFV1", "FFV1"}, {"A_OPUS", "Opus"}, {"A_VORBIS", "Vorbis"},
        {"A_AAC", "AAC"}, {"A_MPEG/L3", "MP3"}, {"A_FLAC", "FLAC"}, {"A_AC3", "AC-3"}, {"A_EAC3", "E-AC-3"}, {"A_PCM/INT/LIT", "PCM"},
    };
    for (const auto& name : names){
        if (id == name.first)
            return name.second;
    }
    if (id.rfind("A_AAC", 0) == 0)
        return "AAC";
    return id.empty() ? std::string("none") : id;
}

const char* VideoFormats::conversionHint(){
    return "convert it to AV1 + Opus WebM: import it with import_files (it converts with FFmpeg), or run "
           "ffmpeg -i <file> -c:v libsvtav1 -crf 32 -c:a libopus <file>.webm";
}
