// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "audio/AudioImport.h"
#include "audio/AudioMeter.h"

#include "json.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    // Files and captures to numbers: the format, loudness and peaks (AudioMeter), bands, spectral
    // features, onsets, silence, stereo, the loop seam, pitch and a measured kind, as the JSON item
    // analyze_audio and get_asset_info return (docs/mcp/guide/sound.md). Plain functions of the
    // bytes, no audio device, safe on any thread; the editor's tools run them on a worker. Items of
    // files are cached by the SHA-1 of their bytes, and the import settings' hash for a clip as it
    // plays.
    class AudioAnalysis {
    public:
        using Json = nlohmann::json;

        // Bump whenever any value changes for the same input: the cache file names carry it.
        static constexpr int VERSION = 1;
        // Files larger than this are refused (an hour of 48 kHz stereo float WAV is about 1.4 GB)
        static constexpr uint64_t MAX_FILE_BYTES = 512ull * 1024 * 1024;
        // Files that decode to more samples (all channels) than this are refused before they are
        // decoded: 20 minutes of 48 kHz stereo. The analysis holds the decoded signal and a mono
        // downmix (6 bytes a stereo frame), so this bounds its memory to about 700 MB and its
        // time to well under the tools' 600 s limit; a 512 MB OGG or MP3 can hold hours.
        static constexpr uint64_t MAX_DECODED_SAMPLES = 20ull * 60 * 48000 * 2;
        static constexpr double MAX_DECODED_MINUTES = 20.0;

        // The spectral frames: 2048-point Hann windows every 512 samples of the mono downmix
        static constexpr int FFT_SIZE = 2048;
        static constexpr int FFT_HOP = 512;

        // A decoded signal, planar: channel c's frame i is samples[c * frames + i]
        struct Signal {
            unsigned int channels = 0;
            unsigned int sampleRate = 0;
            size_t frames = 0;
            std::vector<float> samples;
            // samples (all channels) that were NaN or infinite where the signal came from; decode
            // and readCaptureAudio set them to 0
            uint64_t nonFinite = 0;

            const float* channel(unsigned int c) const { return samples.data() + (size_t)c * frames; }
            double seconds() const { return sampleRate > 0 ? (double)frames / sampleRate : 0.0; }
            // the channel mean
            std::vector<float> mono() const;
        };

        // The region of a file a clip plays: its frames
        // [startFrame, endFrame) after the trims, at gainDb, with raised-cosine fades over its
        // first fadeInFrames and last fadeOutFrames (AudioClipSource's, AudioImport.h's clipFadeWeight).
        // A default value is the whole file as it is.
        struct Region {
            // endFrame for the file's end, whatever its length
            static constexpr uint32_t FILE_END = UINT32_MAX;

            uint32_t startFrame = 0;
            uint32_t endFrame = FILE_END;
            uint32_t fadeInFrames = 0;
            uint32_t fadeOutFrames = 0;
            float gainDb = 0.0f;

            // The region a clip with these import settings plays from a file of fileFrames: the
            // trims (endFrame stays FILE_END without a tail trim; trims that leave nothing leave an
            // empty region at the trimmed start, as AudioClipSource's), the gain, and the fades clamped
            // to the region's length as AudioClipSource clamps them
            static Region of(const ClipImportSettings& settings, uint32_t fileFrames);
            // The file as it is: no trims, no gain, no fades (its defects are the cached ones)
            bool isWholeFile() const;
            // the end frame in a file of fileFrames, and the fades as they apply there
            uint32_t end(size_t fileFrames) const;
            uint32_t fadeIn(size_t fileFrames) const;
            uint32_t fadeOut(size_t fileFrames) const;
            // {start_frame, end_frame, fade_in_frames, fade_out_frames, gain_db}, the end and the
            // fades as they apply in a file of fileFrames
            Json toJson(size_t fileFrames) const;
            // the same region of a file of fileFrames
            bool sameAs(const Region& other, size_t fileFrames) const;
        };

        // The signal a region of a decoded file plays (sample by sample as AudioClipSource makes it:
        // the trims, the gain and the fades), for measurements of what a voice played
        static Signal played(const Signal& file, const Region& region);
        // a real signal's band [low, high) Hz, in place: `buffer` holds `size` (a power of two) complex
        // values interleaved (re, im; the signal in re, zero-padded), every FFT bin outside the band (and its
        // mirror) is zeroed, and the band comes back in re
        static void bandPass(std::vector<float>& buffer, size_t size, double rate, double low, double high);

        // The container and encoding as read from the bytes: codec wav / ogg / mp3 /
        // flac; encoding pcm8..pcm32, float32, float64 (and the other WAV tags dr_wav reads),
        // vorbis, mp3 or flac
        struct Format {
            std::string codec;
            std::string encoding;
        };

        // What a file is beyond its bytes: its importer type, S25 labels and licence (its .meta, or
        // a built-in manifest entry) and a kind given by a built-in manifest entry. Empty: nothing
        // known, the kind is measured.
        struct Labels {
            std::string type;
            std::string manifestKind;
            Json tags;       // null or an array
            Json licence;    // null or an object
            // a starter library clip: its curated notes read as information (fileItem)
            bool library = false;
        };

        // A capture session on disk (S35): <root>/<id>/{session.json, master.wav, events.jsonl}
        struct Capture {
            std::string id;
            std::filesystem::path directory;
            Json session;
            // capturing (the live session), complete or aborted (left capturing by a crash)
            std::string state;
            bool live = false;
        };

        static bool probe(const std::string& bytes, Format& format, std::string& error);
        // Decodes WAV (RIFF or RF64), OGG Vorbis, MP3 or FLAC with SoLoud::Wav::loadMem (no device).
        // A WAV's sizes are clamped to its bytes first (AudioFileCheck::clampWavSizes: a WAV
        // streamed to a pipe says 0xFFFFFFFF), and AudioFileCheck::check refuses what SoLoud can't
        // decode safely (more than 8 channels, a cut OGG or FLAC, ...). A file that decodes to more
        // than MAX_DECODED_SAMPLES, or whose rate is outside AudioMeter's, is refused from its
        // headers before it is decoded (the lengths the decoders read: WAV data, FLAC STREAMINFO,
        // the last Ogg granule, MP3 frames). NaN and infinite samples become 0 and are counted in
        // signal.nonFinite.
        static bool decode(const std::string& bytes, Signal& signal, std::string& error);
        // The same, freeing `bytes` as soon as SoLoud has decoded them, before the signal is
        // copied out: the bytes, SoLoud's copy and the signal are never held at once
        static bool decodeReleasing(std::string& bytes, Signal& signal, std::string& error);

        // The measurements, full detail: non_finite, loudness, peak, rms_dbfs, dc_offset,
        // bands_db, band_share, sub_share, centroid_hz, flatness, rolloff_hz, onset, silence,
        // stereo, loop, pitch, measured_kind, onsets_ms, momentary, short_term. File mode pads
        // the momentary windows and flushes the true-peak filter; Stream mode meters as a
        // capture does, so a whole capture gives its session.json values.
        static Json measure(const Signal& signal, AudioMeterMode mode);
        // The same, also filling `spectrum` with the power spectra |X_k|^2 of every spectral frame of
        // the mean summed over the frames (FFT_SIZE / 2 + 1 values): the whole file's defects take
        // it for their resonance, so a file is transformed once
        static Json measure(const Signal& signal, AudioMeterMode mode, std::vector<double>* spectrum);
        // {seam_jump, seam_step_db, seam_level_db} of a voice wrapping from frame end - 1 to frame start
        static Json loopSeam(const Signal& signal, size_t start, size_t end);
        // The classification from the measurements (the first rule that matches)
        static std::string measuredKind(const Json& measured, double durationSeconds);

        // The defects of a file's played region: edges, clicks,
        // tail, gaps, resonance and aliasing (null beyond SoundTypes::SPECTRAL_DEFECTS_MAX_SECONDS),
        // the cut levels of its trims, and the region measured. Null for a silent region or a
        // file under 2 frames; trims that leave under 2 frames give {region, empty: true, frames}
        // (it plays nothing). Every threshold and window is SoundTypes.h's (`-- defects`).
        static Json defects(const Signal& signal, const Region& region);
        // The same with the whole file's summed spectrum from measure() (used for the whole-file
        // region only, where the played mean is the file's; null: computed)
        static Json defects(const Signal& signal, const Region& region, const std::vector<double>* spectrum);
        // A clip as the engine plays it (R3): the measurements of the region its import settings
        // leave (gain, trims and fades applied; the loop seam at the loop region's wrap), with
        // `played` (the region and its length), `source` (the raw file's numbers) and the raw
        // `format`; `cached` itself for settings that leave the samples as they are. Through the
        // file <sha1>-i<hash>-v<N>.json; an essential has no file: pass its decoded signal.
        static bool analyzePlayed(const std::filesystem::path& file, const std::filesystem::path& cacheDirectory, const Json& cached,
                                  const ClipImportSettings& settings, Json& out, std::string& error, const Signal* decoded = nullptr);
        // A project clip through the cache: analyzeFile, then its .meta's import settings resolved and
        // analyzePlayed. Settings the file cannot play give the file's numbers with `import_error`.
        // decoded, when given, receives the signal as it plays.
        static bool analyzeClip(const std::filesystem::path& file, const std::filesystem::path& cacheDirectory, Json& out,
                                std::string& error, Signal* decoded = nullptr);
        // The longest raised-cosine fade-in (clipFadeWeight) of at most `longest` frames, and at
        // least SoundTypes::FADE_IN_MIN_FRAMES, that takes at most FADE_IN_MAX_LOSS_DB of the
        // energy of the first ATTACK_WINDOW_SECONDS; 0 when even the shortest takes more.
        // squares[k] is frame k's energy (the sum over the channels of its squares), for the first
        // ATTACK_WINDOW_SECONDS of the sound or all of it when shorter. SoundSynth's fade-in and the
        // notes' click-safe fade-in.
        static size_t safeFadeInFrames(const std::vector<double>& squares, double rate, size_t longest);
        // Does an edge sample v (d its outward step) click against silence: loud against the file's
        // peak, audible on its own and no zero crossing (SoundTypes.h's `-- defects`)
        static bool edgeSampleClicks(double v, double d, double peak);

        // The level a trim cuts into at `frame`: the largest |sample| over the
        // channels within SoundTypes::CUT_WINDOW_MS of it, in dB re the file's sample peak
        // (AudioMeter::SILENCE_DB for a silent file)
        static double cutLevelDb(const Signal& file, uint32_t frame);

        // The cached content of a file's bytes: analyzer_version, sha1, format, defects and measure()
        static bool analyzeBytes(const std::string& bytes, Json& cached, std::string& error, Signal* decoded = nullptr);

        // The head and tail silence of a signal in ms, measure()'s "silence": {head_ms, tail_ms}
        static Json silence(const Signal& signal);

        // A file's silence through the cache in cacheDirectory: the cached analysis's when the
        // cache has the file, else measured from its bytes (read by the caller) with a decode and
        // silence() alone, and nothing written: the whole analysis of a long file takes seconds,
        // and a Play or an export resolves every `auto` trim (AudioImportSettings::resolve)
        static bool silenceOfFile(const std::filesystem::path& file, const std::string& bytes,
                                  const std::filesystem::path& cacheDirectory, Json& silence, std::string& error);

        // A file through the cache in cacheDirectory: the SHA-1 of the bytes (remembered by path,
        // size and modification time), then the cache file or a new analysis written to it.
        // decoded, when given, receives the signal (decoding a cached file again).
        static bool analyzeFile(const std::filesystem::path& file, const std::filesystem::path& cacheDirectory,
                                Json& cached, std::string& error, Signal* decoded = nullptr);

        // The item of a cached file (analyzeFile or analyzePlayed) as the tools return it: n, path,
        // the kind and kind_source resolved from labels, type / labels / licence, notes (head_silence
        // above headSilenceMaxMs); summary detail leaves out onsets_ms, momentary and short_term.
        static Json fileItem(const Json& cached, int n, const std::string& path, const Labels& labels, bool full,
                             double headSilenceMaxMs);
        // A starter library clip's notes as the curation explains them (labels.library):
        // loudness_target under the target says, as information, that the library's gain stopped at
        // the true-peak ceiling, then what fits
        static void curatedNotes(Json& item);
        // The labels of an asset file from its .meta (type, S25 labels, licence), with the kind of
        // the built-in clip it came from; empty without a .meta
        static Labels labelsOf(const std::filesystem::path& file);
        // kind and kind_source: the manifest's kind, else the type's, else measured
        static void resolveKind(const std::string& measuredKind, const Labels& labels, std::string& kind, std::string& kindSource);
        // The notes for an item ({id, text}, in table order, after as_it_plays and non_finite);
        // head_silence above headSilenceMaxMs. Then the defect notes of the item's defects
        // (edge_click, clicks (not crackle), truncated_tail, silence_gap, harsh_resonance, aliasing,
        // trim_cut; empty_region alone for trims that leave nothing), as the kind and the type's
        // designedEnd allow, with `fix` (the import settings keys that fix it) where a fade does.
        // A capture gets only the level notes (non_finite, silent, clipping, true_peak,
        // dc_offset, low_end, phase).
        static Json notes(const Json& item, const std::string& kind, const std::string& type, double headSilenceMaxMs,
                          bool capture = false);
        // "head silence 22.0 ms; ..." from the notes: each note's headlines, the clauses (between
        // "; ") that have a colon, each up to it, joined by " and "
        static std::string notesSummary(const Json& notes);

        // <project>/.jasperengine/cache/audio and <dir>/<sha1>[-i<hash>]-v<VERSION>.json (importHash 0: none)
        static std::filesystem::path cacheDirectory(const std::filesystem::path& projectDirectory);
        static std::filesystem::path cacheFile(const std::filesystem::path& cacheDirectory, const std::string& sha1, uint64_t importHash = 0);
        static std::string hashText(uint64_t hash);
        static bool readCache(const std::filesystem::path& cacheDirectory, const std::string& sha1, Json& cached, uint64_t importHash = 0);
        // through io::writeFileAtomic, under the name its sha1 and import_hash give
        static bool writeCache(const std::filesystem::path& cacheDirectory, const Json& cached);

        // -- captures

        // <project>/.jasperengine/audio/captures
        static std::filesystem::path captureRoot(const std::filesystem::path& projectDirectory);
        // YYYYMMDD-HHMMSS-NN
        static bool isSessionId(const std::string& name);
        // The sessions under root, oldest first. A session.json still "capturing" is the live
        // session when its id is liveSession (AudioCapture::sessionId(), "" when no
        // capture runs); otherwise it was left by a crash and reads "aborted".
        static std::vector<Capture> listCaptures(const std::filesystem::path& root, const std::string& liveSession);
        // "current" (the live session), "last" (the newest one that is not live) or a session id
        static bool findCapture(const std::filesystem::path& root, const std::string& reference, const std::string& liveSession,
                                Capture& capture, std::string& error);
        // master.wav as far as its header says it is flushed (float32, any channel count). NaN
        // and infinite samples are counted in signal.nonFinite and left as read: captureItem
        // sets those of its range to 0
        static bool readCaptureAudio(const std::string& bytes, Signal& signal, std::string& error);
        // The item of a capture's audio over [start, end) seconds (end < 0: to its end), measured
        // in Stream mode from the range's first sample; range receives the frames used (audio
        // itself when the range is all of it, so pass it with std::move). The audio of a
        // truncated session stops at its cap: a range reaching past it is measured up to it
        // and gets the `truncated` note.
        static bool captureItem(const Capture& capture, Signal audio, uint64_t bytes, double start, double end,
                                bool full, Json& item, Signal& range, std::string& error);
        // The capture's length in samples: session.json's `samples` (which go on after a
        // truncation cap), else the audio's frames
        static uint64_t captureSamples(const Capture& capture, const Signal& audio);

        // -- shared helpers

        // Number of spectral frames of n samples (the last one zero-padded), and the magnitudes
        // |X_k|, k = 0..FFT_SIZE/2, of frame `index` (out holds FFT_SIZE/2 + 1 values)
        static size_t spectralFrames(size_t samples);
        static void magnitudes(const float* mono, size_t samples, size_t index, float* out);

        // a measurement rounded to `step`, NaN as null
        static Json rounded(double value, double step);
    };

}
