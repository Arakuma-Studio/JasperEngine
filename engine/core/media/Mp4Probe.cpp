// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Mp4Probe.h"

#include "MediaByteSource.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <numeric>

using namespace jasperengine;

namespace {

    constexpr int MAX_DEPTH = 12;
    constexpr int64_t MAX_LEAF_BOX = 4 << 20;          // a header box read whole (stsd, tkhd, ...)
    constexpr int64_t MAX_TABLE_BOX = 64 << 20;        // stts/ctts/stss
    constexpr int64_t MAX_MOOV_REWRITE = 256 << 20;    // rewriteFastStart keeps the moov in memory

    uint32_t be32(const uint8_t* p){ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
    uint16_t be16(const uint8_t* p){ return (uint16_t)((p[0] << 8) | p[1]); }
    uint64_t be64(const uint8_t* p){ return ((uint64_t)be32(p) << 32) | be32(p + 4); }
    void put32(uint8_t* p, uint32_t v){ p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
    void put64(uint8_t* p, uint64_t v){ put32(p, (uint32_t)(v >> 32)); put32(p + 4, (uint32_t)v); }

    std::string fourcc(const uint8_t* p){
        std::string text(reinterpret_cast<const char*>(p), 4);
        for (char& c : text){
            if ((unsigned char)c < 32 || (unsigned char)c > 126)
                c = '?';
        }
        return text;
    }

    bool isType(const uint8_t* p, const char* type){ return std::memcmp(p, type, 4) == 0; }

    struct BoxHeader {
        char type[5] = {};
        int64_t offset = 0;         // of the box
        int64_t headerSize = 0;
        int64_t size = 0;           // whole box
        int64_t payload() const { return offset + headerSize; }
        int64_t payloadSize() const { return size - headerSize; }
        int64_t end() const { return offset + size; }
        bool is(const char* other) const { return std::memcmp(type, other, 4) == 0; }
    };

    enum class ReadResult { OK, END, SHORT, ERROR };

    // up to `size` bytes, through short reads (a growing source returns what has arrived)
    int64_t readFully(MediaByteSource& source, int64_t offset, uint8_t* dst, int64_t size){
        int64_t done = 0;
        while (done < size){
            const int64_t got = source.readAt(offset + done, dst + done, (size_t)(size - done));
            if (got < 0)
                return done > 0 ? done : -1;
            if (got == 0)
                break;
            done += got;
        }
        return done;
    }

    // the header at `offset` of a box inside [.., limit)
    ReadResult readHeader(MediaByteSource& source, int64_t offset, int64_t limit, BoxHeader& box){
        uint8_t head[16];
        const int64_t got = readFully(source, offset, head, 8);
        if (got < 0)
            return ReadResult::ERROR;
        if (got == 0)
            return ReadResult::END;
        if (got < 8)
            return ReadResult::SHORT;
        std::memcpy(box.type, head + 4, 4);
        box.offset = offset;
        uint64_t size = be32(head);
        box.headerSize = 8;
        if (size == 1){
            if (readFully(source, offset + 8, head + 8, 8) < 8)
                return ReadResult::SHORT;
            size = be64(head + 8);
            box.headerSize = 16;
        }else if (size == 0){
            // to the end of the file (or of the parent)
            const int64_t total = limit >= 0 ? limit : source.size();
            if (total < 0)
                return ReadResult::SHORT;
            size = (uint64_t)(total - offset);
        }
        if (size < (uint64_t)box.headerSize || size > (uint64_t)INT64_MAX / 2)
            return ReadResult::ERROR;
        box.size = (int64_t)size;
        if (box.is("uuid"))
            box.headerSize += 16;
        return ReadResult::OK;
    }

    bool readBytes(MediaByteSource& source, int64_t offset, int64_t size, std::vector<uint8_t>& out){
        if (size < 0)
            return false;
        out.resize((size_t)size);
        int64_t done = 0;
        while (done < size){
            const int64_t got = source.readAt(offset + done, out.data() + done, (size_t)(size - done));
            if (got <= 0)
                return false;
            done += got;
        }
        return true;
    }

    // a cursor over a box's bytes in memory
    struct Cursor {
        const uint8_t* data;
        size_t size;
        size_t at = 0;
        bool ok = true;
        bool has(size_t n) const { return ok && at + n <= size; }
        uint8_t u8(){ if (!has(1)){ ok = false; return 0; } return data[at++]; }
        uint16_t u16(){ if (!has(2)){ ok = false; return 0; } const uint16_t v = be16(data + at); at += 2; return v; }
        uint32_t u32(){ if (!has(4)){ ok = false; return 0; } const uint32_t v = be32(data + at); at += 4; return v; }
        uint64_t u64(){ if (!has(8)){ ok = false; return 0; } const uint64_t v = be64(data + at); at += 8; return v; }
        void skip(size_t n){ if (!has(n)){ ok = false; at = size; return; } at += n; }
    };

    // MPEG-4 descriptor length (1-4 bytes of 7 bits)
    uint32_t descriptorLength(Cursor& c){
        uint32_t length = 0;
        for (int i = 0; i < 4; i++){
            const uint8_t b = c.u8();
            length = (length << 7) | (b & 0x7F);
            if (!(b & 0x80))
                break;
        }
        return length;
    }

    char hexDigit(int v){ return "0123456789ABCDEF"[v & 15]; }

    std::string hex2(uint8_t v){
        std::string text(2, '0');
        text[0] = hexDigit(v >> 4);
        text[1] = hexDigit(v);
        return text;
    }

    struct Parser {
        MediaByteSource& source;
        Mp4ProbeResult& result;
        Mp4Track current;
        uint32_t movieTimescale = 0;
        bool shortRead = false;

        explicit Parser(MediaByteSource& source, Mp4ProbeResult& result): source(source), result(result){}

        bool leaf(const BoxHeader& box, std::vector<uint8_t>& bytes, int64_t limit = MAX_LEAF_BOX){
            if (box.payloadSize() > limit)
                return false;
            if (!readBytes(source, box.payload(), box.payloadSize(), bytes)){
                shortRead = true;
                return false;
            }
            return true;
        }

        // the children of [start, end)
        bool children(int64_t start, int64_t end, int depth, const char* parent){
            if (depth > MAX_DEPTH)
                return false;
            int64_t offset = start;
            while (offset + 8 <= end){
                BoxHeader box;
                const ReadResult read = readHeader(source, offset, end, box);
                if (read == ReadResult::SHORT || read == ReadResult::END){
                    shortRead = true;
                    return false;
                }
                if (read == ReadResult::ERROR)
                    return false;
                if (box.end() > end)
                    box.size = end - box.offset;   // a box running past its parent: cut (damaged files)
                if (!childBox(box, depth, parent))
                    return false;
                offset = box.end();
            }
            return true;
        }

        bool childBox(const BoxHeader& box, int depth, const char* parent){
            std::vector<uint8_t> bytes;
            if (box.is("trak")){
                current = Mp4Track();
                if (!children(box.payload(), box.end(), depth + 1, "trak"))
                    return !shortRead;
                result.trackCount++;
                finishTrack();
                return true;
            }
            if (box.is("mdia") || box.is("minf") || box.is("stbl") || box.is("edts"))
                return children(box.payload(), box.end(), depth + 1, box.type) || !shortRead;
            if (box.is("mvex")){
                result.fragmented = true;
                return true;
            }
            if (box.is("mvhd")){
                if (!leaf(box, bytes))
                    return !shortRead;
                Cursor c{bytes.data(), bytes.size()};
                const uint8_t version = c.u8();
                c.skip(3);
                c.skip(version == 1 ? 16 : 8);
                movieTimescale = c.u32();
                const uint64_t duration = version == 1 ? c.u64() : c.u32();
                if (c.ok && movieTimescale > 0 && duration != UINT32_MAX && duration != UINT64_MAX)
                    result.duration = (double)duration / movieTimescale;
                return true;
            }
            if (box.is("tkhd")){
                if (!leaf(box, bytes))
                    return !shortRead;
                Cursor c{bytes.data(), bytes.size()};
                const uint8_t version = c.u8();
                const uint32_t flags = ((uint32_t)c.u8() << 16) | ((uint32_t)c.u8() << 8) | c.u8();
                c.skip(version == 1 ? 16 : 8);
                current.id = c.u32();
                c.skip(4);
                c.skip(version == 1 ? 8 : 4);
                c.skip(8 + 2 + 2 + 2 + 2);
                int32_t matrix[9];
                for (int32_t& m : matrix)
                    m = (int32_t)c.u32();
                const uint32_t width = c.u32();
                const uint32_t height = c.u32();
                if (!c.ok)
                    return true;
                current.enabled = (flags & 1) != 0;
                current.presentationWidth = width / 65536.0;
                current.presentationHeight = height / 65536.0;
                const double a = matrix[0] / 65536.0, b = matrix[1] / 65536.0;
                const double degrees = std::atan2(b, a) * 180.0 / 3.14159265358979323846;
                int rotation = (int)std::lround(degrees / 90.0) * 90;
                rotation = ((rotation % 360) + 360) % 360;
                current.rotation = rotation;
                return true;
            }
            if (box.is("elst")){
                if (!leaf(box, bytes))
                    return !shortRead;
                Cursor c{bytes.data(), bytes.size()};
                const uint8_t version = c.u8();
                c.skip(3);
                const uint32_t count = c.u32();
                for (uint32_t i = 0; i < count && c.ok; i++){
                    c.skip(version == 1 ? 8 : 4);
                    const int64_t mediaTime = version == 1 ? (int64_t)c.u64() : (int64_t)(int32_t)c.u32();
                    c.skip(4);
                    // an empty edit (-1) delays the start; the first real one says where the media starts
                    if (c.ok && mediaTime >= 0){
                        current.mediaTime = mediaTime;
                        break;
                    }
                }
                return true;
            }
            if (box.is("mdhd")){
                if (!leaf(box, bytes))
                    return !shortRead;
                Cursor c{bytes.data(), bytes.size()};
                const uint8_t version = c.u8();
                c.skip(3);
                c.skip(version == 1 ? 16 : 8);
                current.timescale = c.u32();
                const uint64_t duration = version == 1 ? c.u64() : c.u32();
                if (c.ok && duration != UINT32_MAX && duration != UINT64_MAX)
                    current.durationTicks = duration;
                return true;
            }
            if (box.is("hdlr")){
                if (!leaf(box, bytes))
                    return !shortRead;
                if (bytes.size() >= 12){
                    const uint8_t* type = bytes.data() + 8;
                    if (isType(type, "vide"))
                        current.kind = Mp4Track::Kind::VIDEO;
                    else if (isType(type, "soun"))
                        current.kind = Mp4Track::Kind::AUDIO;
                    else
                        current.kind = Mp4Track::Kind::OTHER;
                }
                return true;
            }
            if (box.is("stsd")){
                if (!leaf(box, bytes))
                    return !shortRead;
                parseStsd(bytes);
                return true;
            }
            if (box.is("stts")){
                current.sttsOffset = box.payload();
                current.sttsSize = box.payloadSize();
                if (!leaf(box, bytes, MAX_TABLE_BOX))
                    return !shortRead;
                Cursor c{bytes.data(), bytes.size()};
                c.skip(4);
                const uint32_t count = c.u32();
                uint64_t samples = 0, ticks = 0;
                for (uint32_t i = 0; i < count && c.has(8); i++){
                    const uint32_t n = c.u32();
                    const uint32_t delta = c.u32();
                    samples += n;
                    ticks += (uint64_t)n * delta;
                }
                current.sampleCount = (uint32_t)std::min<uint64_t>(samples, UINT32_MAX);
                if (ticks > 0 && current.timescale > 0)
                    current.frameRate = (double)samples * current.timescale / (double)ticks;
                return true;
            }
            if (box.is("ctts")){
                current.cttsOffset = box.payload();
                current.cttsSize = box.payloadSize();
                return true;
            }
            if (box.is("stss")){
                current.stssOffset = box.payload();
                current.stssSize = box.payloadSize();
                return true;
            }
            (void)parent;
            return true;
        }

        // the sample entry's codec configuration boxes, from `at` to the end
        void parseEntryChildren(const uint8_t* data, size_t size){
            size_t at = 0;
            while (at + 8 <= size){
                uint64_t boxSize = be32(data + at);
                const uint8_t* type = data + at + 4;
                size_t header = 8;
                if (boxSize == 1 && at + 16 <= size){
                    boxSize = be64(data + at + 8);
                    header = 16;
                }else if (boxSize == 0){
                    boxSize = size - at;
                }
                if (boxSize < header || at + boxSize > size)
                    break;
                const uint8_t* payload = data + at + header;
                const size_t payloadSize = (size_t)boxSize - header;
                if (isType(type, "avcC") && payloadSize >= 4){
                    current.codecString = current.fourcc + "." + hex2(payload[1]) + hex2(payload[2]) + hex2(payload[3]);
                    // High 10 (110) and above carry bit_depth_luma_minus8 after the SPS/PPS lists: rare, kept at 8
                }else if (isType(type, "hvcC") && payloadSize >= 23){
                    const int profile = payload[1] & 0x1F;
                    const int level = payload[12];
                    current.bitDepth = (payload[17] & 0x07) + 8;     // bitDepthLumaMinus8
                    current.codecString = current.fourcc + "." + std::to_string(profile) + ".L" + std::to_string(level);
                }else if (isType(type, "vpcC") && payloadSize >= 7){
                    // FullBox: version/flags, profile, level, bitDepth(4) chromaSubsampling(3) ...
                    const int profile = payload[4];
                    const int level = payload[5];
                    current.bitDepth = payload[6] >> 4;
                    char text[32];
                    std::snprintf(text, sizeof(text), "vp09.%02d.%02d.%02d", profile, level, current.bitDepth);
                    current.codecString = text;
                }else if (isType(type, "av1C") && payloadSize >= 3){
                    const int profile = payload[1] >> 5;
                    const int level = payload[1] & 0x1F;
                    const bool highBitDepth = (payload[2] & 0x40) != 0;
                    const bool twelveBit = (payload[2] & 0x20) != 0;
                    current.bitDepth = highBitDepth ? (twelveBit ? 12 : 10) : 8;
                    char text[32];
                    std::snprintf(text, sizeof(text), "av01.%d.%02dM.%02d", profile, level, current.bitDepth);
                    current.codecString = text;
                }else if (isType(type, "esds") && payloadSize > 4){
                    Cursor c{payload, payloadSize};
                    c.skip(4);
                    if (c.u8() == 0x03){
                        descriptorLength(c);
                        c.skip(2);
                        const uint8_t flags = c.u8();
                        if (flags & 0x80) c.skip(2);
                        if (flags & 0x40) c.skip(c.u8());
                        if (flags & 0x20) c.skip(2);
                    }
                    if (c.ok && c.u8() == 0x04){
                        descriptorLength(c);
                        const uint8_t objectType = c.u8();
                        c.skip(12);
                        std::string codec = "mp4a." + hex2(objectType);
                        std::transform(codec.begin(), codec.end(), codec.begin(), [](unsigned char ch){ return (char)std::tolower(ch); });
                        if (objectType == 0x40 && c.ok && c.u8() == 0x05){
                            descriptorLength(c);
                            const uint8_t first = c.u8();
                            int objectTypeAudio = first >> 3;
                            if (objectTypeAudio == 31 && c.ok)
                                objectTypeAudio = 32 + (((first & 7) << 3) | (c.u8() >> 5));
                            if (c.ok)
                                codec = "mp4a.40." + std::to_string(objectTypeAudio);
                        }
                        if (c.ok){
                            current.codecString = codec;
                            if (objectType == 0x69 || objectType == 0x6B)
                                current.fourcc = "mp3 ";
                        }
                    }
                }else if (isType(type, "sinf")){
                    // an encrypted entry's original format (frma)
                    size_t inner = 0;
                    while (inner + 8 <= payloadSize){
                        const uint32_t innerSize = be32(payload + inner);
                        if (innerSize < 8 || inner + innerSize > payloadSize)
                            break;
                        if (isType(payload + inner + 4, "frma") && innerSize >= 12)
                            current.codecString = fourcc(payload + inner + 8);
                        inner += innerSize;
                    }
                }
                at += (size_t)boxSize;
            }
        }

        void parseStsd(const std::vector<uint8_t>& bytes){
            Cursor c{bytes.data(), bytes.size()};
            c.skip(4);
            const uint32_t count = c.u32();
            if (!c.ok || count == 0 || !c.has(8))
                return;
            const size_t entryStart = c.at;
            const uint32_t entrySize = c.u32();
            if (entrySize < 16 || entryStart + entrySize > bytes.size())
                return;
            current.fourcc = fourcc(bytes.data() + entryStart + 4);
            current.codecString = current.fourcc;
            current.encrypted = current.fourcc == "encv" || current.fourcc == "enca";
            const uint8_t* entry = bytes.data() + entryStart;
            if (current.kind == Mp4Track::Kind::VIDEO){
                // VisualSampleEntry: 8 (box) + 8 (SampleEntry) + 16, then width, height
                if (entrySize >= 86){
                    current.width = be16(entry + 32);
                    current.height = be16(entry + 34);
                    parseEntryChildren(entry + 86, entrySize - 86);
                }
            }else if (current.kind == Mp4Track::Kind::AUDIO){
                // AudioSampleEntry: 8 (box) + 8 (SampleEntry) + version(2) ... channelcount at 24
                if (entrySize >= 36){
                    const uint16_t version = be16(entry + 16);   // QuickTime sound description version
                    current.channels = be16(entry + 24);
                    current.sampleRate = (int)(be32(entry + 32) >> 16);
                    size_t children = 36;
                    if (version == 1)
                        children += 16;
                    else if (version == 2){
                        children += 36;
                        if (entrySize >= 72){
                            uint64_t bits = be64(entry + 40);
                            double rate = 0.0;
                            std::memcpy(&rate, &bits, sizeof(rate));
                            if (std::isfinite(rate) && rate > 0.0 && rate < 1e7)
                                current.sampleRate = (int)rate;
                            current.channels = (int)be32(entry + 48);
                        }
                    }
                    if (children < entrySize)
                        parseEntryChildren(entry + children, entrySize - children);
                }
            }
            if (current.encrypted && current.codecString == current.fourcc)
                current.codecString = current.fourcc;
        }

        void finishTrack(){
            if (current.timescale > 0)
                current.duration = (double)current.durationTicks / current.timescale;
            if (current.kind == Mp4Track::Kind::VIDEO && result.video.kind == Mp4Track::Kind::NONE && current.enabled)
                result.video = current;
            else if (current.kind == Mp4Track::Kind::AUDIO && result.audio.kind == Mp4Track::Kind::NONE && current.enabled)
                result.audio = current;
            else if (current.kind == Mp4Track::Kind::VIDEO && result.video.kind == Mp4Track::Kind::NONE)
                result.video = current;
            else if (current.kind == Mp4Track::Kind::AUDIO && result.audio.kind == Mp4Track::Kind::NONE)
                result.audio = current;
        }
    };

    bool knownTopLevel(const uint8_t* type){
        static const char* const types[] = {"ftyp", "moov", "mdat", "free", "skip", "wide", "pnot", "uuid", "styp", "sidx", "moof", "meta"};
        for (const char* t : types){
            if (isType(type, t))
                return true;
        }
        return false;
    }

}

bool Mp4Probe::sniff(const uint8_t* data, size_t size){
    if (size < 8)
        return false;
    const uint32_t boxSize = be32(data);
    if (!knownTopLevel(data + 4))
        return false;
    // a plausible first box: ftyp is small; the others may be anything but 2..7
    return boxSize == 0 || boxSize == 1 || boxSize >= 8;
}

Mp4ProbeResult Mp4Probe::probe(MediaByteSource& source){
    Mp4ProbeResult result;
    uint8_t head[12] = {};
    const int64_t got = readFully(source, 0, head, sizeof(head));
    if (got < 8){
        result.needMoreData = got >= 0 && source.isGrowing();
        result.error = got < 0 ? "could not be read" : "too short to be a video";
        return result;
    }
    if (!sniff(head, (size_t)got)){
        result.error = "not an ISO-BMFF (MP4/MOV) file";
        return result;
    }
    result.isoBmff = true;

    Parser parser(source, result);
    int64_t offset = 0;
    bool moovRead = false;
    for (int boxes = 0; boxes < 100000; boxes++){
        BoxHeader box;
        const ReadResult read = readHeader(source, offset, -1, box);
        if (read == ReadResult::END)
            break;
        if (read == ReadResult::SHORT){
            result.needMoreData = true;
            break;
        }
        if (read == ReadResult::ERROR){
            if (!moovRead)
                result.error = "damaged box at offset " + std::to_string(offset);
            break;
        }
        if (box.is("ftyp")){
            std::vector<uint8_t> bytes;
            if (box.payloadSize() <= 4096 && readBytes(source, box.payload(), box.payloadSize(), bytes) && bytes.size() >= 8){
                result.majorBrand = fourcc(bytes.data());
                for (size_t i = 8; i + 4 <= bytes.size(); i += 4)
                    result.brands.push_back(fourcc(bytes.data() + i));
            }
        }else if (box.is("moov") && !moovRead){
            result.moovOffset = box.offset;
            const int64_t total = source.size();
            if (total >= 0 && box.end() > total){
                // a file still downloading (or cut): the moov is not all there
                if (source.isGrowing()){
                    result.needMoreData = true;
                    result.fastStart = result.mdatOffset < 0;
                    break;
                }
            }
            parser.shortRead = false;
            parser.children(box.payload(), box.end(), 1, "moov");
            if (parser.shortRead){
                result.needMoreData = source.isGrowing();
                if (!result.needMoreData)
                    result.error = "the file is cut short (its moov box is incomplete)";
                break;
            }
            moovRead = true;
            if (result.mdatOffset < 0)
                result.fastStart = true;
        }else if (box.is("mdat")){
            if (result.mdatOffset < 0)
                result.mdatOffset = box.offset;
            // the media data before the moov: no progressive start; past it the moov follows
        }else if (box.is("moof")){
            result.fragmented = true;
        }
        offset = box.end();
        if (moovRead && result.mdatOffset >= 0)
            break;
        // a growing file: what is past the received bytes is not known yet
        if (source.isGrowing() && offset >= source.availableBytes() && !moovRead){
            result.needMoreData = true;
            break;
        }
    }

    if (result.majorBrand == "qt  ")
        result.container = "mov";
    else if (result.majorBrand.rfind("3g", 0) == 0)
        result.container = "3gp";
    else
        result.container = "mp4";

    if (!moovRead){
        if (result.error.empty())
            result.error = result.needMoreData ? "waiting for the moov box" : "no moov box (not a complete MP4)";
        return result;
    }
    // fast start: the moov box precedes every mdat (an mdat found after it counts too)
    if (result.mdatOffset >= 0 && result.mdatOffset < result.moovOffset)
        result.fastStart = false;
    if (result.duration <= 0.0)
        result.duration = std::max(result.video.duration, result.audio.duration);
    if (result.video.kind == Mp4Track::Kind::NONE && result.audio.kind == Mp4Track::Kind::NONE){
        result.error = "no video or audio track";
        return result;
    }
    result.ok = true;
    result.needMoreData = false;
    return result;
}

int Mp4VideoIndex::frameAt(int64_t ns) const{
    if (ptsNs.empty())
        return -1;
    const auto it = std::upper_bound(ptsNs.begin(), ptsNs.end(), ns);
    if (it == ptsNs.begin())
        return 0;
    return (int)(it - ptsNs.begin()) - 1;
}

int Mp4VideoIndex::keyframeAtOrBefore(int index) const{
    if (keyframes.empty())
        return index < 0 ? -1 : 0;
    const auto it = std::upper_bound(keyframes.begin(), keyframes.end(), index);
    if (it == keyframes.begin())
        return keyframes.front();
    return *(it - 1);
}

int Mp4VideoIndex::keyframeAfter(int index) const{
    const auto it = std::upper_bound(keyframes.begin(), keyframes.end(), index);
    return it == keyframes.end() ? -1 : *it;
}

bool Mp4Probe::buildVideoIndex(MediaByteSource& source, const Mp4ProbeResult& result, Mp4VideoIndex& out, std::string& error){
    out = Mp4VideoIndex();
    const Mp4Track& track = result.video;
    if (track.kind != Mp4Track::Kind::VIDEO || track.timescale == 0 || track.sttsOffset < 0){
        error = "no video sample table";
        return false;
    }
    if (track.sttsSize > MAX_TABLE_BOX || track.cttsSize > MAX_TABLE_BOX || track.stssSize > MAX_TABLE_BOX){
        error = "the sample tables are too large";
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!readBytes(source, track.sttsOffset, track.sttsSize, bytes)){
        error = "the stts box could not be read";
        return false;
    }
    // decode times
    std::vector<int64_t> dts;
    {
        Cursor c{bytes.data(), bytes.size()};
        c.skip(4);
        const uint32_t count = c.u32();
        int64_t time = 0;
        for (uint32_t i = 0; i < count && c.has(8); i++){
            const uint32_t n = c.u32();
            const uint32_t delta = c.u32();
            if (dts.size() + n > (size_t)8 * 1024 * 1024){
                error = "too many samples";
                return false;
            }
            for (uint32_t k = 0; k < n; k++){
                dts.push_back(time);
                time += delta;
            }
        }
    }
    if (dts.empty()){
        error = "the video track has no samples (a fragmented file?)";
        return false;
    }
    // composition offsets
    std::vector<int64_t> cts = dts;
    if (track.cttsOffset >= 0 && readBytes(source, track.cttsOffset, track.cttsSize, bytes)){
        Cursor c{bytes.data(), bytes.size()};
        const uint8_t version = c.u8();
        c.skip(3);
        const uint32_t count = c.u32();
        size_t sample = 0;
        for (uint32_t i = 0; i < count && c.has(8) && sample < cts.size(); i++){
            const uint32_t n = c.u32();
            const uint32_t raw = c.u32();
            // version 0 is unsigned, but encoders write negative offsets into it too
            const int64_t offset = version == 1 ? (int64_t)(int32_t)raw : (int64_t)(int32_t)raw;
            for (uint32_t k = 0; k < n && sample < cts.size(); k++)
                cts[sample++] += offset;
        }
    }
    // presentation order
    std::vector<int> order(cts.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b){ return cts[(size_t)a] < cts[(size_t)b]; });
    std::vector<int> rank(cts.size());
    for (size_t i = 0; i < order.size(); i++)
        rank[(size_t)order[i]] = (int)i;
    // the edit list's start, else the first presented sample, is time 0
    const int64_t start = track.mediaTime > 0 ? track.mediaTime : cts[(size_t)order[0]];
    out.ptsNs.resize(cts.size());
    for (size_t i = 0; i < order.size(); i++){
        const int64_t ticks = cts[(size_t)order[i]] - start;
        out.ptsNs[i] = (int64_t)std::llround((long double)ticks * 1e9L / track.timescale);
    }
    // samples before the edit's start are not shown: frames before time 0 merge into frame 0
    size_t firstShown = 0;
    while (firstShown + 1 < out.ptsNs.size() && out.ptsNs[firstShown + 1] <= 0)
        firstShown++;
    // sync samples (1-based, decode order); none listed: every sample is one
    if (track.stssOffset >= 0 && readBytes(source, track.stssOffset, track.stssSize, bytes)){
        Cursor c{bytes.data(), bytes.size()};
        c.skip(4);
        const uint32_t count = c.u32();
        for (uint32_t i = 0; i < count && c.has(4); i++){
            const uint32_t number = c.u32();
            if (number >= 1 && number <= rank.size())
                out.keyframes.push_back(rank[number - 1]);
        }
    }else{
        out.keyframes.resize(out.ptsNs.size());
        std::iota(out.keyframes.begin(), out.keyframes.end(), 0);
    }
    if (firstShown > 0){
        out.ptsNs.erase(out.ptsNs.begin(), out.ptsNs.begin() + (std::ptrdiff_t)firstShown);
        out.ptsNs[0] = std::max<int64_t>(out.ptsNs[0], 0);
        for (int& key : out.keyframes)
            key = std::max(key - (int)firstShown, 0);
    }
    if (!out.ptsNs.empty() && out.ptsNs[0] > 0 && track.mediaTime <= 0){
        // no edit list: the first frame shows at 0
        const int64_t shift = out.ptsNs[0];
        for (int64_t& pts : out.ptsNs)
            pts -= shift;
    }
    std::sort(out.keyframes.begin(), out.keyframes.end());
    out.keyframes.erase(std::unique(out.keyframes.begin(), out.keyframes.end()), out.keyframes.end());
    if (out.keyframes.empty() || out.keyframes.front() != 0)
        out.keyframes.insert(out.keyframes.begin(), 0);
    out.frameRate = track.frameRate;
    return true;
}

std::string Mp4Probe::codecName(const std::string& code){
    static const std::pair<const char*, const char*> names[] = {
        {"avc1", "H.264"}, {"avc3", "H.264"}, {"hvc1", "HEVC"}, {"hev1", "HEVC"}, {"dvh1", "Dolby Vision"}, {"dvhe", "Dolby Vision"},
        {"vp08", "VP8"}, {"vp09", "VP9"}, {"av01", "AV1"}, {"mp4v", "MPEG-4 Part 2"}, {"s263", "H.263"}, {"apcn", "ProRes"},
        {"apch", "ProRes"}, {"apcs", "ProRes"}, {"apco", "ProRes"}, {"ap4h", "ProRes"}, {"jpeg", "Motion JPEG"},
        {"mp4a", "AAC"}, {"mp3 ", "MP3"}, {".mp3", "MP3"}, {"Opus", "Opus"}, {"fLaC", "FLAC"}, {"ac-3", "AC-3"}, {"ec-3", "E-AC-3"},
        {"alac", "ALAC"}, {"lpcm", "PCM"}, {"sowt", "PCM"}, {"twos", "PCM"}, {"samr", "AMR"}, {"encv", "encrypted video"},
        {"enca", "encrypted audio"},
    };
    for (const auto& name : names){
        if (code == name.first)
            return name.second;
    }
    return code;
}

// ---------------------------------------------------------------- fast start rewrite

namespace {

