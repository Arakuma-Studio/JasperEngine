// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SoundSlicer.h"

#include "AudioImportSettings.h"
#include "AudioProvenance.h"
#include "SoundTypes.h"
#include "WavWriter.h"
#include "command/type/SetAssetMetaCmd.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

const double GATE_LOOKAHEAD_MS = 50.0;
const double ENVELOPE_MS = 1.0;
const double REFINE_SHARE = 0.1;
const double TAIL_WINDOW_MS = 5.0;

double dbOf(double amplitude) {
    return amplitude > 0.0 ? 20.0 * std::log10(amplitude) : AudioMeter::SILENCE_DB;
}

size_t framesOf(double ms, double rate) {
    return (size_t)std::llround(ms * rate / 1000.0);
}

// the largest |sample| of a frame over the channels
double frameMaximum(const AudioAnalysis::Signal& signal, size_t i) {
    double largest = 0.0;
    for (unsigned int c = 0; c < signal.channels; c++) largest = std::max(largest, (double)std::fabs(signal.channel(c)[i]));
    return largest;
}

// Does a slice's outer sample click against silence? Any channel's, by analyze_audio's edge rule.
bool edgeClicks(const AudioAnalysis::Signal& signal, size_t edge, size_t inner, double peak) {
    for (unsigned int c = 0; c < signal.channels; c++) {
        const double v = signal.channel(c)[edge];
        if (AudioAnalysis::edgeSampleClicks(v, v - (double)signal.channel(c)[inner], peak)) return true;
    }
    return false;
}

} // namespace

bool SoundSlicer::checkSource(const AudioAnalysis::Signal& signal, std::string& error) {
    if (signal.seconds() <= SoundTypes::SLICE_MAX_SOURCE_SECONDS) return true;
    char text[120];
    std::snprintf(text, sizeof(text), "the source is %.0f s; a slice call takes at most %.0f s.", signal.seconds(), SoundTypes::SLICE_MAX_SOURCE_SECONDS);
    error = text;
    return false;
}

