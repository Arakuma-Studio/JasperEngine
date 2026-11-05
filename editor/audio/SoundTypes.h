// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <string>
#include <vector>

// The sound vocabulary of the editor's audio tools: every type,
// the kind of sound it is and its loudness target, and the thresholds of the analysis notes.
// One header so nothing defines its own copy.
namespace jasperengine::editor::SoundTypes {

    // What a sound does in time, measured from its bytes (AudioAnalysis' measured_kind) or given
    // by its type
    inline constexpr const char* TRANSIENT = "transient";
    inline constexpr const char* TONAL = "tonal";
    inline constexpr const char* SWELL = "swell";
    inline constexpr const char* RISER = "riser";
    inline constexpr const char* GLITCH = "glitch";
    inline constexpr const char* LOOP = "loop";
    // only from the voice type, never measured
    inline constexpr const char* SPEECH = "speech";

    struct SoundType {
        const char* name;
        const char* kind;
        // LUFS: momentary max (LUFS-M) for one-shot kinds, integrated for loop and speech
        double targetLufs;
        bool integratedTarget;
        // the sound stops by design: its end is a cut on purpose, so it never gets truncated_tail
        bool designedEnd;
    };

    // The 31 types. The targets of click, explosion, hurt and shoot sit below the usual value for
    // their kind: their generate_sound presets are short, peaky sounds that could not reach it at
    // their default params without passing -1 dBTP.
    inline const std::vector<SoundType>& all() {
        static const std::vector<SoundType> types = {
            {"click", TRANSIENT, -32.0, false, false},
            {"key", TRANSIENT, -26.0, false, false},
            {"tick", TRANSIENT, -28.0, false, false},
            {"tap", TRANSIENT, -22.0, false, false},
            {"pop", TRANSIENT, -22.0, false, false},
            {"thump", TRANSIENT, -16.0, false, false},
            {"hit", TRANSIENT, -14.0, false, false},
            {"boom", TRANSIENT, -14.0, false, false},
            {"paper", SWELL, -22.0, false, false},
            {"swish", SWELL, -20.0, false, false},
            {"whoosh", SWELL, -18.0, false, false},
            {"transition", SWELL, -16.0, false, false},
            {"riser", RISER, -16.0, false, false},
            {"reverse", RISER, -18.0, false, false},
            {"ambience", LOOP, -28.0, true, false},
            {"voice", SPEECH, -20.0, true, false},
            {"glitch", GLITCH, -20.0, false, true},
            {"ui", TONAL, -22.0, false, true},
            {"zap", TONAL, -18.0, false, true},
            {"chime", TONAL, -20.0, false, false},
            {"shimmer", TONAL, -20.0, false, false},
            {"footstep", TRANSIENT, -22.0, false, false},
            {"impact", TRANSIENT, -18.0, false, false},
            {"explosion", TRANSIENT, -18.0, false, false},
            {"pickup", TONAL, -18.0, false, true},
            {"jump", TONAL, -18.0, false, true},
            {"hurt", TRANSIENT, -24.0, false, false},
            {"shoot", TRANSIENT, -21.0, false, false},
            {"door", SWELL, -18.0, false, false},
            {"stinger", TONAL, -16.0, false, true},
            {"music", LOOP, -20.0, true, false},
        };
        return types;
    }

    // nullptr for an unknown (or empty) name
    inline const SoundType* find(const std::string& name) {
        for (const SoundType& type : all()) {
            if (name == type.name) return &type;
        }
        return nullptr;
    }

    // Types whose energy is meant to sit low: no low_end note for them
    inline bool isLowEndType(const std::string& name) {
        return name == "boom" || name == "explosion" || name == "thump" || name == "ambience" || name == "music";
    }

    // Types that stop by design (SoundType::designedEnd); false for an unknown or empty name
    inline bool isDesignedEnd(const std::string& name) {
        const SoundType* type = find(name);
        return type && type->designedEnd;
    }

    // -- silence and edges

    // A sample is sound from this level up (head and tail silence, the silent-file rule)
    inline constexpr double SILENCE_THRESHOLD_DBFS = -60.0;

    // -- notes

