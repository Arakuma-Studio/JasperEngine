// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AudioFileCheck.h"

#include "soloud.h"

// stb_vorbis's header (SoLoud's copy, the one the engine links) maps FILE to SoLoud's File after stdio
#include <cstdio>

// SoLoud's decode buffers (Wav::loadwav's float tmp[512 * MAX_CHANNELS], WavStream's) and mixer
static constexpr unsigned int SOLOUD_MAX_CHANNELS = MAX_CHANNELS;
#undef MAX_CHANNELS

// the decoders SoLoud compiles (soloud/src/audiosource/wav/dr_impl.cpp, with its options, and
// soloud/src/audiosource/wav/stb_vorbis.c): declarations only
#define DR_WAV_NO_STDIO
#include "soloud/src/audiosource/wav/dr_wav.h"
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_CRC
#include "soloud/src/audiosource/wav/dr_flac.h"
#define DR_MP3_NO_STDIO
#define DR_MP3_FLOAT_OUTPUT
#include "soloud/src/audiosource/wav/dr_mp3.h"
#define STB_VORBIS_NO_STDIO
#include "soloud/src/audiosource/wav/stb_vorbis.h"

#include <algorithm>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace jasperengine;

static_assert(AudioFileCheck::MAX_CHANNELS == SOLOUD_MAX_CHANNELS, "AudioFileCheck::MAX_CHANNELS is SoLoud's MAX_CHANNELS");