bool SoundSlicer::detect(const AudioAnalysis::Signal& signal, const Options& options, const std::string& kind, std::vector<Slice>& slices, std::string& error) {
    slices.clear();
    if (options.gateDb < -80.0 || options.gateDb > -6.0) {
        error = "gate_db is from -80 to -6.";
        return false;
    }
    if (options.minGapMs < 10.0 || options.minGapMs > 2000.0) {
        error = "min_gap_ms is from 10 to 2000.";
        return false;
    }
    if (options.preMs < 0.0 || options.preMs > 50.0) {
        error = "pre_ms is from 0 to 50.";
        return false;
    }
    if (options.maxMs < 20.0 || options.maxMs > 10000.0) {
        error = "max_ms is from 20 to 10000.";
        return false;
    }
    if (!checkSource(signal, error)) return false;
    const double rate = signal.sampleRate;
    const size_t n = signal.frames;
    double filePeak = 0.0;
    for (size_t i = 0; i < n; i++) filePeak = std::max(filePeak, frameMaximum(signal, i));
    if (filePeak <= 0.0) {
        error = "the source is silent: nothing to slice.";
        return false;
    }

    // the 1 ms envelope of the mono mix
    const std::vector<float> mono = signal.mono();
    const size_t step = std::max<size_t>(1, framesOf(ENVELOPE_MS, rate));
    std::vector<double> envelope(n / step + 1, 0.0);
    for (size_t i = 0; i < n; i += step) {
        double sum = 0.0;
        const size_t end = std::min(n, i + step);
        for (size_t k = i; k < end; k++) sum += (double)mono[k] * mono[k];
        envelope[i / step] = std::sqrt(sum / (double)step);
    }
    double monoPeak = 0.0;
    for (float x : mono) monoPeak = std::max(monoPeak, (double)std::fabs(x));
    const double gate = monoPeak * std::pow(10.0, options.gateDb / 20.0);
    const size_t lookahead = (size_t)std::ceil(GATE_LOOKAHEAD_MS / ENVELOPE_MS);

    // onsets as analyze_audio finds them, refined to the first 1 ms frame at 10% of what follows
    const nlohmann::json measured = AudioAnalysis::measure(signal, AudioMeterMode::File);
    std::vector<double> accepted;
    for (const nlohmann::json& onsetMs : measured.value("onsets_ms", nlohmann::json::array())) {
        const size_t frame = std::min(envelope.size() - 1, framesOf(onsetMs.get<double>(), rate) / step);
        const size_t last = std::min(envelope.size(), frame + lookahead + 1);
        double largest = 0.0;
        for (size_t k = frame; k < last; k++) largest = std::max(largest, envelope[k]);
        if (largest < gate) continue;
        size_t at = frame;
        for (size_t k = frame; k < last; k++) {
            if (envelope[k] >= REFINE_SHARE * largest) {
                at = k;
                break;
            }
        }
        const double ms = (double)at * (double)step / rate * 1000.0;
        if (!accepted.empty() && ms - accepted.back() < options.minGapMs) continue;
        accepted.push_back(ms);
    }
    if (accepted.empty()) {
        error = "no take passes gate_db: lower it.";
        return false;
    }
    if ((int)accepted.size() > SoundTypes::SLICE_MAX) {
        error = std::to_string(accepted.size()) + " onsets: raise gate_db or min_gap_ms.";
        return false;
    }

    const size_t tailWindow = std::max<size_t>(1, framesOf(TAIL_WINDOW_MS, rate));
    size_t previousEnd = 0;
    for (size_t s = 0; s < accepted.size(); s++) {
        const size_t onset = framesOf(accepted[s], rate);
        const size_t pre = framesOf(options.preMs, rate);
        const size_t start = std::max(previousEnd, onset > pre ? onset - pre : 0);
        size_t limit = std::min(n, start + framesOf(options.maxMs, rate));
        if (s + 1 < accepted.size()) {
            const size_t next = framesOf(accepted[s + 1], rate);
            limit = std::min(limit, next > pre ? next - pre : 0);
        }
        if (limit <= start) continue;
        double peak = 0.0;
        for (size_t i = start; i < limit; i++) peak = std::max(peak, frameMaximum(signal, i));
        const double threshold = peak * std::pow(10.0, SoundTypes::SLICE_TAIL_DB / 20.0);
        // ends 5 ms after the last window at or above the tail level
        size_t end = start;
        for (size_t w = start; w < limit; w += tailWindow) {
            double largest = 0.0;
            const size_t stop = std::min(limit, w + tailWindow);
            for (size_t i = w; i < stop; i++) largest = std::max(largest, frameMaximum(signal, i));
            if (largest >= threshold) end = stop;
        }
        end = std::min(limit, end + tailWindow);
        if ((double)(end - start) / rate * 1000.0 < SoundTypes::SLICE_MIN_MS) continue;

        Slice slice;
        slice.startFrame = (uint32_t)start;
        slice.endFrame = (uint32_t)end;
        slice.peakDb = dbOf(peak);
        slice.headCutDb = AudioAnalysis::cutLevelDb(signal, slice.startFrame);
        slice.tailCutDb = AudioAnalysis::cutLevelDb(signal, slice.endFrame);
        if (edgeClicks(signal, start, start + 1, filePeak)) slice.fadeInMs = SoundTypes::clickSafeFadeInMs(kind);
        if (edgeClicks(signal, end - 1, end - 2, filePeak)) slice.fadeOutMs = SoundTypes::clickSafeFadeOutMs(kind);
        slices.push_back(slice);
        previousEnd = end;
    }
    if (slices.empty()) {
        error = "every take is shorter than " + std::to_string((int)SoundTypes::SLICE_MIN_MS) + " ms: lower gate_db or min_gap_ms.";
        return false;
    }
    return true;
}