    inline constexpr double SILENT_PEAK_DBFS = -90.0;
    inline constexpr double TRUE_PEAK_MAX_DBTP = 0.0;
    inline constexpr double DC_OFFSET_MAX = 0.01;
    // share of the energy in the 31.5 and 63 Hz bands (below about 90 Hz)
    inline constexpr double LOW_END_SHARE_MAX = 0.40;
    inline constexpr double SOFT_ATTACK_MS = 25.0;
    // a riser peaks in its last 20%
    inline constexpr double RISER_PEAK_MIN_SHARE = 0.80;
    inline constexpr double LOOP_SEAM_STEP_MAX_DB = 12.0;
    inline constexpr double LOOP_SEAM_LEVEL_MAX_DB = 3.0;
    inline constexpr double LOUDNESS_TARGET_TOLERANCE_LU = 3.0;
    // the true peak a gain suggestion leaves room for
    inline constexpr double HEADROOM_DBTP = -1.0;
    inline constexpr double PHASE_CORRELATION_MIN = -0.3;

    // -- measured_kind, the first rule that matches

    inline constexpr double LOOP_MIN_SECONDS = 4.0;
    // the loudness range (EBU Tech 3342) of the short-term values
    inline constexpr double LOOP_RANGE_MAX_LU = 6.0;
    inline constexpr double RISER_MIN_SECONDS = 1.0;
    inline constexpr double RISER_PEAK_SHARE = 0.7;
    inline constexpr int GLITCH_MIN_ONSETS = 4;
    inline constexpr double GLITCH_MAX_SECONDS = 1.5;
    inline constexpr double GLITCH_MIN_FLATNESS = 0.3;
    inline constexpr double SWELL_ATTACK_MS = 50.0;
    inline constexpr double TONAL_MIN_CONFIDENCE = 0.6;

    // -- defects: the measurements of AudioAnalysis::defects
    // and the limits of their notes. Analysis-note limits like the ones above, not QA thresholds.

    // edges (2.2.1): an edge sample clicks above EDGE_MAX_DB re the played peak, from
    // SILENCE_THRESHOLD_DBFS up, unless it moves toward 0 fast enough to reach it within this
    // many steps at the edge's own slope (a zero crossing)
    inline constexpr double EDGE_MAX_DB = -40.0;
    inline constexpr double EDGE_ZERO_CROSSING_STEPS = 2.0;

    // clicks inside (2.2.2), on 1 ms frames of the second difference of the channel mean
    inline constexpr double CLICK_FRAME_MS = 1.0;
    // the context medians: frames k-10 .. k-3 before and k+3 .. k+10 after, at least 4 each
    inline constexpr int CLICK_CONTEXT_NEAR_FRAMES = 3;
    inline constexpr int CLICK_CONTEXT_FAR_FRAMES = 10;
    inline constexpr int CLICK_CONTEXT_MIN_FRAMES = 4;
    // the share of a frame's second-difference energy in its largest samples: a frame at or over
    // it is impulsive
    inline constexpr int CLICK_IMPULSE_SAMPLES = 4;
    inline constexpr double CLICK_IMPULSE_SHARE = 0.7;
    inline constexpr double CLICK_RISE_DB = 18.0;
    // A candidate that belongs to the waveform is no click:
    // - in an impulse texture: CLICK_TEXTURE_SHARE of the frames within +/- CLICK_TEXTURE_MS are
    //   impulsive (sfxr's sample-and-hold noise, a buzz, a pulse or square wave from about 50 Hz up);
    // - or where the sound itself repeats: another impulsive frame within +/- CLICK_RECUR_MS (at most
    //   CLICK_RISE_DB under it, not the frames next to it: the same event) has its impulse at a lag
    //   where the channel mean over +/- max(lag / 2, CLICK_REPEAT_MIN_HALF_MS) around the candidate's,
    //   both impulses' +/- CLICK_REPEAT_GUARD samples left out, correlates with its copy CLICK_REPEAT_MIN
    //   or more, the correlation peaking strictly inside lag +/- CLICK_REPEAT_SEARCH samples: the
    //   waveform's period (a click burst or an underrun pattern recurs at a lag the sound does not).
    // Never within CLICK_DROPOUT_GUARD samples of a dropout's edge: a run of CLICK_DROPOUT_MIN_SAMPLES
    // or more of digital silence in the channel mean that the gap rule (below) calls cut
    inline constexpr double CLICK_RECUR_MS = 50.0;
    inline constexpr int CLICK_RECUR_SAME_FRAMES = 1;
    inline constexpr double CLICK_TEXTURE_MS = 250.0;
    inline constexpr double CLICK_TEXTURE_SHARE = 0.05;
    inline constexpr double CLICK_REPEAT_MIN = 0.9;
    inline constexpr int CLICK_REPEAT_SEARCH = 3;
    inline constexpr int CLICK_REPEAT_GUARD = 2;
    inline constexpr double CLICK_REPEAT_MIN_HALF_MS = 2.5;
    inline constexpr int CLICK_DROPOUT_GUARD = 2;
    inline constexpr size_t CLICK_DROPOUT_MIN_SAMPLES = 2;
    inline constexpr double CLICK_STEADY_DB = 8.0;
    inline constexpr double CLICK_AUDIBLE_DB = 60.0;
    // frames this close to the region's edges (edges' own) or to the loudest frame (the
    // attack) are not judged
    inline constexpr double CLICK_GUARD_MS = 3.0;
    inline constexpr int CLICK_ATTACK_GUARD_FRAMES = 3;
    // crackle: clicks that make an irregular field through the sound (fire, rain, vinyl,
    // sparks) are its texture, and get no note: at least CRACKLE_MIN_CLICKS, CRACKLE_MIN_PER_SECOND,
    // spanning CRACKLE_MIN_SPREAD of the region, their intervals' coefficient of variation
    // CRACKLE_MIN_CV or more (an underrun pattern is regular; a burst spans a moment)
    inline constexpr int CRACKLE_MIN_CLICKS = 8;
    inline constexpr double CRACKLE_MIN_PER_SECOND = 0.5;
    inline constexpr double CRACKLE_MIN_SPREAD = 0.5;
    inline constexpr double CRACKLE_MIN_CV = 0.5;

