// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AudioQa.h"

#include "AudioBaseline.h"
#include "util/QaChecks.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    using Json = nlohmann::json;

    constexpr double AUDIO_QA_FLOOR_DB = -60.0;
    constexpr int MAX_AT = qa::MAX_AT;

    double linear(double db) {
        return std::pow(10.0, db / 20.0);
    }

    double round2(double value) {
        return std::round(value * 100.0) / 100.0;
    }

    // what a run reads, once
    struct Context {
        const AudioSessionData& data;
        const AudioQa::Request& request;
        std::vector<float> millis;           // the loudest |sample| of each ms of the range
        int frameOfMs = 48;
        std::vector<Json> starts;            // voice_start lines of the range
        std::map<int64_t, Json> startOfVoice;
        std::vector<Json> markers;           // the gameplay markers of the range
        std::map<std::string, Json> clips;   // analysis of each clip
        double worstMargin = 1e9;
        double worstMarginAt = -1.0;
        std::string worstMarginClip;
    };

    Json entry(const std::string& id) {
        static const std::set<std::string> mixer = {"true_peak", "clipping", "voice_budget", "load_delay"};
        return qa::row(id, mixer.count(id) ? "mixer" : "content");
    }

    void addAt(Json& check, double t) {
        qa::addAt(check, t);
    }

    double stampSeconds(const Context& c, const Json& line) {
        return c.data.seconds(line);
    }

    // the voice's stamp, in frames of the range
    int64_t rangeFrame(const Context& c, const Json& line) {
        return (int64_t)line.value("sample", (uint64_t)0) - (int64_t)c.data.startFrame;
    }

    std::string entityOf(const Json& line) {
        return line.contains("entity") && line["entity"].is_string() ? line["entity"].get<std::string>() : std::string();
    }

    const Json& clipAnalysis(Context& c, const std::string& clip) {
        auto found = c.clips.find(clip);
        if (found != c.clips.end()) return found->second;
        Json out;
        std::error_code ec;
        const fs::path inProject = c.request.projectDirectory / fs::path(clip);
        const fs::path inAssets = c.request.assetsDirectory / fs::path(clip);
        const fs::path file = fs::exists(inAssets, ec) ? inAssets : inProject;
        std::string error;
        if (!AudioAnalysis::analyzeClip(file, AudioAnalysis::cacheDirectory(c.request.projectDirectory), out, error)) out = Json::object();
        return c.clips.emplace(clip, std::move(out)).first->second;
    }

    // ---- loudness, peaks

    Json checkLoudness(Context& c) {
        Json check = entry("loudness");
        check["method"] = "BS.1770 integrated loudness of master.wav over the range";
        const AudioQaSettings& qa = c.request.qa;
        const AudioLoudnessTargets& targets = c.request.targets;
        const double gated = (double)c.data.loudness.gatedBlocks * AudioMeter::CURVE_STEP;
        check["threshold"] = {{"target_lufs", targets.targetLufs}, {"tolerance_lu", targets.toleranceLu}};
        check["threshold_source"] = "audio.loudness_target_lufs, audio.loudness_tolerance_lu";
        if (!c.data.loudness.integrated || gated < qa.loudnessMinSeconds) {
            check["measured"] = {{"gated_seconds", round2(gated)}};
            check["method"] = std::string(check["method"]) + "; skipped under audio.qa.loudness_min_seconds of gated audio";
            return check;
        }
        const double lufs = *c.data.loudness.integrated;
        check["measured"] = round2(lufs);
        const double off = lufs - targets.targetLufs;
        if (std::fabs(off) <= targets.toleranceLu) {
            check["status"] = "PASS";
        } else {
            check["status"] = "FAIL";
            check["limited_by"] = "The mix is " + std::to_string((int)std::lround(std::fabs(off))) + " LU " + (off > 0 ? "above" : "below") + " the target.";
            check["fix_hint"] = "Move the groups' faders (audio_mixer) by about " + std::to_string((int)std::lround(-off)) + " dB.";
        }
        return check;
    }

    Json checkTruePeak(Context& c) {
        Json check = entry("true_peak");
        check["method"] = "BS.1770 4x oversampled peak of master.wav";
        const double peak = c.data.loudness.truePeakDb;
        const double ceiling = c.request.targets.ceilingDbtp + 0.1;
        check["measured"] = round2(peak);
        check["threshold"] = round2(ceiling);
        check["threshold_source"] = "audio.true_peak_ceiling_dbtp + 0.1 dB";
        check["status"] = peak <= ceiling ? "PASS" : "FAIL";
        if (peak > ceiling) {
            check["limited_by"] = "The output passes the ceiling by " + std::to_string(round2(peak - ceiling)).substr(0, 4) + " dB.";
            check["fix_hint"] = "Lower the loudest group, or put a limiter on Master (audio_mixer).";
        }
        // S38's limiter lines, when the session has them
        double reduction = 0.0;
        double at = -1.0;
        for (const Json& line : c.data.events) {
            if (line.value("kind", "") != "limiter" || !c.data.inRange(line)) continue;
            const double value = line.value("gain_reduction_db", 0.0);
            if (value > reduction) {
                reduction = value;
                at = c.data.seconds(line);
            }
        }
        if (at >= 0.0 && reduction > c.request.qa.limiterWarnDb && check["status"] == "PASS") {
            check["status"] = "WARN";
            check["threshold_source"] = "audio.qa.limiter_warn_db";
            check["limited_by"] = "The Master limiter takes " + std::to_string((int)std::lround(reduction)) + " dB off: the mix is too hot.";
            check["fix_hint"] = "Lower the groups before the limiter.";
            addAt(check, at);
        }
        return check;
    }

    Json checkClipping(Context& c) {
        Json check = entry("clipping");
        check["method"] = "samples of master.wav with |x| >= 1 - 2^-15";
        check["threshold"] = AudioMeter::CLIP_LEVEL;
        check["threshold_source"] = "the session";
        uint64_t clipped = 0;
        const AudioAnalysis::Signal& audio = c.data.audio;
        for (unsigned int ch = 0; ch < audio.channels; ch++) {
            const float* samples = audio.channel(ch);
            for (size_t i = 0; i < audio.frames; i++) {
                if (std::fabs(samples[i]) >= AudioMeter::CLIP_LEVEL) {
                    if (clipped++ == 0 || (int)check["at"].size() < MAX_AT) addAt(check, c.data.startSeconds + (double)i / c.data.rate);
                }
            }
        }
        check["measured"] = clipped;
        check["status"] = clipped ? "FAIL" : "PASS";
        if (clipped) {
            check["limited_by"] = std::to_string(clipped) + " clipped sample(s).";
            check["fix_hint"] = "Lower the loudest group, or put a limiter on Master (audio_mixer).";
        }
        return check;
    }

    // ---- coverage and sync

    bool matchesOn(const Json& marker, const std::string& on) {
        return marker.value("kind", "") == on || (marker.value("kind", "") == "marker" && marker.value("label", "") == on) ||
               (on.rfind("marker:", 0) == 0 && marker.value("label", "") == on.substr(7));
    }

    bool matchesSound(const Json& start, const std::string& sound) {
        if (sound.empty()) return true;
        return start.value("clip", "").find(sound) != std::string::npos || start.value("event_path", "") == sound || start.value("event", "") == sound;
    }

    // the time of a start's aligned point: its stamp, the clip's peak or its end
    double alignedTime(Context& c, const Json& start, const std::string& align) {
        const double t = stampSeconds(c, start);
        if (align == "onset") return t;
        const Json& clip = clipAnalysis(c, start.value("clip", ""));
        const double speed = std::max(1e-3, start.value("pitch", 1.0));
        if (align == "peak") {
            const Json onset = clip.value("onset", Json::object());
            if (onset.contains("peak_ms") && onset["peak_ms"].is_number()) return t + onset["peak_ms"].get<double>() / 1000.0 / speed;
            return t;
        }
        const Json length = clip.contains("played") ? clip["played"] : clip.value("format", Json::object());
        return t + length.value("duration_s", 0.0) / speed;
    }

    Json checkCoverage(Context& c, std::vector<Json>& matched) {
        Json check = entry("coverage");
        const AudioQaSettings& qa = c.request.qa;
        const Json& rules = c.request.expect;
        check["threshold"] = qa.syncToleranceMs;
        check["threshold_source"] = "audio.qa.sync_tolerance_ms";
        if (!rules.empty()) {
            check["method"] = "each marker of an expect rule has its voice_start within within_ms of the aligned point";
            int total = 0, missed = 0;
            double worst = 0.0;
            std::string first;
            for (size_t r = 0; r < rules.size(); r++) {
                const Json& rule = rules[r];
                const std::string on = rule.value("on", "");
                const std::string wantEntity = rule.value("entity", "");
                const std::string sound = rule.value("sound", "");
                const std::string align = rule.value("align", "onset");
                const double within = rule.value("within_ms", (double)qa.syncToleranceMs);
                const double offset = rule.value("offset_ms", 0.0);
                if (rule.contains("within_ms")) {
                    check["threshold"] = within;
                    check["threshold_source"] = "expect[" + std::to_string(r) + "].within_ms";
                }
                for (const Json& marker : c.markers) {
                    if (!matchesOn(marker, on)) continue;
                    if (!wantEntity.empty() && entityOf(marker) != wantEntity) continue;
                    total++;
                    const double target = stampSeconds(c, marker) + offset / 1000.0;
                    const Json* best = nullptr;
                    double bestDistance = 1e9;
                    for (const Json& start : c.starts) {
                        if (!matchesSound(start, sound)) continue;
                        const double distance = std::fabs(alignedTime(c, start, align) - target) * 1000.0;
                        if (distance < bestDistance) {
                            bestDistance = distance;
                            best = &start;
                        }
                    }
                    if (best && bestDistance <= within) {
                        matched.push_back(*best);
                        worst = std::max(worst, bestDistance);
                    } else {
                        missed++;
                        addAt(check, stampSeconds(c, marker));
                        if (first.empty()) first = marker.value("label", on) + " at " + std::to_string(round2(stampSeconds(c, marker))).substr(0, 5) + " s";
                    }
                }
            }
            check["measured"] = {{"markers", total}, {"missed", missed}, {"worst_ms", round2(worst)}};
            check["status"] = missed ? "FAIL" : "PASS";
            if (missed) {
                check["limited_by"] = std::to_string(missed) + " of " + std::to_string(total) + " expected moment(s) had no sound in time; first: " + first + ".";
                check["fix_hint"] = "Play the sound from that moment (AudioSource playOn, Audio.play).";
            }
            return check;
        }

        check["method"] = "a marker class (kind + entity) sounded on some occurrences and not on others";
        std::map<std::string, std::pair<int, int>> classes;   // sounded, silent
        std::map<std::string, double> firstSilent;
        for (const Json& marker : c.markers) {
            const std::string kind = marker.value("kind", "");
            if (kind == "scene_load" || kind == "input" || kind == "marker" || kind == "ui_hover") continue;
            const std::string key = kind + " " + entityOf(marker);
            const double t = stampSeconds(c, marker);
            bool sounded = false;
            for (const Json& start : c.starts) {
                if (std::fabs(stampSeconds(c, start) - t) * 1000.0 <= qa.syncToleranceMs) {
                    sounded = true;
                    matched.push_back(start);
                }
            }
            auto& counts = classes[key];
            (sounded ? counts.first : counts.second)++;
            if (!sounded && !firstSilent.count(key)) firstSilent[key] = t;
        }
        if (classes.empty()) return check;
        check["status"] = "PASS";
        int partial = 0;
        std::string first;
        for (const auto& [key, counts] : classes) {
            if (counts.first > 0 && counts.second > 0) {
                partial++;
                addAt(check, firstSilent[key]);
                if (first.empty()) first = key + " sounded " + std::to_string(counts.first) + " of " + std::to_string(counts.first + counts.second) + " times";
            }
        }
        check["measured"] = {{"classes", classes.size()}, {"partial", partial}};
        if (partial) {
            check["status"] = "WARN";
            check["limited_by"] = first + ".";
            check["fix_hint"] = "Pass expect rules for what must sound, or play it every time.";
        }
        return check;
    }

    Json checkSync(Context& c, const std::vector<Json>& matched) {
        Json check = entry("sync");
        const AudioQaSettings& qa = c.request.qa;
        check["method"] = "audible onset - stamp - latency_samples - delay_samples, on master.wav";
        check["threshold"] = qa.headSilenceMaxMs;
        check["threshold_source"] = "audio.qa.head_silence_max_ms";
        const std::vector<Json>& voices = matched.empty() ? c.starts : matched;
        double worst = -1.0;
        std::string worstClip;
        int measured = 0, late = 0;
        std::set<int64_t> seen;
        for (const Json& start : voices) {
            if (!seen.insert(start.value("voice", (int64_t)0)).second) continue;
            const int64_t stamp = rangeFrame(c, start);
            if (stamp < 0 || stamp >= (int64_t)c.data.audio.frames) continue;
            const int64_t search = (int64_t)c.data.rate;
            const int64_t onset = AudioQa::onsetFrame(c.data, stamp, search, qa.audibilityMarginDb);
            if (onset < 0) continue;
            measured++;
            const double ms = (double)(onset - stamp - start.value("latency_samples", 0) - start.value("delay_samples", 0)) * 1000.0 / c.data.rate;
            if (ms > worst) {
                worst = ms;
                worstClip = start.value("clip", "");
            }
            if (ms > qa.headSilenceMaxMs) {
                late++;
                addAt(check, stampSeconds(c, start));
            }
        }
        if (measured == 0) return check;
        check["measured"] = round2(std::max(worst, 0.0));
        check["status"] = late ? "FAIL" : "PASS";
        if (late) {
            check["limited_by"] = worstClip + " starts " + std::to_string((int)std::lround(worst)) + " ms after it was played.";
            check["fix_hint"] = "Trim its head: set_audio_import_settings {paths: [\"" + worstClip + "\"], settings: {trim_head_ms: \"auto\"}}.";
        }
        return check;
    }

    // ---- the groups

    // the groups a voice of `group` sounds through, Master and its own among them
    std::set<std::string> relatedGroups(const Context& c, const std::string& group) {
        std::set<std::string> related = {group, "Master"};
        const Json parents = c.data.capture.session.value("mixer", Json::object()).value("parents", Json::object());
        std::string at = group;
        for (int depth = 0; depth < 32 && parents.contains(at); depth++) {
            at = parents[at].get<std::string>();
            related.insert(at);
        }
        for (auto it = parents.begin(); it != parents.end(); ++it) {
            std::string up = it.key();
            for (int depth = 0; depth < 32 && parents.contains(up); depth++) {
                up = parents[up].get<std::string>();
                if (up == group) related.insert(it.key());
            }
        }
        return related;
    }

    Json checkAudibility(Context& c) {
        Json check = entry("audibility");
        const AudioQaSettings& qa = c.request.qa;
        check["method"] = "a voice's dominant band (the clip's analysis + its gain) against the other groups' over its first 500 ms";
        check["threshold"] = qa.audibilityMarginDb;
        check["threshold_source"] = "audio.qa.audibility_margin_db";
        int compared = 0, masked = 0;
        std::string limited;
        std::string fix;
        for (const Json& start : c.starts) {
            const std::string group = start.value("group", "");
            const Json& clip = clipAnalysis(c, start.value("clip", ""));
            // S36's bands, by their labels
            const Json bands = clip.value("bands_db", Json::object());
            if (!bands.is_object() || bands.empty()) continue;
            int dominant = 0;
            double level = -1e9;
            for (int b = 0; b < AudioMeter::BANDS; b++) {
                const char* label = AudioMeter::bandLabel(b);
                if (bands.contains(label) && bands[label].is_number() && bands[label].get<double>() > level) {
                    level = bands[label].get<double>();
                    dominant = b;
                }
            }
            // the voice's gain, and the constant-power pan's half of its energy on each channel
            level += start.value("gain_db", 0.0) - 3.01;
            const double t = stampSeconds(c, start);
            const std::set<std::string> related = relatedGroups(c, group);
            double loudest = -1e9;
            std::string maskedBy;
            for (const Json& line : c.data.levels) {
                const double lineT = c.data.seconds(line);
                if (lineT < t || lineT > t + 0.6) continue;
                const Json groups = line.value("groups", Json::object());
                for (auto it = groups.begin(); it != groups.end(); ++it) {
                    if (related.count(it.key())) continue;
                    const Json other = it.value().value("bands_db", Json::array());
                    if (other.size() != (size_t)AudioMeter::BANDS || !other[dominant].is_number()) continue;
                    if (other[dominant].get<double>() > loudest) {
                        loudest = other[dominant].get<double>();
                        maskedBy = it.key();
                    }
                }
            }
            if (loudest <= AUDIO_QA_FLOOR_DB) continue;
            compared++;
            const double margin = level - loudest;
            if (margin < c.worstMargin) {
                c.worstMargin = margin;
                c.worstMarginAt = t;
                c.worstMarginClip = start.value("clip", "");
            }
            if (margin < qa.audibilityMarginDb) {
                masked++;
                addAt(check, t);
                if (limited.empty()) {
                    const double raise = qa.audibilityMarginDb - margin;
                    limited = start.value("clip", "") + " is masked by group " + maskedBy + " in the " + AudioMeter::bandLabel(dominant) + " band (margin " +
                              std::to_string(round2(margin)).substr(0, 5) + " dB).";
                    fix = "Lower group " + maskedBy + " by " + std::to_string((int)std::ceil(raise)) + " dB, or raise the voice by as much.";
                }
            }
        }
        if (compared == 0) return check;
        check["measured"] = round2(c.worstMargin);
        check["status"] = masked ? "FAIL" : "PASS";
        if (masked) {
            check["limited_by"] = limited;
            check["fix_hint"] = fix;
        }
        return check;
    }

    Json checkVoiceBudget(Context& c) {
        Json check = entry("voice_budget");
        const AudioQaSettings& qa = c.request.qa;
        check["method"] = "voices at once in the log, and the important ones stolen or rejected";
        const int peak = AudioSessionReader::voicesPeak(c.data, "");
        const double limit = c.request.maxVoices * (double)qa.voiceBudgetWarn;
        check["threshold"] = limit;
        check["threshold_source"] = "audio.max_voices x audio.qa.voice_budget_warn";
        check["measured"] = peak;
        check["status"] = "PASS";
        if (peak > limit) {
            check["status"] = "WARN";
            check["limited_by"] = std::to_string(peak) + " voices at once of " + std::to_string(c.request.maxVoices) + ".";
            check["fix_hint"] = "Cap the busy sounds (a Sound Event's max instances, AudioSource maxVoices).";
        }
        int lost = 0;
        std::string first;
        for (const Json& line : c.data.events) {
            if (!c.data.inRange(line)) continue;
            const std::string kind = line.value("kind", "");
            int priority = -1;
            if (kind == "voice_reject" && line.value("reason", "") == "budget") {
                priority = line.value("priority", 128);
            } else if (kind == "voice_stop" && line.value("reason", "").rfind("stolen", 0) == 0) {
                auto found = c.startOfVoice.find(line.value("voice", (int64_t)0));
                if (found != c.startOfVoice.end()) priority = found->second.value("priority", 128);
            }
            if (priority >= 0 && priority <= qa.importantPriority) {
                lost++;
                addAt(check, c.data.seconds(line));
                if (first.empty()) first = line.value("clip", "");
            }
        }
        if (lost) {
            check["status"] = "FAIL";
            check["threshold"] = qa.importantPriority;
            check["threshold_source"] = "audio.qa.important_priority";
            check["limited_by"] = std::to_string(lost) + " important voice(s) lost to the budget; first: " + first + ".";
            check["fix_hint"] = "Lower the priority of the many (a higher number), or raise audio.max_voices.";
        }
        return check;
    }

    Json checkDucking(Context& c) {
        Json check = entry("ducking");
        const AudioQaSettings& qa = c.request.qa;
        check["method"] = "a ducked group's level while its side-chain group is above -60 dB against while it is not";
        if (c.request.ducks.empty()) return check;
        bool any = false;
        std::string limited;
        std::string warned;
        double worstDrop = 1e9;
        for (const AudioQa::DuckEffect& duck : c.request.ducks) {
            // medians: the lines the side chain starts or stops in hold some of both
            std::vector<double> duckedOn, duckedOff, sideOn;
            for (const Json& line : c.data.levels) {
                if (!c.data.inRange(line)) continue;
                const Json groups = line.value("groups", Json::object());
                if (!groups.contains(duck.group) || !groups.contains(duck.sidechain)) continue;
                const double side = groups[duck.sidechain].value("rms_db", AudioMeter::SILENCE_DB);
                const double level = groups[duck.group].value("rms_db", AudioMeter::SILENCE_DB);
                if (side > AUDIO_QA_FLOOR_DB) {
                    duckedOn.push_back(level);
                    sideOn.push_back(side);
                } else if (level > AUDIO_QA_FLOOR_DB) {
                    duckedOff.push_back(level);
                }
            }
            if (duckedOn.empty() || duckedOff.empty()) continue;
            any = true;
            const auto median = [](std::vector<double>& values) {
                std::nth_element(values.begin(), values.begin() + (std::ptrdiff_t)(values.size() / 2), values.end());
                return values[values.size() / 2];
            };
            const double offDb = median(duckedOff);
            const double onDb = median(duckedOn);
            const double sideDb = median(sideOn);
            const double drop = offDb - onDb;
            const double required = std::max(0.0, -20.0 * std::log10(std::max(duck.level, 1e-6)) - 3.0);
            worstDrop = std::min(worstDrop, drop);
            check["threshold"] = round2(required);
            if (drop < required) {
                limited = "Group " + duck.group + " drops only " + std::to_string(round2(drop)).substr(0, 5) + " dB while " + duck.sidechain + " plays.";
            } else if (sideDb - onDb < qa.voiceOverMusicDb) {
                warned = "Group " + duck.sidechain + " stands only " + std::to_string(round2(sideDb - onDb)).substr(0, 5) + " dB over " + duck.group + ".";
            }
        }
        if (!any) return check;
        check["measured"] = round2(worstDrop);
        check["threshold_source"] = "the duck effect's level - 3 dB";
        check["status"] = !limited.empty() ? "FAIL" : !warned.empty() ? "WARN" : "PASS";
        if (!limited.empty()) {
            check["limited_by"] = limited;
            check["fix_hint"] = "Lower the duck effect's level, or shorten its attack (audio_mixer).";
        } else if (!warned.empty()) {
            check["threshold_source"] = "audio.qa.voice_over_music_db";
            check["limited_by"] = warned;
            check["fix_hint"] = "Raise the side-chain group or lower the ducked one (audio_mixer).";
        }
        return check;
    }

    Json checkSilence(Context& c) {
        Json check = entry("silence");
        const AudioQaSettings& qa = c.request.qa;
        check["method"] = "gaps below -60 dBFS in master.wav after the first music voice";
        check["threshold"] = qa.maxSilenceSeconds;
        check["threshold_source"] = "audio.qa.max_silence_seconds";
        int64_t from = -1;
        for (const Json& start : c.starts) {
            if (start.value("group", "") == qa.musicGroup || start.value("owner", "") == "music") {
                from = rangeFrame(c, start);
                break;
            }
        }
        if (from < 0) return check;
        const double floor = linear(AUDIO_QA_FLOOR_DB);
        double longest = 0.0, run = 0.0;
        double runStart = 0.0;
        const size_t first = (size_t)(from / c.frameOfMs);
        for (size_t m = first; m < c.millis.size(); m++) {
            if (c.millis[m] < floor) {
                if (run == 0.0) runStart = c.data.startSeconds + (double)m / 1000.0;
                run += 0.001;
                if (run > longest) longest = run;
            } else {
                if (run > qa.maxSilenceSeconds) addAt(check, runStart);
                run = 0.0;
            }
        }
        if (run > qa.maxSilenceSeconds) addAt(check, runStart);
        check["measured"] = round2(longest);
        check["status"] = longest > qa.maxSilenceSeconds ? "FAIL" : "PASS";
        if (longest > qa.maxSilenceSeconds) {
            check["limited_by"] = "The music stops for " + std::to_string(round2(longest)).substr(0, 4) + " s.";
            check["fix_hint"] = "Loop the music, or queue the next track before this one ends.";
        }
        return check;
    }

    Json checkLoadDelay(Context& c) {
        Json check = entry("load_delay");
        check["method"] = "voice_start lines with load_delay";
        check["threshold"] = 0;
        check["threshold_source"] = "the session";
        int delayed = 0, worst = 0;
        std::string clip;
        for (const Json& start : c.starts) {
            const int delay = start.value("load_delay", 0);
            if (delay <= 0) continue;
            delayed++;
            addAt(check, stampSeconds(c, start));
            if (delay > worst) {
                worst = delay;
                clip = start.value("clip", "");
            }
        }
        check["measured"] = worst;
        check["status"] = delayed ? "WARN" : c.starts.empty() ? "SKIP" : "PASS";
        if (delayed) {
            check["limited_by"] = clip + " waited " + std::to_string(worst) + " samples for its load.";
            check["fix_hint"] = "AudioSystem.preload it, or use Preload on the source.";
        }
        return check;
    }

    Json checkRepetition(Context& c) {
        Json check = entry("repetition");
        const AudioQaSettings& qa = c.request.qa;
        check["method"] = "plays of one clip in a row from one source or event";
        check["threshold"] = qa.repeatMinPlays;
        check["threshold_source"] = "audio.qa.repeat_min_plays, audio.qa.repeat_window_s";
        if (c.starts.empty()) return check;
        struct Run {
            std::string clip;
            std::vector<double> times;
        };
        std::map<std::string, Run> runs;
        int worst = 0;
        std::string limited;
        for (const Json& start : c.starts) {
            const std::string event = start.value("event", "");
            const std::string source = !event.empty() ? "event " + event : !entityOf(start).empty() ? entityOf(start) : start.value("owner", "") + " " + start.value("clip", "");
            Run& run = runs[source];
            const std::string clip = start.value("clip", "");
            const double t = stampSeconds(c, start);
            if (run.clip != clip) {
                run.clip = clip;
                run.times.clear();
            }
            run.times.push_back(t);
            while (!run.times.empty() && t - run.times.front() > qa.repeatWindowS) run.times.erase(run.times.begin());
            const int plays = (int)run.times.size();
            if (plays >= qa.repeatMinPlays) {
                if (plays > worst) {
                    worst = plays;
                    limited = source + " plays " + clip + " " + std::to_string(plays) + " times in a row.";
                }
                addAt(check, t);
            }
        }
        check["measured"] = worst;
        check["status"] = worst ? "WARN" : "PASS";
        if (worst) {
            check["limited_by"] = limited;
            check["fix_hint"] = "Use a Sound Event with several clips, or vary its pitch and volume (edit_sound_event).";
        }
        return check;
    }

    Json checkRegression(Context& c) {
        Json check = entry("regression");
        const AudioQaSettings& qa = c.request.qa;
        check["method"] = "starts and onsets, and the group bands per marker, against the baseline";
        check["threshold"] = {{"timing_ms", qa.regressionTimingMs}, {"band_db", qa.regressionBandDb}};
        check["threshold_source"] = "audio.qa.regression_timing_ms, audio.qa.regression_band_db";
        const Json& baseline = c.request.baseline;
        if (baseline.is_null()) return check;
        const Json now = AudioBaseline::build(c.data, qa.audibilityMarginDb);
        const auto keyed = [](const Json& list) {
            std::map<std::string, Json> out;
            for (const Json& item : list) out[item.value("key", "") + "#" + std::to_string(item.value("n", 0))] = item;
            return out;
        };
        const std::map<std::string, Json> startsThen = keyed(baseline.value("starts", Json::array()));
        const std::map<std::string, Json> startsNow = keyed(now.value("starts", Json::array()));
        double worstTiming = 0.0, worstBand = 0.0;
        std::string limited;
        for (const auto& [key, then] : startsThen) {
            auto found = startsNow.find(key);
            if (found == startsNow.end()) {
                limited = key.substr(0, key.find('|')) + " no longer starts.";
                worstTiming = 1e6;
                addAt(check, then.value("t", 0.0));
                continue;
            }
            const auto heardAt = [](const Json& item) {
                const double onset = item.contains("onset_ms") && item["onset_ms"].is_number() ? item["onset_ms"].get<double>() : 0.0;
                return item.value("t", 0.0) * 1000.0 + onset;
            };
            const double moved = std::fabs(heardAt(found->second) - heardAt(then));
            if (moved > worstTiming) worstTiming = moved;
            if (moved > qa.regressionTimingMs) {
                addAt(check, found->second.value("t", 0.0));
                if (limited.empty()) limited = key.substr(0, key.find('|')) + " moved " + std::to_string((int)std::lround(moved)) + " ms.";
            }
        }
        const std::map<std::string, Json> markersThen = keyed(baseline.value("markers", Json::array()));
        const std::map<std::string, Json> markersNow = keyed(now.value("markers", Json::array()));
        for (const auto& [key, then] : markersThen) {
            auto found = markersNow.find(key);
            if (found == markersNow.end()) continue;
            const Json groups = then.value("bands_db", Json::object());
            const Json groupsNow = found->second.value("bands_db", Json::object());
            for (auto it = groups.begin(); it != groups.end(); ++it) {
                if (!groupsNow.contains(it.key())) continue;
                const Json& a = it.value();
                const Json& b = groupsNow[it.key()];
                for (size_t k = 0; k < a.size() && k < b.size(); k++) {
                    if (!a[k].is_number() || !b[k].is_number()) continue;
                    const double x = a[k].get<double>(), y = b[k].get<double>();
                    if (x < AUDIO_QA_FLOOR_DB && y < AUDIO_QA_FLOOR_DB) continue;
                    const double moved = std::fabs(x - y);
                    if (moved > worstBand) worstBand = moved;
                    if (moved > qa.regressionBandDb && limited.empty()) {
                        limited = "Group " + it.key() + " moved " + std::to_string(round2(moved)).substr(0, 5) + " dB in the " + AudioMeter::bandLabel((int)k) + " band at " + key.substr(0, key.find('|')) + ".";
                    }
                }
            }
        }
        check["measured"] = {{"timing_ms", round2(worstTiming)}, {"band_db", round2(worstBand)}};
        check["status"] = limited.empty() ? "PASS" : "FAIL";
        if (!limited.empty()) {
            check["limited_by"] = limited;
            check["fix_hint"] = "Undo what moved it, or save a new baseline if the change is meant.";
        }
        return check;
    }

}

