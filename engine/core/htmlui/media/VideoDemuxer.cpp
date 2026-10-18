// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VideoDemuxer.h"

#include "io/File.h"
#include "io/FileData.h"

#include <mkvparser/mkvparser.h>
#include <opus.h>

#include <algorithm>
#include <cstring>
#include <limits>

using namespace jasperengine;

namespace {

    constexpr long long BLOCK_SIZE = 64 * 1024;
    constexpr size_t CACHE_BLOCKS = 4;

    VideoColor::Matrix matrixOf(long long coefficients){
        // ISO/IEC 23091-4 MatrixCoefficients
        switch (coefficients){
            case 0: return VideoColor::Matrix::IDENTITY;
            case 1: return VideoColor::Matrix::BT709;
            case 5:
            case 6: return VideoColor::Matrix::BT601;
            case 9:
            case 10: return VideoColor::Matrix::BT2020;
            default: return VideoColor::Matrix::UNSPECIFIED;
        }
    }

}

// mkvparser's reader over a FileData handle, with a few 64 KiB blocks cached (mkvparser reads
// element headers a few bytes at a time)
class VideoDemuxer::Reader : public mkvparser::IMkvReader {
public:
    explicit Reader(FileData* file): file(file){
        total = file ? (long long)file->length() : 0;
    }

    ~Reader() override{
        delete file;
    }

    int Read(long long pos, long len, unsigned char* buf) override{
        if (!file || pos < 0 || len < 0 || pos + len > total){
            return -1;
        }
        if (len == 0)
            return 0;
        // large reads (frame data) go straight to the file
        if (len >= BLOCK_SIZE){
            return readDirect(pos, len, buf) ? 0 : -1;
        }
        while (len > 0){
            const long long blockStart = (pos / BLOCK_SIZE) * BLOCK_SIZE;
            const Block* block = load(blockStart);
            if (!block)
                return -1;
            const long long inBlock = pos - blockStart;
            const long long available = (long long)block->data.size() - inBlock;
            if (available <= 0)
                return -1;
            const long count = (long)std::min<long long>(available, len);
            std::memcpy(buf, block->data.data() + inBlock, (size_t)count);
            buf += count;
            pos += count;
            len -= count;
        }
        return 0;
    }

    int Length(long long* totalOut, long long* availableOut) override{
        if (totalOut)
            *totalOut = total;
        if (availableOut)
            *availableOut = total;
        return 0;
    }

    bool failed = false;

    size_t cacheBytes() const{
        size_t bytes = 0;
        for (const Block& block : blocks)
            bytes += block.data.capacity();
        return bytes;
    }

private:
    struct Block {
        long long start = -1;
        uint64_t used = 0;
        std::vector<unsigned char> data;
    };

    bool readDirect(long long pos, long long len, unsigned char* buf){
        if (pos > (long long)std::numeric_limits<int>::max()){
            failed = true;
            return false;
        }
        file->seek((int)pos);
        const unsigned int read = file->read(buf, (unsigned int)len);
        if ((long long)read != len){
            failed = true;
            return false;
        }
        return true;
    }

    const Block* load(long long start){
        for (Block& block : blocks){
            if (block.start == start){
                block.used = ++clock;
                return &block;
            }
        }
        Block* target = nullptr;
        if (blocks.size() < CACHE_BLOCKS){
            blocks.emplace_back();
            target = &blocks.back();
        }else{
            target = &blocks[0];
            for (Block& block : blocks){
                if (block.used < target->used)
                    target = &block;
            }
        }
        const long long size = std::min(BLOCK_SIZE, total - start);
        target->data.resize((size_t)size);
        target->start = -1;
        if (!readDirect(start, size, target->data.data()))
            return nullptr;
        target->start = start;
        target->used = ++clock;
        return target;
    }

    FileData* file = nullptr;
    long long total = 0;
    std::vector<Block> blocks;
    uint64_t clock = 0;
};

VideoDemuxer::VideoDemuxer() = default;

VideoDemuxer::~VideoDemuxer(){
    close();
}

void VideoDemuxer::close(){
    segment.reset();
    reader.reset();
    videoPackets.clear();
    audioPackets.clear();
    videoTrackNumber = -1;
    audioTrackNumber = -1;
    durationNs = 0;
}

bool VideoDemuxer::fail(MediaErrorCode code, const std::string& message){
    error.code = code;
    error.message = message;
    return false;
}