    // truncated tail (2.2.3), on 5 ms frames of the channel mean aligned to the region's end
    inline constexpr double TAIL_FRAME_MS = 5.0;
    // judged with at least this many frames, and as many after the loudest
    inline constexpr int TAIL_MIN_FRAMES = 12;
    // the end level: the mean of the last 2 frames; under TAIL_CUT_MIN_DB the sound decayed
    inline constexpr int TAIL_END_FRAMES = 2;
    inline constexpr double TAIL_CUT_MIN_DB = -45.0;
    // the decay line: frames from 30 to 9 before the end (150 ms to 40 ms), at least 4
    inline constexpr int TAIL_FIT_FIRST_FRAMES = 30;
    inline constexpr int TAIL_FIT_LAST_FRAMES = 9;
    inline constexpr int TAIL_FIT_MIN_FRAMES = 4;
    // still decaying: the line falls faster than this
    inline constexpr double TAIL_DECAY_DB_PER_S = -20.0;
    // The release: 5 ms windows ending 0, 1, 2, ... ms before the end, back to the line's
    // last frame; the first within TAIL_RELEASE_DROP_DB of the line starts it. A raised-cosine
    // fade-out of T ms is that far under its start TAIL_RELEASE_SHARE x T before its end (at a
    // window's centre), so release_ms = (that time + half a window) / the share, about T for a fade
    // baked into the file; a region's own fade-out counts at its length. The tail is cut when it
    // still decays, ends at or above TAIL_CUT_MIN_DB and its release is under TAIL_RELEASE_MIN_MS;
    // a fade-out of TAIL_RELEASE_FIX_MS, the note's fix, always clears it, and a longer fade-out
    // never makes it worse (it lowers the end, moves the release back, and leaves the line alone)
    inline constexpr double TAIL_RELEASE_STEP_MS = 1.0;
    inline constexpr double TAIL_RELEASE_DROP_DB = 3.0;
    inline constexpr double TAIL_RELEASE_SHARE = 0.636;
    inline constexpr double TAIL_RELEASE_MIN_MS = 15.0;
    inline constexpr double TAIL_RELEASE_FIX_MS = 20.0;