namespace {

uint16_t readU16(const unsigned char* p){
    return (uint16_t)(p[0] | (p[1] << 8));
}

uint32_t readU32(const unsigned char* p){
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void writeU32(unsigned char* p, uint32_t value){
    p[0] = (unsigned char)(value & 0xFF);
    p[1] = (unsigned char)((value >> 8) & 0xFF);
    p[2] = (unsigned char)((value >> 16) & 0xFF);
    p[3] = (unsigned char)((value >> 24) & 0xFF);
}

// What clampWavSizes writes: the RIFF size, the data chunk's size, and for an RF64 file the
// RIFF tag with its ds64 chunk renamed JUNK (a chunk every reader skips: no byte moves)
struct WavSizes{
    uint32_t riffSize = 0;
    uint64_t dataSizeAt = 0;
    uint32_t dataSize = 0;
    uint64_t ds64At = 0;
    bool rf64 = false;
};

// False when the bytes need no change, or are not a RIFF/RF64 WAVE file with a data chunk.
// Offsets are 64-bit: a chunk size of 0xFFFFFFFF must not wrap a 32-bit size_t (web, Android).
bool planWavSizes(const unsigned char* b, uint64_t n, WavSizes& sizes){
    if (!b || n < 12 || std::memcmp(b + 8, "WAVE", 4) != 0) {
        return false;
    }
    sizes.rf64 = std::memcmp(b, "RF64", 4) == 0;
    if (!sizes.rf64 && std::memcmp(b, "RIFF", 4) != 0) {
        return false;
    }
    // the RIFF sizes are 32-bit
    if (n - 8 > 0xFFFFFFFFull) {
        return false;
    }
    bool ds64 = false;
    uint64_t ds64Data = 0;
    uint32_t blockAlign = 0;
    for (uint64_t pos = 12; pos + 8 <= n;) {
        const uint32_t size = readU32(b + pos + 4);
        if (std::memcmp(b + pos, "data", 4) == 0) {
            if (sizes.rf64 && !ds64) {
                return false;
            }
            const uint64_t present = n - (pos + 8);
            // RF64 keeps the size in ds64; 0 there is a stream that never went back to write it
            uint64_t declared = size;
            if (sizes.rf64 && size == 0xFFFFFFFFu) {
                declared = ds64Data > 0 ? ds64Data : UINT64_MAX;
            }
            uint64_t data = declared;
            if (declared > present) {
                data = blockAlign > 0 ? present - present % blockAlign : present;
            }
            if (data > 0xFFFFFFFFull) {
                return false;
            }
            const uint32_t riff = readU32(b + 4);
            sizes.riffSize = sizes.rf64 ? (uint32_t)(n - 8) : (uint32_t)std::min<uint64_t>(riff, n - 8);
            sizes.dataSizeAt = pos + 4;
            sizes.dataSize = (uint32_t)data;
            return sizes.rf64 || data != size || sizes.riffSize != riff;
        }
        if (sizes.rf64 && pos == 12 && std::memcmp(b + pos, "ds64", 4) == 0 && size >= 24 && pos + 8 + 24 <= n) {
            // riff size (8 bytes), data size (8), sample count (8), ...
            ds64 = true;
            sizes.ds64At = pos;
            ds64Data = (uint64_t)readU32(b + pos + 16) | ((uint64_t)readU32(b + pos + 20) << 32);
        } else if (std::memcmp(b + pos, "fmt ", 4) == 0 && size >= 16 && pos + 8 + 16 <= n) {
            blockAlign = (uint32_t)b[pos + 20] | ((uint32_t)b[pos + 21] << 8);
        }
        pos += 8 + (uint64_t)size + (size & 1);
    }
    return false;
}

std::string text(const char* format, ...){
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return buffer;
}

// The channels and the rate SoLoud can decode and mix
bool checkChannelsAndRate(const AudioFileFormat& format, std::string& error){
    if (format.channels < AudioFileCheck::MIN_CHANNELS || format.channels > AudioFileCheck::MAX_CHANNELS) {
        error = text("%u channels (%u-%u are supported)", format.channels, AudioFileCheck::MIN_CHANNELS, AudioFileCheck::MAX_CHANNELS);
        return false;
    }
    if (format.sampleRate < AudioFileCheck::MIN_SAMPLE_RATE || format.sampleRate > AudioFileCheck::MAX_SAMPLE_RATE) {
        error = text("sample rate %u Hz (%u-%u Hz are supported)", format.sampleRate, AudioFileCheck::MIN_SAMPLE_RATE,
                     AudioFileCheck::MAX_SAMPLE_RATE);
        return false;
    }
    return true;
}

// A length, and a whole decode within maxSamples (frames * channels), a stream within WavStream's
// 32-bit frame count
bool checkLength(AudioFileFormat& format, AudioDecode decode, uint64_t maxSamples, std::string& error){
    if (format.frames == 0) {
        error = "it holds no audio";
        return false;
    }
    if (decode == AudioDecode::Whole && format.frames > maxSamples / format.channels) {
        format.tooLong = true;
        error = text("decodes to %.1f min of audio (%u channel(s) at %u Hz): a clip loads whole up to %llu samples",
                     (double)format.frames / format.sampleRate / 60.0, format.channels, format.sampleRate,
                     (unsigned long long)maxSamples);
        return false;
    }
    if (decode == AudioDecode::Stream && format.frames > AudioFileCheck::MAX_STREAM_FRAMES) {
        format.tooLong = true;
        error = text("decodes to %.1f min of audio: a stream holds up to %llu frames", (double)format.frames / format.sampleRate / 60.0,
                     (unsigned long long)AudioFileCheck::MAX_STREAM_FRAMES);
        return false;
    }
    return true;
}

// The frames an ADPCM data chunk of `bytes` holds, decoded as dr_wav does: blocks of blockAlign
// bytes, each a header per channel (MS: 7 bytes, two frames; IMA: 4 bytes, one frame) and then
// nibbles (MS: one byte at a time; IMA: 4 bytes per channel at a time, 8 frames). A last, partial
// block decodes what it holds.
uint64_t adpcmFrames(uint16_t tag, uint64_t bytes, unsigned int blockAlign, unsigned int channels){
    const bool ms = tag == DR_WAVE_FORMAT_ADPCM;
    const uint64_t header = (ms ? 7 : 4) * (uint64_t)channels;
    auto block = [&](uint64_t size) -> uint64_t {
        if (size < header) return 0;
        return ms ? 2 + (size - header) * 2 / channels : 1 + (size - header) / (4 * (uint64_t)channels) * 8;
    };
    return (bytes / blockAlign) * block(blockAlign) + block(bytes % blockAlign);
}

const char* encodingName(uint16_t tag){
    switch (tag) {
        case DR_WAVE_FORMAT_PCM: return "PCM";
        case DR_WAVE_FORMAT_IEEE_FLOAT: return "float";
        case DR_WAVE_FORMAT_ALAW: return "A-law";
        case DR_WAVE_FORMAT_MULAW: return "mu-law";
        case DR_WAVE_FORMAT_ADPCM: return "MS ADPCM";
        case DR_WAVE_FORMAT_DVI_ADPCM: return "IMA ADPCM";
        default: return "";
    }
}

// A RIFF WAVE file, as dr_wav 0.13.2 reads it (drwav_init__internal): the fmt chunk is the first
// one so named, wherever it is; the data chunk the first after it. dr_wav also takes the size of
// the last data chunk and divides by the frame size before it checks it, so both are checked here
// before dr_wav runs; then dr_wav's own view is checked against the bytes.
bool checkWav(const unsigned char* b, uint64_t n, AudioDecode decode, AudioFileFormat& format, uint64_t maxSamples, std::string& error){
    format.codec = "wav";
    if (n < 12 || std::memcmp(b + 8, "WAVE", 4) != 0) {
        error = "a RIFF file that is not a WAVE file";
        return false;
    }
    uint64_t fmtAt = 0;
    uint32_t fmtSize = 0;
    for (uint64_t pos = 12; pos + 8 <= n;) {
        const uint32_t size = readU32(b + pos + 4);
        if (std::memcmp(b + pos, "fmt ", 4) == 0) {
            fmtAt = pos;
            fmtSize = size;
            break;
        }
        pos += 8 + (uint64_t)size + (size & 1);
    }
    if (fmtAt == 0) {
        error = "WAV file without a fmt chunk";
        return false;
    }
    // dr_wav's first test of a RIFF file
    if (readU32(b + 4) < 36) {
        error = text("its RIFF size is %u (36 at least)", readU32(b + 4));
        return false;
    }
    // dr_wav reads 16 bytes whatever the size says, and then walks on from the wrong place
    if (fmtSize < 16) {
        error = text("its fmt chunk is %u bytes (16 at least)", fmtSize);
        return false;
    }
    const unsigned char* fmt = b + fmtAt + 8;
    if (fmtAt + 8 + 16 > n || (fmtSize > 16 && fmtAt + 8 + 18 > n)) {
        error = "the file ends inside its fmt chunk";
        return false;
    }
    const uint16_t formatTag = readU16(fmt);
    const unsigned int channels = readU16(fmt + 2);
    const unsigned int sampleRate = readU32(fmt + 4);
    const unsigned int blockAlign = readU16(fmt + 12);
    const unsigned int bits = readU16(fmt + 14);
    // WAVE_FORMAT_EXTENSIBLE: the encoding is the sub-format's first two bytes (cbSize 22)
    uint16_t tag = formatTag;
    if (formatTag == DR_WAVE_FORMAT_EXTENSIBLE) {
        if (fmtSize < 40 || readU16(fmt + 16) != 22 || fmtAt + 8 + 40 > n) {
            error = "its WAVE_FORMAT_EXTENSIBLE fmt chunk is malformed (no 22-byte extension)";
            return false;
        }
        tag = readU16(fmt + 24);
    }
    format.channels = channels;
    format.sampleRate = sampleRate;
    if (!checkChannelsAndRate(format, error)) {
        return false;
    }
    const char* name = encodingName(tag);
    if (!*name) {
        error = text("WAV encoding (format tag 0x%04X) is not supported: PCM, float, A-law, mu-law, MS ADPCM and IMA ADPCM are", tag);
        return false;
    }
    if (bits == 0 || bits > 64) {
        error = text("%s WAV with %u bits per sample", name, bits);
        return false;
    }
    if (blockAlign == 0) {
        error = "its fmt chunk gives a block align of 0";
        return false;
    }
    // dr_wav's bytes per frame (drwav_get_bytes_per_pcm_frame)
    const unsigned int frameBytes = bits % 8 == 0 ? bits * channels / 8 : blockAlign;
    const bool adpcm = tag == DR_WAVE_FORMAT_ADPCM || tag == DR_WAVE_FORMAT_DVI_ADPCM;
    if (tag == DR_WAVE_FORMAT_ALAW || tag == DR_WAVE_FORMAT_MULAW) {
        // anything but one byte a sample is 0 bytes a frame to dr_wav, which divides by it
        if (frameBytes != channels) {
            error = text("%s WAV with %u bits per sample in %u-byte frames (8 bits, one byte per channel, are supported)", name, bits,
                         blockAlign);
            return false;
        }
    } else if (tag == DR_WAVE_FORMAT_IEEE_FLOAT) {
        if (bits != 32 && bits != 64) {
            error = text("float WAV with %u bits per sample (32 and 64 are supported)", bits);
            return false;
        }
    } else if (tag == DR_WAVE_FORMAT_PCM) {
        // dr_wav converts samples of 1 to 8 bytes; other sizes come out silent
        if (frameBytes / channels < 1 || frameBytes / channels > 8) {
            error = text("PCM WAV with %u bits per sample in %u-byte frames for %u channel(s)", bits, frameBytes, channels);
            return false;
        }
    } else if (adpcm) {
        const unsigned int header = (tag == DR_WAVE_FORMAT_ADPCM ? 7 : 4) * channels;
        if (channels > 2) {
            error = text("%s WAV with %u channels (1 or 2 are supported)", name, channels);
            return false;
        }
        if (blockAlign < header) {
            error = text("%s WAV with %u-byte blocks (a block's header alone is %u bytes)", name, blockAlign, header);
            return false;
        }
    }

    // the chunks after fmt as dr_wav walks them, to the end of the file: it reads from the first
    // data chunk but takes the last one's size
    int dataChunks = 0;
    for (uint64_t pos = fmtAt; pos + 8 <= n;) {
        const uint32_t size = readU32(b + pos + 4);
        if (std::memcmp(b + pos, "data", 4) == 0) dataChunks++;
        pos += 8 + (uint64_t)size + (size & 1);
    }
    if (dataChunks == 0) {
        error = "WAV file without a data chunk after its fmt chunk";
        return false;
    }
    if (dataChunks > 1) {
        error = text("WAV file with %d data chunks (one is supported)", dataChunks);
        return false;
    }

    drwav wav;
    if (!drwav_init_memory(&wav, b, (size_t)n, nullptr)) {
        error = "can't read this WAV file's chunks";
        return false;
    }
    bool ok = true;
    if (wav.channels != channels || wav.sampleRate != sampleRate || wav.translatedFormatTag != tag) {
        // this reading and dr_wav's differ: refuse rather than guess which one SoLoud gets
        error = "can't read this WAV file's fmt chunk";
        ok = false;
    } else if (wav.dataChunkDataPos > n || wav.dataChunkDataSize > n - wav.dataChunkDataPos) {
        error = "its data chunk runs past the end of the file";
        ok = false;
    }
    if (ok) {
        format.frames = wav.totalPCMFrameCount;
        if (adpcm) {
            const uint64_t holds = adpcmFrames(tag, wav.dataChunkDataSize, blockAlign, channels);
            if (format.frames > holds) {
                // dr_wav counts a partial last block as whole, and wraps below 0 for one shorter than its header
                error = text("its %s data (%llu bytes in %u-byte blocks) holds %llu frames, but dr_wav counts more (a last block "
                             "shorter than its header)", name, (unsigned long long)wav.dataChunkDataSize, blockAlign,
                             (unsigned long long)holds);
                ok = false;
            }
        }
    }
    // MS ADPCM: dr_wav reads a block's predictor byte per channel and indexes a 7-entry coefficient
    // table with it, with no bounds check. A predictor of 7 or more reads out of bounds, so every
    // block's predictors are checked here, against the data dr_wav sees.
    if (ok && tag == DR_WAVE_FORMAT_ADPCM) {
        for (uint64_t at = wav.dataChunkDataPos; at + (uint64_t)channels <= wav.dataChunkDataPos + wav.dataChunkDataSize; at += blockAlign) {
            for (unsigned int c = 0; c < channels; c++) {
                if (b[at + c] >= 7) {
                    error = text("its MS ADPCM data is damaged: a block's predictor is %u (0-6 are valid)", b[at + c]);
                    ok = false;
                    break;
                }
            }
            if (!ok) break;
        }
    }
    // IMA ADPCM: dr_wav stops decoding at a block whose step index (a header byte per channel) is
    // past its 89-entry table, and loadwav (patched) leaves the frames after it silent. Every
    // block's step indices are checked here, so such a file is refused as damaged without decoding.
    if (ok && tag == DR_WAVE_FORMAT_DVI_ADPCM) {
        const uint64_t end = wav.dataChunkDataPos + wav.dataChunkDataSize;
        for (uint64_t at = wav.dataChunkDataPos; ok && at + 4 * (uint64_t)channels <= end; at += blockAlign) {
            for (unsigned int c = 0; c < channels; c++) {
                if (b[at + 4 * c + 2] >= 89) {
                    error = text("its IMA ADPCM data is damaged: a block's step index is %u (0-88 are valid)", b[at + 4 * c + 2]);
                    ok = false;
                    break;
                }
            }
        }
    }
    ok = ok && checkLength(format, decode, maxSamples, error);
    drwav_uninit(&wav);
    return ok;
}

// The codec of an Ogg file's first stream, from the start of its first packet (the first page's
// first segment): "Vorbis", another codec's name, or "" when the bytes say none
const char* oggCodec(const unsigned char* b, uint64_t n){
    static const struct { const char* magic; size_t size; const char* name; } codecs[] = {
        {"\x01vorbis", 7, "Vorbis"},
        {"OpusHead", 8, "Opus"},
        {"\x7F" "FLAC", 5, "FLAC"},
        {"Speex   ", 8, "Speex"},
        {"CELT    ", 8, "CELT"},
        {"PCM     ", 8, "PCM"},
        {"\x80theora", 7, "Theora video"},
        {"OVP80", 5, "VP8 video"},
        {"BBCD\0", 5, "Dirac video"},
        {"\x80kate\0\0\0", 8, "Kate subtitles"},
        {"fishead\0", 8, "a Skeleton index"},
    };
    if (n < 28) {
        return "";
    }
    const uint64_t at = 27 + (uint64_t)b[26];
    for (const auto& codec : codecs) {
        if (at + codec.size <= n && b[26] > 0 && b[27] >= codec.size && std::memcmp(b + at, codec.magic, codec.size) == 0) {
            return codec.name;
        }
    }
    return "";
}

// stb_vorbis 1.22 allocates the comment header's strings and list at the lengths they state,
// before it reads them, and its cleanup after a failed list allocation crashes. So the comment
// header (the second packet, from the second page on, as stb_vorbis reads it) must hold the
// lengths it states. A second packet that is no Vorbis comment header stb_vorbis refuses itself.
bool oggCommentsFit(const unsigned char* b, uint64_t n, std::string& error){
    // the first page holds one segment, the identification header (stb_vorbis refuses others)
    if (n < 28 || b[26] != 1) {
        return true;
    }
    uint64_t pos = 28 + (uint64_t)b[27];
    std::vector<unsigned char> packet;
    bool complete = false;
    while (!complete && pos + 27 <= n && std::memcmp(b + pos, "OggS", 4) == 0) {
        const unsigned int segments = b[pos + 26];
        if (pos + 27 + segments > n) {
            break;
        }
        uint64_t at = pos + 27 + segments;
        for (unsigned int s = 0; s < segments && !complete; s++) {
            const unsigned int size = b[pos + 27 + s];
            const uint64_t have = at < n ? std::min<uint64_t>(size, n - at) : 0;
            packet.insert(packet.end(), b + at, b + at + have);
            // a segment the file cuts short reads as zeros to stb_vorbis's memory reader
            packet.insert(packet.end(), size - have, 0);
            at += size;
            complete = size < 255;
        }
        pos = at;
    }
    // \x03vorbis, the vendor's length and text, the comment count, each comment's length and text
    const uint64_t size = packet.size();
    if (size < 7 || std::memcmp(packet.data(), "\x03vorbis", 7) != 0) {
        return true;
    }
    // stb_vorbis reads a length the packet does not hold as end-of-packet bytes (0xFF): a huge one
    if (size < 11) {
        error = "its comment header is damaged: it ends before its vendor string's length";
        return false;
    }
    uint64_t at = 11;
    const uint64_t vendor = readU32(packet.data() + 7);
    if (vendor > size - at) {
        error = text("its comment header is damaged: its vendor string's length (%llu bytes) runs past the header (%llu bytes)",
                     (unsigned long long)vendor, (unsigned long long)size);
        return false;
    }
    at += vendor;
    if (at + 4 > size) {
        error = "its comment header is damaged: it ends before its comment count";
        return false;
    }
    const uint64_t comments = readU32(packet.data() + at);
    at += 4;
    if (comments > (size - at) / 4) {
        error = text("its comment header is damaged: it counts %llu comments in %llu bytes", (unsigned long long)comments,
                     (unsigned long long)(size - at));
        return false;
    }
    for (uint64_t i = 0; i < comments; i++) {
        const uint64_t length = readU32(packet.data() + at);
        at += 4;
        if (length > size - at) {
            error = text("its comment header is damaged: comment %llu's length (%llu bytes) runs past the header",
                         (unsigned long long)(i + 1), (unsigned long long)length);
            return false;
        }
        at += length;
        if (i + 1 < comments && at + 4 > size) {
            error = text("its comment header is damaged: it counts %llu comments but holds %llu", (unsigned long long)comments,
                         (unsigned long long)(i + 1));
            return false;
        }
    }
    return true;
}

// The next Ogg capture pattern at or after `from`, n when there is none
uint64_t nextOggPage(const unsigned char* b, uint64_t n, uint64_t from){
    while (from + 4 <= n) {
        const void* found = std::memchr(b + from, 'O', n - from - 3);
        if (!found) break;
        from = (uint64_t)(static_cast<const unsigned char*>(found) - b);
        if (std::memcmp(b + from, "OggS", 4) == 0) return from;
        from++;
    }
    return n;
}

// What an Ogg file's pages can hold, read from their headers alone (no decoding). A Vorbis packet
// gives at most half the stream's largest block in frames (stb_vorbis_info::max_frame_size), and a
// page's granule position counts the frames up to its last packet. So no page of the first
// stream (the one stb_vorbis decodes) can count more frames than its packets so far hold, and the
// length stb_vorbis reads from the file's last page can't be more than every packet in the file
// holds. A part cut from a longer stream without new granules (it starts at a large one), or a
// granule a damaged or made-up file states, would otherwise size a clip of silence.
bool oggPagesHoldLength(const unsigned char* b, uint64_t n, uint64_t length, unsigned int packetFrames, std::string& error){
    if (n < 27) {
        return true;
    }
    const uint64_t most = std::max(1u, packetFrames);
    const uint32_t serial = readU32(b + 14);
    uint64_t packets = 0, streamPackets = 0, page = 0;
    // stb_vorbis resynchronises at the next capture pattern; so does this walk
    for (uint64_t pos = 0; (pos = nextOggPage(b, n, pos)) + 27 <= n;) {
        const unsigned int segments = b[pos + 26];
        if (pos + 27 + segments > n) break;
        uint64_t body = 0, ended = 0;
        for (unsigned int s = 0; s < segments; s++) {
            body += b[pos + 27 + s];
            ended += b[pos + 27 + s] < 255;
        }
        packets += ended;
        page++;
        if (readU32(b + pos + 14) == serial) {
            streamPackets += ended;
            const int64_t granule = (int64_t)((uint64_t)readU32(b + pos + 6) | ((uint64_t)readU32(b + pos + 10) << 32));
            if (granule > 0 && (uint64_t)granule > streamPackets * most) {
                error = text("the Ogg file is cut or damaged: its page %llu counts %llu frames, more than the %llu packets up to it "
                             "hold (a part cut from a longer stream keeps its granules: re-encode it)",
                             (unsigned long long)page, (unsigned long long)granule, (unsigned long long)streamPackets);
                return false;
            }
        }
        pos += 27 + segments + body;
    }
    if (length > packets * most) {
        error = text("the Ogg file is cut or damaged: its last page counts %llu frames, but its %llu packets hold at most %llu",
                     (unsigned long long)length, (unsigned long long)packets, (unsigned long long)(packets * most));
        return false;
    }
    return true;
}

// Ogg Vorbis, as SoLoud's loadogg reads it: the length is the last page's granule position
// (stb_vorbis_stream_length_in_samples, 0 when there is none). loadogg (patched) allocates that
// length, writes what the stream decodes up to it and leaves the rest silent, so the headers and
// the pages are all the check reads: a stream that decodes past its last granule (untrimmed end
// padding) is cut to it, as other decoders do, and one that decodes less ends in silence.
bool checkOgg(const unsigned char* b, uint64_t n, AudioDecode decode, AudioFileFormat& format, uint64_t maxSamples, std::string& error){
    format.codec = "ogg";
    if (n > (uint64_t)INT_MAX) {
        error = "an Ogg file larger than 2 GB";
        return false;
    }
    const char* codec = oggCodec(b, n);
    if (*codec && std::strcmp(codec, "Vorbis") != 0) {
        error = text("this Ogg file holds %s, not Vorbis: only Ogg Vorbis is supported", codec);
        return false;
    }
    if (!oggCommentsFit(b, n, error)) {
        return false;
    }
    int failure = 0;
    stb_vorbis* vorbis = stb_vorbis_open_memory(b, (int)n, &failure, nullptr);
    if (!vorbis) {
        error = "can't read this Ogg file (only Ogg Vorbis is supported)";
        return false;
    }
    const stb_vorbis_info info = stb_vorbis_get_info(vorbis);
    format.channels = (unsigned int)info.channels;
    format.sampleRate = info.sample_rate;
    format.frames = stb_vorbis_stream_length_in_samples(vorbis);
    stb_vorbis_close(vorbis);
    if (!checkChannelsAndRate(format, error)) {
        return false;
    }
    if (format.frames == 0) {
        error = "its length can't be read: no page at its end gives one (a cut or damaged Ogg file)";
        return false;
    }
    return checkLength(format, decode, maxSamples, error) &&
           oggPagesHoldLength(b, n, format.frames, (unsigned int)info.max_frame_size, error);
}

// CRC-8 of a FLAC frame header (polynomial x^8 + x^2 + x + 1, from 0)
uint8_t flacCrc8(const unsigned char* p, uint64_t size){
    uint8_t crc = 0;
    for (uint64_t i = 0; i < size; i++) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; bit++) crc = (uint8_t)((crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1);
    }
    return crc;
}

// A FLAC frame header at b[at], as dr_flac reads one (drflac__read_next_flac_frame_header) and with
// its CRC-8 right: the first frame it holds and how many
bool flacFrameAt(const unsigned char* b, uint64_t n, uint64_t at, unsigned int maxBlock, uint64_t& first, uint64_t& frames){
    if (at + 6 > n || b[at] != 0xFF || (b[at + 1] & 0xFE) != 0xF8) return false;
    const bool variable = b[at + 1] & 1;
    const unsigned int blockCode = b[at + 2] >> 4, rateCode = b[at + 2] & 0x0F;
    const unsigned int channelCode = b[at + 3] >> 4, bitsCode = (b[at + 3] >> 1) & 7;
    if (blockCode == 0 || rateCode == 15 || channelCode > 10 || bitsCode == 3 || bitsCode == 7 || (b[at + 3] & 1)) return false;
    // the frame (fixed blocks) or first sample (variable) number, UTF-8 coded: up to 6 or 7 bytes
    uint64_t p = at + 4;
    const unsigned char lead = b[p++];
    int more = 0;
    uint64_t number = lead;
    if (lead >= 0x80) {
        if ((lead & 0xE0) == 0xC0) { more = 1; number = lead & 0x1F; }
        else if ((lead & 0xF0) == 0xE0) { more = 2; number = lead & 0x0F; }
        else if ((lead & 0xF8) == 0xF0) { more = 3; number = lead & 0x07; }
        else if ((lead & 0xFC) == 0xF8) { more = 4; number = lead & 0x03; }
        else if ((lead & 0xFE) == 0xFC) { more = 5; number = lead & 0x01; }
        else if (lead == 0xFE) { more = 6; number = 0; }
        else return false;
    }
    for (int i = 0; i < more; i++) {
        if (p >= n || (b[p] & 0xC0) != 0x80) return false;
        number = (number << 6) | (b[p++] & 0x3F);
    }
    if (blockCode == 1) frames = 192;
    else if (blockCode <= 5) frames = 576u << (blockCode - 2);
    else if (blockCode == 6) { if (p + 1 > n) return false; frames = (uint64_t)b[p] + 1; p += 1; }
    else if (blockCode == 7) { if (p + 2 > n) return false; frames = ((uint64_t)b[p] << 8 | b[p + 1]) + 1; p += 2; }
    else frames = 256u << (blockCode - 8);
    if (rateCode == 12) p += 1;
    else if (rateCode == 13 || rateCode == 14) p += 2;
    if (p + 1 > n || flacCrc8(b + at, p - at) != b[p]) return false;
    // dr_flac's numbering of fixed blocks (drflac__get_pcm_frame_range_of_current_flac_frame)
    first = variable ? number : number * maxBlock;
    return true;
}

// FLAC, as SoLoud's loadflac reads it: STREAMINFO's total is the length, and the frames are read
// 512 at a time up to it (patched: what never decodes is silence). The frames must reach that
// length: a frame header (searched from the end, its CRC-8 right) must hold the last of them. A
// cut file has none, so it is refused without decoding it; a file cut inside its last frame loses
// at most that frame to silence.
bool checkFlac(const unsigned char* b, uint64_t n, AudioDecode decode, AudioFileFormat& format, uint64_t maxSamples, std::string& error){
    format.codec = "flac";
    drflac* flac = drflac_open_memory(b, (size_t)n, nullptr);
    if (!flac) {
        error = "can't read this FLAC file's header";
        return false;
    }
    format.channels = flac->channels;
    format.sampleRate = flac->sampleRate;
    format.frames = flac->totalPCMFrameCount;
    const uint64_t firstFrameAt = flac->firstFLACFramePosInBytes;
    const unsigned int maxBlock = flac->maxBlockSizeInPCMFrames;
    drflac_close(flac);
    if (!checkChannelsAndRate(format, error)) {
        return false;
    }
    if (format.frames == 0) {
        error = "its length is not in its header (STREAMINFO counts 0 frames): re-encode it";
        return false;
    }
    if (!checkLength(format, decode, maxSamples, error)) {
        return false;
    }
    // a frame header that a frame's own bytes imitate (its sync code and CRC by chance) holds
    // other numbers: only the frame that holds the last one counts, wherever it is found
    for (uint64_t at = n >= 6 ? n - 5 : 0; at-- > firstFrameAt;) {
        uint64_t first = 0, frames = 0;
        if (b[at] == 0xFF && flacFrameAt(b, n, at, maxBlock, first, frames) && first < format.frames && first + frames >= format.frames) {
            return true;
        }
    }
    error = text("the FLAC file is cut or damaged: its header counts %llu frames, but no frame holds the last of them",
                 (unsigned long long)format.frames);
    return false;
}

// Anything else is tried as MP3, as SoLoud does. dr_mp3 counts the frames by their headers
// (drmp3_get_pcm_frame_count, without decoding them) and loadmp3 allocates that count.
bool checkMp3(const unsigned char* b, uint64_t n, AudioDecode decode, AudioFileFormat& format, uint64_t maxSamples, std::string& error){
    drmp3 mp3;
    if (!drmp3_init_memory(&mp3, b, (size_t)n, nullptr)) {
        error = "not a WAV, OGG, MP3 or FLAC file";
        return false;
    }
    format.codec = "mp3";
    format.channels = mp3.channels;
    format.sampleRate = mp3.sampleRate;
    format.frames = drmp3_get_pcm_frame_count(&mp3);
    drmp3_uninit(&mp3);
    return checkChannelsAndRate(format, error) && checkLength(format, decode, maxSamples, error);
}

}