bool SoundSlicer::plan(const fs::path& source, const std::string& sourceRel, const std::string& sourceSha256, const AudioAnalysis::Signal& signal,
                       const std::vector<Slice>& slices, const std::vector<int>& pick, const fs::path& eventFile, std::vector<File>& files, std::string& error) {
    files.clear();
    std::vector<int> picked = pick;
    if (picked.empty()) {
        for (int i = 1; i <= (int)slices.size(); i++) picked.push_back(i);
    }
    for (int n : picked) {
        if (n < 1 || n > (int)slices.size()) {
            error = "pick " + std::to_string(n) + " is not a slice: there are " + std::to_string(slices.size()) + ".";
            return false;
        }
    }
    uint64_t bytes = 0;
    for (int n : picked) {
        const Slice& s = slices[(size_t)n - 1];
        bytes += (uint64_t)(s.endFrame - s.startFrame) * signal.channels * 2 + 44;
    }
    if (bytes > SoundTypes::SLICE_MAX_BYTES) {
        error = "the slices would be " + std::to_string(bytes / (1024 * 1024)) + " MiB, more than 32 MiB: pick fewer or lower max_ms.";
        return false;
    }
    const std::string stem = eventFile.stem().string();
    const fs::path directory = eventFile.parent_path();

    AudioImportSettings sourceSettings;
    std::string unread;
    if (!AudioImportSettings::load(source, sourceSettings, unread)) sourceSettings = AudioImportSettings();
    AudioProvenance sourceProvenance;
    if (!AudioProvenance::load(source, sourceProvenance, unread)) sourceProvenance = AudioProvenance();
    const YAML::Node labels = SetAssetMetaCmd::read(source, "labels");

    int index = 0;
    for (int n : picked) {
        index++;
        const Slice& s = slices[(size_t)n - 1];
        char number[16];
        std::snprintf(number, sizeof(number), "%02d", index);
        File file;
        file.wav = directory / (stem + "_" + number + ".wav");
        std::error_code ec;
        if (fs::exists(file.wav, ec)) {
            error = file.wav.filename().string() + " exists: choose another event name.";
            files.clear();
            return false;
        }
        std::vector<float> interleaved((size_t)(s.endFrame - s.startFrame) * signal.channels);
        for (unsigned int c = 0; c < signal.channels; c++) {
            const float* channel = signal.channel(c);
            for (uint32_t i = s.startFrame; i < s.endFrame; i++) interleaved[(size_t)(i - s.startFrame) * signal.channels + c] = channel[i];
        }
        file.wavBytes = WavWriter::pcm16(interleaved, signal.channels, signal.sampleRate);

        AudioImportSettings settings;
        settings.type = sourceSettings.type;
        settings.fadeInMs = s.fadeInMs;
        settings.fadeOutMs = s.fadeOutMs;
        AudioProvenance provenance;
        provenance.values = sourceProvenance.values;
        // what the slice came from replaces what the source was generated or imported from
        provenance.values.erase("builtin");
        provenance.values.erase("generator");
        provenance.values["sliced_from"] = {{"path", sourceRel}, {"sha256", sourceSha256}, {"start_frame", s.startFrame}, {"end_frame", s.endFrame}};
        YAML::Node importSection(YAML::NodeType::Map);
        importSection["audioImporter"] = settings.toYaml();
        YAML::Node sections(YAML::NodeType::Map);
        sections["import"] = importSection;
        sections["provenance"] = provenance.toYaml();
        if (labels.IsSequence()) sections["labels"] = labels;
        if (!clipMetaText(file.wav, sections, file.metaText, error)) {
            files.clear();
            return false;
        }
        files.push_back(std::move(file));
    }
    return true;
}

nlohmann::json SoundSlicer::toJson(const Slice& slice, int n, unsigned int rate) {
    return {{"n", n},
            {"start_s", AudioAnalysis::rounded((double)slice.startFrame / rate, 0.001)},
            {"end_s", AudioAnalysis::rounded((double)slice.endFrame / rate, 0.001)},
            {"peak_db", AudioAnalysis::rounded(slice.peakDb, 0.01)},
            {"head_cut_db", AudioAnalysis::rounded(slice.headCutDb, 0.01)},
            {"tail_cut_db", AudioAnalysis::rounded(slice.tailCutDb, 0.01)},
            {"fade_in_ms", slice.fadeInMs},
            {"fade_out_ms", slice.fadeOutMs}};
}
