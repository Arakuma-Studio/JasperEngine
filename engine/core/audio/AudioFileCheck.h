// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef AUDIOFILECHECK_H
#define AUDIOFILECHECK_H

#include "Export.h"

#include <cstddef>
#include <cstdint>
#include <string>

// soloud.h defines MAX_CHANNELS as a macro: keep it out of this header's own constant (a file that
// includes soloud.h first and then names AudioFileCheck::MAX_CHANNELS must #undef it)
#pragma push_macro("MAX_CHANNELS")
#undef MAX_CHANNELS

namespace jasperengine{

    // How SoLoud will decode the bytes
    enum class AudioDecode{
        // SoLoud::Wav: the whole file into one buffer sized from the header's length
        Whole,
        // SoLoud::WavStream: block by block, as the voice plays (Streaming, Compressed In Memory)
        Stream
    };

    // What the decoders read from a file's headers (check() fills it as far as it got)
    struct AudioFileFormat{
        // "wav", "ogg", "mp3", "flac"; empty when the bytes are none of them
        std::string codec;
        unsigned int channels = 0;
        unsigned int sampleRate = 0;
        // the length SoLoud will use: the buffer it allocates for a whole decode
        uint64_t frames = 0;
        // refused for its length alone (frames * channels over the limit given)
        bool tooLong = false;
    };

    // The one check every audio load goes through before SoLoud sees the bytes (SoundPool: games,
    // the editor's Play, edit mode and the Inspector preview; decoded, compressed in memory and
    // streamed clips).
    //
    // SoLoud's decoders trust the headers they read: WAV decodes 512 frames at a time into a
    // stack buffer of 512 * 8 floats whatever the channel count (more than 8 overflow it), OGG and
    // FLAC allocate the length their headers give (the vendored loaders are patched to write no
    // more than that and to leave what doesn't decode silent), stb_vorbis allocates a comment
    // header's lengths before it reads them, dr_wav divides by a frame size it checks only
    // afterwards, and every decoder truncates frames * channels to 32 bits. So the check reads the
    // headers with the same decoders, and the structure the length depends on (Ogg pages, the last
    // FLAC frame, ADPCM block headers) without decoding the audio, and refuses with a reason any
    // file SoLoud cannot decode safely and exactly; it never changes the bytes. clampWavSizes runs
    // first.
    class JASPERENGINE_API AudioFileCheck{
    public:
        // SoLoud's MAX_CHANNELS (its decode and mix buffers)
        static constexpr unsigned int MIN_CHANNELS = 1;
        static constexpr unsigned int MAX_CHANNELS = 8;
        // dr_wav's own limit at the top; anything below 1 kHz is no audio file a game ships
        static constexpr unsigned int MIN_SAMPLE_RATE = 1000;
        static constexpr unsigned int MAX_SAMPLE_RATE = 384000;
        // the most a whole decode holds: 2^30 floats (4 GiB, 3.1 hours of 48 kHz stereo). SoLoud
        // counts samples in 32 bits, so this also keeps frames * channels from wrapping.
        static constexpr uint64_t MAX_DECODED_SAMPLES = 1ull << 30;
        // a stream's length is a 32-bit frame count in WavStream
        static constexpr uint64_t MAX_STREAM_FRAMES = 0xFFFFFFFFull;

        // A WAV whose sizes run past its bytes, as ffmpeg and sox write them to a pipe
        // (0xFFFFFFFF) or as a cut file has them: SoLoud's dr_wav takes the data chunk's size as
        // the length, so a 1 s file would decode as 12 hours. The data chunk's size becomes the
        // bytes present (whole frames) and the RIFF size the file's; an RF64 file (ds64 sizes, 0
        // when streamed), which SoLoud does not read, becomes a plain RIFF one when its sizes fit
        // in 32 bits. Other bytes (OGG, MP3, FLAC, a WAV already right) are left alone. True when
        // it changed them.
        static bool clampWavSizes(unsigned char* data, size_t length);
        // whether clampWavSizes would change these bytes (a reader of const bytes copies only then)
        static bool wavSizesNeedClamping(const unsigned char* data, size_t length);

        // True when SoLoud can decode these bytes (already clamped) safely, as `decode` says.
        // Otherwise error is the reason, for a person ("12 channels (1-8 are supported)"). A
        // whole decode is also refused over maxSamples (frames * channels), from the headers
        // before any decoding. No format's audio is decoded: the load that follows is the only decode.
        static bool check(const unsigned char* data, size_t length, AudioDecode decode, AudioFileFormat& format,
                          std::string& error, uint64_t maxSamples = MAX_DECODED_SAMPLES);

        // clampWavSizes, then check: for a loader that owns its bytes
        static bool prepare(unsigned char* data, size_t length, AudioDecode decode, AudioFileFormat& format,
                            std::string& error, uint64_t maxSamples = MAX_DECODED_SAMPLES);
    };

}

#pragma pop_macro("MAX_CHANNELS")

#endif /* AUDIOFILECHECK_H */