    // digital-silence gaps (2.2.4) between the first and last sample within 60 dB of the peak: runs
    // at or under GAP_ZERO_DB re the played peak
    // that are dropouts, judged on each side against the sound's own level there. Each side has a far
    // window, GAP_NEAR_MS to GAP_FAR_MS from the sample next to the run, and a near window, that
    // sample and the ones beyond it over a quarter of the local period (pi / 2 over the far window's
    // sqrt(sum of squared differences / sum of squares)), clamped to GAP_NEAR_MS .. GAP_FAR_MS. A
    // side is cut when its sample is at least GAP_FLOOR_DBFS (not the last bits of a decay) and within
    // GAP_CUT_DB of the far window's largest |x| (a fade's last sample is far under it: a 0.5 ms
    // linear fade's 27.6 dB at 48 kHz); when both are, the run is a dropout. A tone cut at its zero
    // crossings is as low (a 250 Hz tone's 30 dB): both sides within GAP_ZERO_CROSSING_DB whose near
    // window's RMS is at most GAP_ZERO_CROSSING_ENV_DB under the far window's count too (a fade's is
    // lower). One cut side is enough when the sound goes on: both far windows within GAP_AUDIBLE_DB
    // of the peak and GAP_CONTINUITY_DB of each other (a new attack after a decay is not a dropout)
    inline constexpr double GAP_AUDIBLE_DB = -60.0;
    inline constexpr double GAP_ZERO_DB = -120.0;
    inline constexpr double GAP_MIN_MS = 2.0;
    inline constexpr double GAP_NEAR_MS = 0.5;
    inline constexpr double GAP_FAR_MS = 2.0;
    inline constexpr double GAP_CUT_DB = -25.0;
    inline constexpr double GAP_ZERO_CROSSING_DB = -35.0;
    inline constexpr double GAP_ZERO_CROSSING_ENV_DB = -1.0;
    inline constexpr double GAP_FLOOR_DBFS = -80.0;
    inline constexpr double GAP_CONTINUITY_DB = 10.0;

    // clicks and gaps listed by time
    inline constexpr int DEFECT_TIMES_LISTED = 8;

    // resonance and aliasing are measured on regions up to this long
    inline constexpr double SPECTRAL_DEFECTS_MAX_SECONDS = 10.0;

    // narrow resonance (2.2.5): the summed spectrum on a grid of 48 points per octave from
    // 100 Hz to min(16 kHz, 0.45 x rate), each point the largest bin within +/- 1/96 octave,
    // against the median of +/- 16 points (+/- 1/3 octave); candidates within 30 dB of the
    // largest point
    inline constexpr double RESONANCE_POINTS_PER_OCTAVE = 48.0;
    inline constexpr double RESONANCE_MIN_HZ = 100.0;
    inline constexpr double RESONANCE_MAX_HZ = 16000.0;
    inline constexpr double RESONANCE_MAX_RATE_SHARE = 0.45;
    inline constexpr int RESONANCE_NEIGHBOUR_POINTS = 16;
    inline constexpr double RESONANCE_RANGE_DB = 30.0;
    // the weight of a candidate's prominence by its frequency (the first band that holds it;
    // harshness is worst in 2-6 kHz)
    struct ResonanceWeight {
        double fromHz;
        double toHz;
        double weight;
    };
    inline constexpr ResonanceWeight RESONANCE_WEIGHTS[] = {{2000.0, 6000.0, 1.0}, {1000.0, 9000.0, 0.7}};
    inline constexpr double RESONANCE_OTHER_WEIGHT = 0.5;
    inline constexpr double RESONANCE_MAX_DB = 15.0;

    // aliasing (2.2.6, information), on its own frames: ALIASING_FFT_SIZE points every
    // ALIASING_HOP (side by side: the measurement is a median over them), a 4-term Blackman-Harris
    // window (sidelobes under -92 dB, so a harmonic's leakage never reads as a fold; a main lobe of
    // +/- 4 bins). In frames within 30 dB of the loudest, f0
    // is Phase 1's pitch of the frame's middle FFT_SIZE samples, refined from the frame's own
    // harmonics (least squares through their peaks: the lowest ALIASING_REFINE_FIRST, then all),
    // so the fold positions of high partials land where they are. The partials k f0 above Nyquist
    // up to 4 x rate fold back; a fold position counts in 4 kHz .. min(20 kHz, Nyquist), more than
    // ALIASING_HARMONIC_GUARD_BINS from every harmonic, when its largest bin within +/- 2 stands
    // 10 dB over the floor of +/- 16 bins: their ALIASING_FLOOR_PERCENTILE (their median, which
    // dense folds raise, missed every other fold of a naive 880 Hz saw at 44.1 kHz)
    inline constexpr int ALIASING_FFT_SIZE = 4096;
    inline constexpr int ALIASING_HOP = 4096;
    inline constexpr double ALIASING_RANGE_DB = 30.0;
    inline constexpr double ALIASING_MIN_HZ = 4000.0;
    inline constexpr double ALIASING_MAX_HZ = 20000.0;
    inline constexpr double ALIASING_MAX_RATE_MULTIPLE = 4.0;
    inline constexpr int ALIASING_HARMONIC_GUARD_BINS = 7;
    inline constexpr int ALIASING_PEAK_BINS = 2;
    inline constexpr int ALIASING_MEDIAN_BINS = 16;
    inline constexpr double ALIASING_PEAK_DB = 10.0;
    inline constexpr double ALIASING_FLOOR_PERCENTILE = 0.25;
    // a frame whose 4-20 kHz band holds less than this of its energy folds nothing audible (a pure
    // tone's PCM16 rounding lies on its own harmonic grid and would read as folds)
    inline constexpr double ALIASING_BAND_MIN_DB = -60.0;
    inline constexpr int ALIASING_REFINE_FIRST = 8;
    // a frame's folds count only when at least this share of its fold positions shows a peak (a
    // family of folds, not a partial that happens to sit at one), over every partial or
    // over the odd ones (a square's)
    inline constexpr double ALIASING_FAMILY_SHARE = 0.5;
    inline constexpr double ALIASING_INFO_SHARE = 0.05;

