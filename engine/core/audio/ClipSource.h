// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef CLIPSOURCE_H
#define CLIPSOURCE_H

#include "Export.h"
#include "AudioImport.h"

#include "soloud.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace SoLoud{
    class Wav;
    class WavStream;
}

namespace jasperengine{

    class AudioClipSourceInstance;

    // A sound file as the engine plays it with its import settings: it wraps the pool's sample of
    // the file (a SoLoud::Wav, read in place, or a WavStream or EngineWavStream, each voice with its
    // decoder). Every voice plays the region [start frame, end frame) of the file (the trims), at the
    // clip's gain, with its fades, and a looping voice wraps from the loop end to its loop point.
    // Seeks land on frame floor(seconds x rate) of that region.
    // Fades are applied by each voice as it reads, never baked into the shared samples, because
    // fades never shape a wrap: the fade-in only before the first wrap, the fade-out only on a
    // pass that ends the region. A clip with default settings is not wrapped at all (SoundPool).
    class JASPERENGINE_API AudioClipSource : public SoLoud::AudioSource{
        friend class AudioClipSourceInstance;

    public:
        // sample: a Wav or a WavStream. oggHead: the first bytes of an Ogg file, to tell a stream
        // whose sample positions run behind its decode (a part cut with its packets), or null.
        AudioClipSource(std::shared_ptr<SoLoud::AudioSource> sample, const ClipImportSettings& settings,
                   const unsigned char* oggHead = nullptr, size_t oggHeadBytes = 0);
        ~AudioClipSource() override;

        const std::shared_ptr<SoLoud::AudioSource>& getSample() const;
        const ClipImportSettings& getImportSettings() const;
        // the region a voice plays (the trims applied), seconds
        double getLength() const;
        // the whole file, seconds
        double getFileLength() const;
        // a voice's loop point when its component gives none: the loop start, in seconds from
        // the region's start (0 without a loop start)
        double getDefaultLoopPoint() const;

        unsigned int getChannels() const;
        float getSampleRate() const;
        // the file's frames, and the region's (frames from the file's first frame)
        uint32_t getFileFrames() const;
        uint32_t getFrames() const;
        uint32_t getStartFrame() const;
        uint32_t getEndFrame() const;
        uint32_t getLoopStartFrame() const;
        uint32_t getLoopEndFrame() const;
        // the fades as applied: the settings' frames, each clamped to the region's length
        uint32_t getFadeInFrames() const;
        uint32_t getFadeOutFrames() const;

        SoLoud::AudioSourceInstance* createInstance() override;

    private:
        std::shared_ptr<SoLoud::AudioSource> sample;
        ClipImportSettings settings;
        // the sample held decoded (planar, fileFrames per channel), else streamed
        const float* samples = nullptr;
        SoLoud::WavStream* stream = nullptr;

        uint32_t fileFrames = 0;
        uint32_t startFrame = 0;
        uint32_t endFrame = 0;
        uint32_t loopStartFrame = 0;
        uint32_t loopEndFrame = 0;
        // applied when gainDb is not 0
        float gain = 1.0f;
        uint32_t fadeInFrames = 0;
        uint32_t fadeOutFrames = 0;
        // clipFadeWeight of each fade's frames, made at load (never in the mix)
        std::vector<float> fadeInWeights;
        std::vector<float> fadeOutWeights;
        // an Ogg cut out of a longer stream whose granules run behind its decode: seeks decode from
        // the start (stb_vorbis seeks by granule)
        bool seekByDecoding = false;
        // a seek costs a decode up to the frame (MP3, FLAC without a seek table, a cut Ogg): a
        // looping voice keeps a second decoder at its loop point
        bool seekDecodes = false;

        // the region, loop points and fades from the settings, clamped to the file
        void applyRegion();
    };

    // A voice of a AudioClipSource
    class AudioClipSourceInstance : public SoLoud::AudioSourceInstance{
    public:
        explicit AudioClipSourceInstance(AudioClipSource* parent);
        ~AudioClipSourceInstance() override;

        unsigned int getAudio(float* buffer, unsigned int samplesToRead, unsigned int bufferSize) override;
        bool hasEnded() override;
        SoLoud::result seek(SoLoud::time seconds, float* mixScratch, unsigned int mixScratchSize) override;
        SoLoud::result rewind() override;

        // the frames a looping voice's spare decoder may advance by per read: catching up a loop point
        // 32 times the loop's length into the clip within one pass (MP3 decodes this in about 0.25 ms)
        static constexpr uint32_t SPARE_FRAMES_PER_READ = 16384;

    private:
        AudioClipSource* parent;
        // the file frame the next read starts at
        uint32_t position;
        // Stream: this voice's decoder, and the block it reads into
        std::unique_ptr<SoLoud::AudioSourceInstance> stream;
        float scratch[SAMPLE_GRANULARITY * 8];
        // the decoder gave fewer frames than the file's headers count: the rest of the region is silence
        bool exhausted = false;
        // When a seek decodes: a second decoder, moved toward the loop point a little on each read
        // while the voice loops, so the wrap swaps decoders instead of decoding up to the loop
        // point inside the mix. spareAt is the frame it reads next; it rewinds to the file's start
        // (cheap) when it is past.
        std::unique_ptr<SoLoud::AudioSourceInstance> spare;
        uint32_t spareAt = 0;
        bool spareBroken = false;

        void streamTo(uint32_t frame);
        // the file frame a wrap seeks to (mLoopPoint, as seek computes it)
        uint32_t loopFrame() const;
        void advanceSpare();

        // The last read gave fewer frames than it was asked for: it stopped at its limit. A seek of
        // a looping voice now is SoLoud's wrap, which seeks to the loop point only then (and a
        // script's seek between two mixes follows a full read).
        bool shortRead = false;
        // the last read stopped at the loop end of a looping voice: the next read wraps (a read
        // that fills its block up to the loop end leaves it there until the next mix)
        bool atLoopEnd = false;
        // the voice has wrapped: its fade-in is over for good
        bool wrapped = false;

        // a looping voice before its loop end (or stopped at it) reads up to it, then SoLoud seeks
        // to its loop point; any other read goes to the region's end
        bool readsToLoopEnd() const;
        // where a read stops
        uint32_t readLimit() const;
        // the fades over the `count` frames just read into buffer from `position`
        void applyFades(float* buffer, unsigned int count, unsigned int bufferSize, bool toLoopEnd);
    };

}

#endif /* CLIPSOURCE_H */