bool VideoDemuxer::open(const std::string& filePath, bool headersOnly){
    close();
    error = MediaError();
    path = filePath;
    docType.clear();
    videoCodecId.clear();
    audioCodecId.clear();
    videoTrack = VideoTrackInfo();
    audioTrack = AudioTrackInfo();

    FileData* file = FileData::newFile(filePath.c_str(), true);
    // a File whose fopen failed has no handle (its length() would read through it)
    File* handle = dynamic_cast<File*>(file);
    if (!file || (handle && !handle->getFilePtr()) || file->length() == 0){
        delete file;
        return fail(MediaErrorCode::SRC_NOT_SUPPORTED, "'" + filePath + "' could not be read");
    }
    reader = std::make_unique<Reader>(file);

    long long position = 0;
    mkvparser::EBMLHeader header;
    if (header.Parse(reader.get(), position) < 0 || !header.m_docType)
        return fail(MediaErrorCode::SRC_NOT_SUPPORTED, "'" + filePath + "' is not a WebM file (no EBML header)");
    docType = header.m_docType;
    if (docType != "webm" && docType != "matroska")
        return fail(MediaErrorCode::SRC_NOT_SUPPORTED, "'" + filePath + "' is an EBML file of type '" + docType + "', not WebM");

    mkvparser::Segment* created = nullptr;
    if (mkvparser::Segment::CreateInstance(reader.get(), position, created) != 0 || !created)
        return fail(MediaErrorCode::SRC_NOT_SUPPORTED, "'" + filePath + "' has no WebM segment");
    segment.reset(created);
    if (segment->ParseHeaders() < 0 || !segment->GetInfo() || !segment->GetTracks())
        return fail(MediaErrorCode::SRC_NOT_SUPPORTED, "'" + filePath + "' has damaged WebM headers");

    const mkvparser::Tracks* tracks = segment->GetTracks();
    for (unsigned long i = 0; i < tracks->GetTracksCount(); i++){
        const mkvparser::Track* track = tracks->GetTrackByIndex(i);
        if (!track)
            continue;
        const std::string codec = track->GetCodecId() ? track->GetCodecId() : "";
        const bool video = track->GetType() == mkvparser::Track::kVideo;
        const bool audio = track->GetType() == mkvparser::Track::kAudio;
        if (!video && !audio)
            continue;
        if (video && videoCodecId.empty())
            videoCodecId = codec;
        if (audio && audioCodecId.empty())
            audioCodecId = codec;
        const bool playable = (video && codec == "V_AV1" && videoTrackNumber < 0) || (audio && codec == "A_OPUS" && audioTrackNumber < 0);
        if (!playable)
            continue;

        // encrypted (or header-stripped) content cannot be decoded
        for (unsigned long e = 0; e < track->GetContentEncodingCount(); e++){
            const mkvparser::ContentEncoding* encoding = track->GetContentEncodingByIndex(e);
            if (encoding && (encoding->GetEncryptionCount() > 0 || encoding->GetCompressionCount() > 0)){
                return fail(MediaErrorCode::SRC_NOT_SUPPORTED, "'" + filePath + "': the " + std::string(video ? "video" : "audio") +
                                                                   " track is encrypted or compressed (ContentEncoding)");
            }
        }

        size_t privateSize = 0;
        const unsigned char* privateData = track->GetCodecPrivate(privateSize);
        if (video){
            const mkvparser::VideoTrack* vt = static_cast<const mkvparser::VideoTrack*>(track);
            videoTrackNumber = track->GetNumber();
            videoCodecId = codec;
            videoTrack.codecId = codec;
            videoTrack.width = (int)vt->GetWidth();
            videoTrack.height = (int)vt->GetHeight();
            videoTrack.displayWidth = videoTrack.width;
            videoTrack.displayHeight = videoTrack.height;
            // DisplayUnit 0 = pixels; other units (cm, inches, aspect ratio) say only the ratio
            const long long displayWidth = vt->GetDisplayWidth();
            const long long displayHeight = vt->GetDisplayHeight();
            if (displayWidth > 0 && displayHeight > 0){
                if (vt->GetDisplayUnit() == 0){
                    videoTrack.displayWidth = (int)displayWidth;
                    videoTrack.displayHeight = (int)displayHeight;
                }else if (videoTrack.height > 0){
                    videoTrack.displayWidth = (int)((double)videoTrack.height * (double)displayWidth / (double)displayHeight + 0.5);
                }
            }
            videoTrack.defaultDurationNs = (int64_t)track->GetDefaultDuration();
            if (privateData && privateSize)
                videoTrack.codecPrivate.assign(privateData, privateData + privateSize);
            if (const mkvparser::Colour* colour = vt->GetColour()){
                if (colour->matrix_coefficients != mkvparser::Colour::kValueNotPresent)
                    videoTrack.color.matrix = matrixOf(colour->matrix_coefficients);
                if (colour->range == 2)
                    videoTrack.color.fullRange = true;
                if (colour->primaries != mkvparser::Colour::kValueNotPresent)
                    videoTrack.color.primaries = (int)colour->primaries;
                if (colour->transfer_characteristics != mkvparser::Colour::kValueNotPresent)
                    videoTrack.color.transfer = (int)colour->transfer_characteristics;
            }
            if (videoTrack.width <= 0 || videoTrack.height <= 0 || videoTrack.width > 4096 || videoTrack.height > 4096){
                return fail(MediaErrorCode::SRC_NOT_SUPPORTED, "'" + filePath + "': video size " + std::to_string(videoTrack.width) + "x" +
                                                                   std::to_string(videoTrack.height) + " (1 to 4096 pixels)");
            }
        }else{
            const mkvparser::AudioTrack* at = static_cast<const mkvparser::AudioTrack*>(track);
            audioTrackNumber = track->GetNumber();
            audioCodecId = codec;
            audioTrack.codecId = codec;
            audioTrack.channels = (int)at->GetChannels();
            audioTrack.sampleRate = at->GetSamplingRate();
            audioTrack.codecDelayNs = (int64_t)track->GetCodecDelay();
            audioTrack.seekPreRollNs = (int64_t)track->GetSeekPreRoll();
            if (privateData && privateSize)
                audioTrack.codecPrivate.assign(privateData, privateData + privateSize);
        }
    }

    if (videoTrackNumber < 0 && audioTrackNumber < 0){
        std::string found;
        if (!videoCodecId.empty())
            found += "video " + videoCodecId;
        if (!audioCodecId.empty())
            found += std::string(found.empty() ? "" : ", ") + "audio " + audioCodecId;
        return fail(MediaErrorCode::SRC_NOT_SUPPORTED, "'" + filePath + "' has no AV1 video or Opus audio track (" +
                                                           (found.empty() ? std::string("no tracks") : found) + ")");
    }
    if (videoTrackNumber < 0 && !videoCodecId.empty()){
        return fail(MediaErrorCode::SRC_NOT_SUPPORTED, "'" + filePath + "': the video codec is " + videoCodecId + ", not AV1 (V_AV1)");
    }

    const long long segmentDuration = segment->GetInfo()->GetDuration();
    durationNs = segmentDuration > 0 ? segmentDuration : 0;

    if (headersOnly)
        return true;
    return buildIndex();
}