    struct FileCloser {
        void operator()(FILE* file) const { if (file) fclose(file); }
    };
    using FilePtr = std::unique_ptr<FILE, FileCloser>;

    bool seekFile(FILE* file, int64_t offset){
#if defined(_WIN32)
        return _fseeki64(file, offset, SEEK_SET) == 0;
#else
        return fseeko(file, (off_t)offset, SEEK_SET) == 0;
#endif
    }

    // every stco/co64 inside a moov payload in memory: adds `shift` to offsets below `limit`
    bool patchOffsets(uint8_t* data, size_t size, int64_t shift, int64_t limit, int depth, std::string& error){
        size_t at = 0;
        while (at + 8 <= size){
            uint64_t boxSize = be32(data + at);
            size_t header = 8;
            if (boxSize == 1){
                if (at + 16 > size)
                    return false;
                boxSize = be64(data + at + 8);
                header = 16;
            }else if (boxSize == 0){
                boxSize = size - at;
            }
            if (boxSize < header || at + boxSize > size){
                error = "damaged moov box";
                return false;
            }
            uint8_t* type = data + at + 4;
            uint8_t* payload = data + at + header;
            const size_t payloadSize = (size_t)boxSize - header;
            if (isType(type, "trak") || isType(type, "mdia") || isType(type, "minf") || isType(type, "stbl")){
                if (depth > MAX_DEPTH || !patchOffsets(payload, payloadSize, shift, limit, depth + 1, error))
                    return false;
            }else if (isType(type, "stco") && payloadSize >= 8){
                const uint32_t count = be32(payload + 4);
                if (8 + (uint64_t)count * 4 > payloadSize){
                    error = "damaged stco box";
                    return false;
                }
                for (uint32_t i = 0; i < count; i++){
                    uint8_t* entry = payload + 8 + (size_t)i * 4;
                    const int64_t value = be32(entry);
                    const int64_t moved = value < limit ? value + shift : value;
                    if (moved > UINT32_MAX){
                        error = "the file needs 64-bit chunk offsets once optimized; use: ffmpeg -i <file> -c copy -movflags +faststart <out>.mp4";
                        return false;
                    }
                    put32(entry, (uint32_t)moved);
                }
            }else if (isType(type, "co64") && payloadSize >= 8){
                const uint32_t count = be32(payload + 4);
                if (8 + (uint64_t)count * 8 > payloadSize){
                    error = "damaged co64 box";
                    return false;
                }
                for (uint32_t i = 0; i < count; i++){
                    uint8_t* entry = payload + 8 + (size_t)i * 8;
                    const int64_t value = (int64_t)be64(entry);
                    put64(entry, (uint64_t)(value < limit ? value + shift : value));
                }
            }
            at += (size_t)boxSize;
        }
        return true;
    }