bool AudioFileCheck::clampWavSizes(unsigned char* data, size_t length){
    WavSizes sizes;
    if (!planWavSizes(data, length, sizes)) {
        return false;
    }
    if (sizes.rf64) {
        std::memcpy(data, "RIFF", 4);
        std::memcpy(data + sizes.ds64At, "JUNK", 4);
    }
    writeU32(data + 4, sizes.riffSize);
    writeU32(data + sizes.dataSizeAt, sizes.dataSize);
    return true;
}

bool AudioFileCheck::wavSizesNeedClamping(const unsigned char* data, size_t length){
    WavSizes sizes;
    return planWavSizes(data, length, sizes);
}

bool AudioFileCheck::check(const unsigned char* data, size_t length, AudioDecode decode, AudioFileFormat& format, std::string& error,
                           uint64_t maxSamples){
    format = AudioFileFormat();
    error.clear();
    if (!data || length == 0) {
        error = "the file is empty";
        return false;
    }
    // Wav::loadMem and WavStream::loadMem take a 32-bit length
    if ((uint64_t)length > 0xFFFFFFFFull) {
        error = "larger than 4 GB";
        return false;
    }
    // SoLoud's dispatch (Wav::testAndLoadFile, WavStream::parse): the first four bytes, and
    // anything else is tried as MP3
    if (length >= 4 && std::memcmp(data, "OggS", 4) == 0) {
        return checkOgg(data, length, decode, format, maxSamples, error);
    }
    if (length >= 4 && std::memcmp(data, "RIFF", 4) == 0) {
        return checkWav(data, length, decode, format, maxSamples, error);
    }
    if (length >= 4 && std::memcmp(data, "fLaC", 4) == 0) {
        return checkFlac(data, length, decode, format, maxSamples, error);
    }
    // what SoLoud would try as MP3 and fail: clearer said as what it is
    if (length >= 12 && std::memcmp(data, "RF64", 4) == 0 && std::memcmp(data + 8, "WAVE", 4) == 0) {
        format.codec = "wav";
        error = "an RF64 WAV that can't be read as a RIFF one (its ds64 chunk is not first, or its sizes need 64 bits)";
        return false;
    }
    if (length >= 4 && std::memcmp(data, "riff", 4) == 0) {
        format.codec = "wav";
        error = "Sony Wave64 (W64) is not supported";
        return false;
    }
    return checkMp3(data, length, decode, format, maxSamples, error);
}

bool AudioFileCheck::prepare(unsigned char* data, size_t length, AudioDecode decode, AudioFileFormat& format, std::string& error,
                             uint64_t maxSamples){
    clampWavSizes(data, length);
    return check(data, length, decode, format, error, maxSamples);
}