bool VideoDemuxer::buildIndex(){
    if (segment->Load() < 0 && !segment->GetFirst())
        return fail(MediaErrorCode::SRC_NOT_SUPPORTED, "'" + path + "' has no clusters (no frames)");

    const mkvparser::Cluster* cluster = segment->GetFirst();
    std::vector<uint8_t> head;
    while (cluster && !cluster->EOS()){
        const mkvparser::BlockEntry* entry = nullptr;
        if (cluster->GetFirst(entry) < 0)
            break;
        while (entry && !entry->EOS()){
            const mkvparser::Block* block = entry->GetBlock();
            const long long track = block ? block->GetTrackNumber() : -1;
            if (block && (track == videoTrackNumber || track == audioTrackNumber)){
                const bool video = track == videoTrackNumber;
                int64_t pts = block->GetTime(cluster);
                const int frames = block->GetFrameCount();
                for (int f = 0; f < frames; f++){
                    const mkvparser::Block::Frame& frame = block->GetFrame(f);
                    if (frame.len <= 0)
                        continue;
                    VideoPacketRef packet;
                    packet.offset = frame.pos;
                    packet.size = (uint32_t)frame.len;
                    packet.ptsNs = pts;
                    packet.key = block->IsKey();
                    if (video){
                        videoPackets.push_back(packet);
                    }else{
                        if (f == frames - 1)
                            packet.discardPaddingNs = block->GetDiscardPadding();
                        audioPackets.push_back(packet);
                        // laced frames: the next starts after this one's samples
                        if (frames > 1){
                            head.resize(std::min<size_t>(packet.size, 8));
                            if (reader->Read(packet.offset, (long)head.size(), head.data()) == 0){
                                const int samples = opus_packet_get_nb_samples(head.data(), (opus_int32)head.size(), 48000);
                                if (samples > 0)
                                    pts += (int64_t)samples * 1000000000LL / 48000;
                            }
                        }
                    }
                }
            }
            if (cluster->GetNext(entry, entry) < 0)
                break;
        }
        cluster = segment->GetNext(cluster);
    }
    if (reader->failed)
        return fail(MediaErrorCode::NETWORK, "'" + path + "' could not be read to the end");

    if (videoTrackNumber >= 0){
        if (videoPackets.empty())
            return fail(MediaErrorCode::SRC_NOT_SUPPORTED, "'" + path + "' has a video track without frames");
        for (size_t i = 1; i < videoPackets.size(); i++){
            if (videoPackets[i].ptsNs <= videoPackets[i - 1].ptsNs)
                return fail(MediaErrorCode::SRC_NOT_SUPPORTED, "'" + path + "': video frames out of presentation order (frame " +
                                                                std::to_string(i) + ")");
        }
        // the first frame is where decoding starts, whatever its flag says
        videoPackets[0].key = true;
    }

    // a duration the segment does not give: the end of the last frame
    if (durationNs <= 0){
        int64_t end = 0;
        if (!videoPackets.empty()){
            int64_t frameDuration = videoTrack.defaultDurationNs;
            if (frameDuration <= 0)
                frameDuration = videoPackets.size() > 1 ? (videoPackets.back().ptsNs - videoPackets[videoPackets.size() - 2].ptsNs) : 33333333;
            end = std::max(end, videoPackets.back().ptsNs + frameDuration);
        }
        if (!audioPackets.empty()){
            std::vector<uint8_t> last;
            int64_t packetDuration = 20000000;
            if (read(audioPackets.back(), last)){
                const int samples = opus_packet_get_nb_samples(last.data(), (opus_int32)last.size(), 48000);
                if (samples > 0)
                    packetDuration = (int64_t)samples * 1000000000LL / 48000;
            }
            end = std::max(end, audioPackets.back().ptsNs + packetDuration - audioPackets.back().discardPaddingNs);
        }
        durationNs = end;
    }
    return true;
}