    // trim cuts (2.2.7): the largest |sample| within +/- 0.5 ms of the cut, re the file's peak
    inline constexpr double CUT_WINDOW_MS = 0.5;
    inline constexpr double TRIM_CUT_MAX_DB = -30.0;

    // onset.envelope_peak_ms: the peak of the 20 ms centred RMS envelope of the channel mean
    inline constexpr double ENVELOPE_PEAK_MS = 20.0;

    // the head silence an item's note starts at, ms
    inline constexpr double HEAD_SILENCE_MAX_MS = 15.0;

    // -- click-safe fades : what every proposal of a fade writes (the notes' fix,
    // the Inspector's button, the essentials' import defaults)

    // a short ramp keeps a transient's attack; a loop's end is its seam, so it gets no fade-out
    inline constexpr double CLICK_SAFE_FADE_IN_SHORT_MS = 0.5;
    inline constexpr double CLICK_SAFE_FADE_OUT_SHORT_MS = 5.0;
    inline constexpr double CLICK_SAFE_FADE_IN_MS = 2.0;
    inline constexpr double CLICK_SAFE_FADE_OUT_MS = 12.0;

    // The fade-in a note proposes is the longest raised cosine, up to the kind's length and down to
    // FADE_IN_MIN_FRAMES, that takes at most FADE_IN_MAX_LOSS_DB of the energy of the first
    // ATTACK_WINDOW_SECONDS (AudioAnalysis::safeFadeInFrames, SoundSynth's rule too): a sound that
    // starts at its loudest keeps its attack (0.5 ms took 1.3 LU of ui/tick). When even the
    // shortest takes more, FADE_IN_FLOOR_FRAMES: the 2-frame ramp of SoundSynth's lead-in, which
    // an importer fade can only lay over the first frames
    inline constexpr double FADE_IN_MAX_LOSS_DB = 0.05;
    inline constexpr double ATTACK_WINDOW_SECONDS = 0.4;
    inline constexpr size_t FADE_IN_MIN_FRAMES = 3;
    inline constexpr size_t FADE_IN_FLOOR_FRAMES = 2;

    inline double clickSafeFadeInMs(const std::string& kind) {
        return kind == TRANSIENT || kind == GLITCH ? CLICK_SAFE_FADE_IN_SHORT_MS : CLICK_SAFE_FADE_IN_MS;
    }

    // 0 for a loop
    inline double clickSafeFadeOutMs(const std::string& kind) {
        if (kind == LOOP) return 0.0;
        return kind == TRANSIENT || kind == GLITCH ? CLICK_SAFE_FADE_OUT_SHORT_MS : CLICK_SAFE_FADE_OUT_MS;
    }

    // -- design: what a seed varies in a layered preset (SoundDesign): the force an octave share and a
    // level, the timing a share and a delay, the pitch in semitones
    inline constexpr double DESIGN_FORCE_OCTAVES = 0.25;
    inline constexpr double DESIGN_FORCE_DB = 1.5;
    inline constexpr double DESIGN_TIMING = 0.08;
    inline constexpr double DESIGN_DELAY_MS = 1.0;
    inline constexpr double DESIGN_PITCH_ST = 0.5;

    // -- variety: two variations this far apart (SoundSearch::distance) read as two different sounds,
    // not takes of one
    inline constexpr double VARIETY_WIDE_DISTANCE = 0.5;
    // two seeds of one preset under this are the same take: a preset's seeds lie much closer than an
    // event's clips do (NEAR_DUPLICATE_DISTANCE), 0.003 to 0.218 at their nearest over its 16 families
    inline constexpr double VARIETY_NEAR_DUPLICATE_DISTANCE = 0.020;