const std::vector<std::string>& AudioQa::checkIds() {
    static const std::vector<std::string> ids = {"loudness", "true_peak", "clipping", "coverage", "sync", "audibility", "voice_budget", "ducking", "silence", "load_delay", "repetition", "regression"};
    return ids;
}

int64_t AudioQa::onsetFrame(const AudioSessionData& data, int64_t from, int64_t searchFrames, double marginDb) {
    const AudioAnalysis::Signal& audio = data.audio;
    if (from < 0 || from >= (int64_t)audio.frames || audio.channels == 0) return -1;
    const int64_t ms = std::max<int64_t>(1, data.rate / 1000);
    const auto loudest = [&](int64_t first, int64_t count) {
        float peak = 0.0f;
        for (unsigned int c = 0; c < audio.channels; c++) {
            const float* samples = audio.channel(c);
            for (int64_t i = std::max<int64_t>(first, 0); i < first + count && i < (int64_t)audio.frames; i++) peak = std::max(peak, std::fabs(samples[i]));
        }
        return peak;
    };
    const double margin = linear(marginDb);
    const double floor = linear(AUDIO_QA_FLOOR_DB);
    const int64_t end = std::min<int64_t>(from + searchFrames, (int64_t)audio.frames);
    for (int64_t at = from; at < end; at += ms) {
        const double before = loudest(at - 50 * ms, 50 * ms);
        const double threshold = std::max(before * margin, floor);
        if (loudest(at, ms) < threshold) continue;
        for (int64_t i = at; i < at + ms && i < (int64_t)audio.frames; i++) {
            for (unsigned int c = 0; c < audio.channels; c++) {
                if (std::fabs(audio.channel(c)[i]) >= threshold) return i;
            }
        }
    }
    return -1;
}