const MediaError& VideoDemuxer::getError() const{
    return error;
}

const std::string& VideoDemuxer::getDocType() const{
    return docType;
}

const std::string& VideoDemuxer::getVideoCodecId() const{
    return videoCodecId;
}

const std::string& VideoDemuxer::getAudioCodecId() const{
    return audioCodecId;
}

bool VideoDemuxer::hasVideo() const{
    return videoTrackNumber >= 0;
}

bool VideoDemuxer::hasAudio() const{
    return audioTrackNumber >= 0;
}

const VideoTrackInfo& VideoDemuxer::getVideoTrack() const{
    return videoTrack;
}

const AudioTrackInfo& VideoDemuxer::getAudioTrack() const{
    return audioTrack;
}

const std::vector<VideoPacketRef>& VideoDemuxer::getVideoPackets() const{
    return videoPackets;
}

const std::vector<VideoPacketRef>& VideoDemuxer::getAudioPackets() const{
    return audioPackets;
}

double VideoDemuxer::getDuration() const{
    return (double)durationNs / 1e9;
}

int64_t VideoDemuxer::getDurationNs() const{
    return durationNs;
}

namespace {

    int lastAtOrBefore(const std::vector<VideoPacketRef>& packets, int64_t ptsNs){
        if (packets.empty())
            return -1;
        auto it = std::upper_bound(packets.begin(), packets.end(), ptsNs, [](int64_t value, const VideoPacketRef& packet){
            return value < packet.ptsNs;
        });
        if (it == packets.begin())
            return 0;
        return (int)(it - packets.begin()) - 1;
    }

}

int VideoDemuxer::videoFrameAt(int64_t ptsNs) const{
    return lastAtOrBefore(videoPackets, ptsNs);
}

int VideoDemuxer::audioPacketAt(int64_t ptsNs) const{
    // audio packets of laced blocks may share a pts: the search takes the last of them, so step back
    int index = lastAtOrBefore(audioPackets, ptsNs);
    while (index > 0 && audioPackets[index - 1].ptsNs == audioPackets[index].ptsNs)
        index--;
    return index;
}

int VideoDemuxer::keyframeAtOrBefore(int index) const{
    if (videoPackets.empty())
        return -1;
    index = std::clamp(index, 0, (int)videoPackets.size() - 1);
    while (index > 0 && !videoPackets[index].key)
        index--;
    return index;
}

int VideoDemuxer::keyframeAfter(int index) const{
    for (int i = std::max(index + 1, 0); i < (int)videoPackets.size(); i++){
        if (videoPackets[i].key)
            return i;
    }
    return -1;
}

bool VideoDemuxer::read(const VideoPacketRef& packet, std::vector<uint8_t>& out){
    out.resize(packet.size);
    if (!reader || packet.size == 0 || reader->Read(packet.offset, (long)packet.size, out.data()) != 0){
        return fail(MediaErrorCode::NETWORK, "'" + path + "' could not be read at offset " + std::to_string(packet.offset));
    }
    return true;
}

size_t VideoDemuxer::getCacheBytes() const{
    return reader ? reader->cacheBytes() : 0;
}