    // -- fit: generate_sound like (SoundFit). The scales make each term of the distance 1 at one clearly
    // audible step: 3 dB of balance, 6 dB of envelope, 19% of length, twice or half the attack.
    inline constexpr int FIT_RENDERS = 64;
    inline constexpr int FIT_STAGE_A_RENDERS = 16;
    inline constexpr double FIT_MAX_SECONDS = 8.0;
    inline constexpr double FIT_BAND_FLOOR_DB = -40.0;
    inline constexpr double FIT_BAND_SCALE_DB = 3.0;
    inline constexpr double FIT_ENVELOPE_SCALE_DB = 6.0;
    inline constexpr double FIT_LENGTH_SCALE = 4.0;
    inline constexpr double FIT_CLOSE = 1.0;
    inline constexpr double FIT_SIMILAR = 2.0;

    // -- slice: edit_sound_event slice (SoundSlicer)
    inline constexpr double SLICE_TAIL_DB = -50.0;
    inline constexpr double SLICE_MIN_MS = 30.0;
    inline constexpr int SLICE_MAX = 64;
    inline constexpr double SLICE_MAX_SOURCE_SECONDS = 120.0;
    inline constexpr uint64_t SLICE_MAX_BYTES = 32ull * 1024 * 1024;

    // -- Sound Event problems

    // two clips of an event whose search_sounds similar_to distance is under this sound like one
    // clip: half the smallest distance between the essentials' footstep takes (concrete_1/_2
    // 0.748, grass_1/_2 0.409, wood_1/_2 0.150), so a real variation set is never called a clone
    //
    inline constexpr double NEAR_DUPLICATE_DISTANCE = 0.075;
    // a layer this far or more under the energy sum of the other layers in every band where any
    // layer is above SILENCE_THRESHOLD_DBFS adds nothing measurable
    inline constexpr double LAYER_INAUDIBLE_DB = 12.0;

    // -- repetition: measurement
    // definitions of audio_session check's repetition, not thresholds (those are audio.qa.*)

    // plays whose pitches and gains spread less than these are one pitch and one level to a
    // listener (below what an ear tells apart)
    inline constexpr double SAME_PITCH_SEMITONES = 0.1;
    inline constexpr double SAME_GAIN_DB = 0.5;

    // The UI click is the exception: one voice, one level. Types never judged for repetition.
    inline bool isRepetitionExempt(const std::string& type) {
        return type == "click" || type == "key" || type == "tick" || type == "ui";
    }

    // -- differences: what analyze_audio's
    // differences_vs_1 and the Sound Browser's Compare with selected report, in this order, at most
    // DIFFERENCES_LISTED. Each is reported when the two items differ by at least its limit (a ratio
    // either way, or a difference in dB / LU): hearing-sized steps, measurement definitions.

    inline constexpr int DIFFERENCES_LISTED = 4;
    // momentary max (integrated for the loop and speech kinds), LU
    inline constexpr double DIFFERENCE_LOUDNESS_LU = 1.5;
    // centroid_hz
    inline constexpr double DIFFERENCE_BRIGHTNESS_RATIO = 1.15;
    // onset.attack_ms, and at least DIFFERENCE_ATTACK_MIN_MS apart
    inline constexpr double DIFFERENCE_ATTACK_RATIO = 1.5;
    inline constexpr double DIFFERENCE_ATTACK_MIN_MS = 3.0;
    // the envelope peak to the last sample at or above SILENCE_THRESHOLD_DBFS
    inline constexpr double DIFFERENCE_TAIL_RATIO = 1.5;
    // the share of the energy of all ten octave bands in the first DIFFERENCE_LOW_END_BANDS (31.5, 63
    // and 125 Hz), dB: the sound's balance, so a copy at another gain has the same low end
    inline constexpr double DIFFERENCE_LOW_END_DB = 3.0;
    inline constexpr int DIFFERENCE_LOW_END_BANDS = 3;
    // the sounding length: the first to the last sample at or above SILENCE_THRESHOLD_DBFS
    inline constexpr double DIFFERENCE_LENGTH_RATIO = 1.25;
    // rolloff_hz
    inline constexpr double DIFFERENCE_BANDWIDTH_RATIO = 1.4;

}