AudioQa::Json AudioQa::run(const AudioSessionData& data, const Request& request) {
    Context c{data, request};
    const AudioAnalysis::Signal& audio = data.audio;
    const size_t ms = std::max<size_t>(1, data.rate / 1000);
    c.frameOfMs = (int)ms;
    c.millis.assign(audio.frames / ms + 1, 0.0f);
    for (unsigned int ch = 0; ch < audio.channels; ch++) {
        const float* samples = audio.channel(ch);
        for (size_t i = 0; i < audio.frames; i++) {
            float& peak = c.millis[i / ms];
            peak = std::max(peak, std::fabs(samples[i]));
        }
    }
    for (const Json& line : data.events) {
        const std::string kind = line.value("kind", "");
        if (kind == "voice_start") {
            c.startOfVoice[line.value("voice", (int64_t)0)] = line;
            if (data.inRange(line)) c.starts.push_back(line);
        } else if (AudioSessionReader::isMarkerKind(kind) && data.inRange(line)) {
            c.markers.push_back(line);
        }
    }

    std::map<std::string, Json> results;
    std::vector<Json> matched;
    const auto wanted = [&](const char* id) {
        return request.checks.empty() || std::find(request.checks.begin(), request.checks.end(), id) != request.checks.end();
    };
    if (wanted("loudness")) results["loudness"] = checkLoudness(c);
    if (wanted("true_peak")) results["true_peak"] = checkTruePeak(c);
    if (wanted("clipping")) results["clipping"] = checkClipping(c);
    // sync measures what coverage matched
    if (wanted("coverage") || wanted("sync")) {
        Json coverage = checkCoverage(c, matched);
        if (wanted("coverage")) results["coverage"] = std::move(coverage);
    }
    if (wanted("sync")) results["sync"] = checkSync(c, matched);
    if (wanted("audibility")) results["audibility"] = checkAudibility(c);
    if (wanted("voice_budget")) results["voice_budget"] = checkVoiceBudget(c);
    if (wanted("ducking")) results["ducking"] = checkDucking(c);
    if (wanted("silence")) results["silence"] = checkSilence(c);
    if (wanted("load_delay")) results["load_delay"] = checkLoadDelay(c);
    if (wanted("repetition")) results["repetition"] = checkRepetition(c);
    if (wanted("regression")) results["regression"] = checkRegression(c);

    Json checks = Json::array();
    for (const std::string& id : checkIds()) {
        auto found = results.find(id);
        if (found != results.end()) checks.push_back(std::move(found->second));
    }
    const bool stop = qa::countRounds(checks, request.rounds, request.qa.fixRounds);
    const std::string verdict = qa::verdict(checks);

    // what a person should hear: the first of each failure, the worst margin, the loudest moment, each clip's first start
    Json listen = Json::array();
    const auto addListen = [&](double t, const std::string& why) {
        if ((int)listen.size() >= 5 || t < 0.0) return;
        for (const Json& moment : listen) {
            if (std::fabs(moment.value("t", 0.0) - t) < 0.1) return;
        }
        listen.push_back({{"t", std::round(t * 1000.0) / 1000.0}, {"why", why}});
    };
    for (const Json& check : checks) {
        if (check["status"] != "FAIL" || check["at"].empty()) continue;
        addListen(check["at"][0].get<double>(), check["id"].get<std::string>() + ": " + check["limited_by"].get<std::string>());
    }
    if (c.worstMarginAt >= 0.0) addListen(c.worstMarginAt, "the worst audibility margin: " + c.worstMarginClip);
    size_t loudestMs = 0;
    for (size_t m = 0; m < c.millis.size(); m++) {
        if (c.millis[m] > c.millis[loudestMs]) loudestMs = m;
    }
    if (audio.frames > 0) addListen(data.startSeconds + (double)loudestMs / 1000.0, "the loudest moment");
    std::set<std::string> clips;
    for (const Json& start : c.starts) {
        const std::string clip = start.value("clip", "");
        if (clips.insert(clip).second) addListen(data.seconds(start), "the first start of " + clip);
    }
    if (listen.size() < 3) addListen(data.startSeconds, "the start");
    if (listen.size() < 3) addListen((data.startSeconds + data.endSeconds) * 0.5, "the middle");
    if (listen.size() < 3) addListen(data.endSeconds, "the end");

    // the audit: what differs from the defaults
    const AudioQaSettings defaults;
    const AudioLoudnessTargets targetDefaults;
    qa::ThresholdAudit audit;
    for (const AudioQaField& field : audioQaFields()) {
        if (field.real) audit.add(field.key, request.qa.*field.real, defaults.*field.real);
        else audit.add(field.key, request.qa.*field.whole, defaults.*field.whole);
    }
    audit.add("audio.qa.music_group", request.qa.musicGroup, defaults.musicGroup);
    audit.add("audio.loudness_target_lufs", request.targets.targetLufs, targetDefaults.targetLufs);
    audit.add("audio.loudness_tolerance_lu", request.targets.toleranceLu, targetDefaults.toleranceLu);
    audit.add("audio.true_peak_ceiling_dbtp", request.targets.ceilingDbtp, targetDefaults.ceilingDbtp);

    const std::string guidance = qa::guidance(stop, verdict == "FAIL", request.qa.fixRounds, "capture again and check again",
                                              "Report these numbers; never claim a sound plays without a capture.");

    return {{"session", data.capture.id}, {"scene", data.capture.session.value("scene", "")},
            {"format", "jasperengine.audio-check"}, {"version", 1},
            {"verdict", verdict},
            {"range", {{"start", data.startSeconds}, {"end", data.endSeconds}}},
            {"checks", checks}, {"listen", listen}, {"thresholds", audit.json()}, {"guidance", guidance}};
}
