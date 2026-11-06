// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MUSICSEQUENCESOURCE_H
#define MUSICSEQUENCESOURCE_H

#include "Export.h"

#include "soloud.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace jasperengine{

    class AudioClipSource;
    class MusicSequenceSourceInstance;

    // One section of a music track (docs/specs/S39-music.md R2): a clip played from its trimmed
    // start to its loop end, its loop region repeated until it played `loops` passes (counting the
    // first; 0 = until Audio.advanceMusic), then its tail. Prepared on the main thread (the pick, the
    // clip, its instance, its gain) and handed to the audio thread.
    struct MusicSection{
        // held until the section is done: its instance reads this clip's samples
        std::shared_ptr<AudioClipSource> clip;
        std::unique_ptr<SoLoud::AudioSourceInstance> instance;
        // its number in the track (0 the first), and its clip's index in the event (-1: a clip track)
        int index = 0;
        int clipIndex = -1;
        unsigned int loops = 1;
        // linear: the event clip's volume_db (the clip's own gain is AudioClipSource's)
        float gain = 1.0f;
    };

    // The audio source of one music track's voice (R5). Its instance reads the current section's
    // AudioClipSource instance on the audio thread, counts its passes (that instance's LOOPING flag
    // while passes remain, SoLoud's wrap at the short read that ends a pass, the flag cleared for the
    // last pass so the tail and the clip's fade-out play) and switches to the next section at the
    // next frame: a hand-over is gapless at any frame rate and with any device period. The main
    // thread offers the next section through a single-slot atomic as soon as the current one began,
    // reads where each began through a lock-free single-producer queue, and deletes the sections
    // the audio thread is done with (nothing is allocated or freed in the mix). Channels and rate
    // are the first section's: MusicPlayer refuses a track whose sections differ.
    class JASPERENGINE_API MusicSequenceSource : public SoLoud::AudioSource{
        friend class MusicSequenceSourceInstance;

    public:
        // a section began: its number and the track frame (the source's rate, from the track's first
        // frame) of its first frame
        struct Report{
            int section = 0;
            uint64_t startFrame = 0;
        };

        // sections at least 100 ms long: far more than a frame's worth
        static constexpr unsigned int REPORT_CAPACITY = 64;
        static constexpr unsigned int RETIRED_CAPACITY = 16;
        static constexpr uint64_t NO_STOP = ~0ull;

        MusicSequenceSource(unsigned int channels, float sampleRate);
        ~MusicSequenceSource() override;

        SoLoud::AudioSourceInstance* createInstance() override;

        // main thread, before the voice starts: the section it begins with
        void setFirst(std::unique_ptr<MusicSection> section);
        // main thread: the section after the current one; false while the slot still holds the one
        // offered before
        bool offerNext(std::unique_ptr<MusicSection> section);
        // the slot is empty and the track goes on: the next section can be offered
        bool wantsNext() const;
        // no section follows the current one: the voice ends after it
        void endAfterCurrent();
        // the current section ends at the end of its pass in progress (Audio.advanceMusic)
        void requestAdvance();
        // the voice ends at this track frame exactly (a cut at a switch point)
        void stopAt(uint64_t trackFrame);
        // main thread: the oldest report not taken yet
        bool takeReport(Report& report);
        // main thread: deletes the sections the audio thread finished
        void collectRetired();
        // the voice played its last section to the end, or reached its stop frame
        bool hasEnded() const;

    private:
        std::unique_ptr<MusicSection> first;
        std::atomic<MusicSection*> next{nullptr};
        std::atomic<bool> endRequested{false};
        std::atomic<bool> advanceRequested{false};
        std::atomic<bool> ended{false};
        std::atomic<uint64_t> stopFrame{NO_STOP};
        // written by the audio thread (tail), read by the main thread (head)
        Report reports[REPORT_CAPACITY];
        std::atomic<unsigned int> reportHead{0};
        std::atomic<unsigned int> reportTail{0};
        // the finished sections, the same way round
        std::atomic<MusicSection*> retired[RETIRED_CAPACITY] = {};
        std::atomic<unsigned int> retiredHead{0};
        std::atomic<unsigned int> retiredTail{0};

        // audio thread
        void report(int section, uint64_t startFrame);
        void retire(MusicSection* section);
    };

    // The voice of a MusicSequenceSource (audio thread)
    class MusicSequenceSourceInstance : public SoLoud::AudioSourceInstance{
    public:
        explicit MusicSequenceSourceInstance(MusicSequenceSource* parent);
        ~MusicSequenceSourceInstance() override;

        unsigned int getAudio(float* buffer, unsigned int samplesToRead, unsigned int bufferSize) override;
        bool hasEnded() override;

    private:
        MusicSequenceSource* parent;
        std::unique_ptr<MusicSection> current;
        // passes of the current section so far, the track's frames so far
        unsigned int passes = 0;
        uint64_t trackFrames = 0;
    };

}

#endif //MUSICSEQUENCESOURCE_H