    bool copyRange(FILE* in, FILE* out, int64_t offset, int64_t size){
        if (!seekFile(in, offset))
            return false;
        std::vector<uint8_t> buffer(1 << 20);
        while (size > 0){
            const size_t chunk = (size_t)std::min<int64_t>(size, (int64_t)buffer.size());
            if (fread(buffer.data(), 1, chunk, in) != chunk || fwrite(buffer.data(), 1, chunk, out) != chunk)
                return false;
            size -= (int64_t)chunk;
        }
        return true;
    }

}

bool Mp4Probe::rewriteFastStart(const std::string& inputPath, const std::string& outputPath, std::string& error){
    std::string openError;
    std::unique_ptr<MediaByteSource> source = MediaByteSource::openFile(inputPath, openError);
    if (!source){
        error = openError;
        return false;
    }
    // the top-level boxes
    std::vector<BoxHeader> boxes;
    int64_t offset = 0;
    const int64_t total = source->size();
    while (offset < total){
        BoxHeader box;
        const ReadResult read = readHeader(*source, offset, total, box);
        if (read != ReadResult::OK || box.end() > total){
            error = "damaged box at offset " + std::to_string(offset);
            return false;
        }
        boxes.push_back(box);
        offset = box.end();
    }
    int moov = -1;
    bool moof = false;
    for (size_t i = 0; i < boxes.size(); i++){
        if (boxes[i].is("moov") && moov < 0)
            moov = (int)i;
        if (boxes[i].is("moof"))
            moof = true;
    }
    if (moov < 0){
        error = "no moov box";
        return false;
    }
    if (moof){
        error = "a fragmented MP4 streams as it is";
        return false;
    }
    const BoxHeader& moovBox = boxes[(size_t)moov];
    if (moovBox.size > MAX_MOOV_REWRITE){
        error = "the moov box is too large to rewrite";
        return false;
    }
    std::vector<uint8_t> moovBytes;
    if (!readBytes(*source, moovBox.offset, moovBox.size, moovBytes)){
        error = "the moov box could not be read";
        return false;
    }
    source.reset();

    // the new order: ftyp (and what precedes the first media box), the moov, everything else
    size_t front = 0;
    while (front < boxes.size() && (boxes[front].is("ftyp") || boxes[front].is("pnot")))
        front++;
    bool before = false;
    for (size_t i = front; i < (size_t)moov; i++){
        if (boxes[i].is("mdat"))
            before = true;
    }
    const int64_t insertAt = front < boxes.size() ? boxes[front].offset : moovBox.offset;
    // data between insertAt and the moov moves forward by the moov's size; data after it stays
    if (before && !patchOffsets(moovBytes.data() + moovBox.headerSize, moovBytes.size() - (size_t)moovBox.headerSize, moovBox.size,
                                moovBox.offset, 0, error))
        return false;

    FilePtr in(fopen(inputPath.c_str(), "rb"));
    FilePtr out(fopen(outputPath.c_str(), "wb"));
    if (!in || !out){
        error = !in ? "'" + inputPath + "' could not be read" : "'" + outputPath + "' could not be written";
        return false;
    }
    bool ok = copyRange(in.get(), out.get(), 0, insertAt);
    ok = ok && fwrite(moovBytes.data(), 1, moovBytes.size(), out.get()) == moovBytes.size();
    for (size_t i = front; ok && i < boxes.size(); i++){
        if ((int)i == moov)
            continue;
        ok = copyRange(in.get(), out.get(), boxes[i].offset, boxes[i].size);
    }
    ok = ok && fflush(out.get()) == 0;
    if (!ok)
        error = "writing '" + outputPath + "' failed";
    return ok;
}
