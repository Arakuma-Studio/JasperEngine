// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AudioTimelineImage.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    using Json = nlohmann::json;
    using Palette = AudioImages::Palette;
    using Lane = AudioImages::Lane;

    void append(AudioImages::Image& to, const AudioImages::Image& part) {
        to.width = part.width;
        to.height += part.height;
        to.rgba.insert(to.rgba.end(), part.rgba.begin(), part.rgba.end());
        to.markerLabels += part.markerLabels;
        to.eventLabels += part.eventLabels;
        to.hiddenLabels += part.hiddenLabels;
    }

    constexpr double GROUP_FLOOR_DB = -60.0;

}

AudioTimelineImage::Result AudioTimelineImage::render(const AudioSessionData& data, const Options& options, int width) {
    const double axis = std::max(data.endSeconds - data.startSeconds, 1e-3);

    AudioImages::Input input;
    input.signal = &data.audio;
    input.title = data.capture.id + "  " + data.capture.session.value("scene", std::string());
    input.startSeconds = data.startSeconds;
    input.axisSeconds = axis;
    input.momentary = data.momentary;
    input.shortTerm = data.shortTerm;
    input.truePeakDbtp = data.loudness.truePeakDb;
    input.markers = options.markers;
    input.events = options.events;

    Result result;
    result.lanes = Json::array();
    int y = 0;
    const auto addLane = [&](const std::string& name, int height) {
        result.lanes.push_back({{"name", name}, {"y", y}, {"height", height}});
        y += height;
    };
    addLane("title", AudioImages::laneHeight(Lane::Title));
    addLane("waveform", AudioImages::laneHeight(Lane::Waveform));
    addLane("spectrogram", AudioImages::laneHeight(Lane::Spectrogram));
    addLane("loudness", AudioImages::laneHeight(Lane::Loudness));
    append(result.image, AudioImages::render(input, {Lane::Title, Lane::Waveform, Lane::Spectrogram, Lane::Loudness}, width));

    // the groups that sound in the range
    std::map<std::string, std::vector<std::pair<double, double>>> groups;
    for (const Json& line : data.levels) {
        if (!data.inRange(line)) continue;
        const Json levels = line.value("groups", Json::object());
        for (auto it = levels.begin(); it != levels.end(); ++it) {
            groups[it.key()].push_back({data.seconds(line), it.value().value("peak_db", AudioMeter::SILENCE_DB)});
        }
    }
    int colour = 0;
    for (const auto& [name, points] : groups) {
        const Palette::Rgb rgb = Palette::BUS[(name == "Master" ? 0 : ++colour) % Palette::BUS_COUNT];
        const bool sounds = std::any_of(points.begin(), points.end(), [](const std::pair<double, double>& p) { return p.second > GROUP_FLOOR_DB; });
        if (!sounds) continue;
        AudioImages::Image lane;
        AudioImages::blank(lane, width, GROUP_LANE_HEIGHT);
        AudioImages::Canvas canvas(lane);
        for (size_t i = 0; i < points.size(); i++) {
            const double level = std::clamp((points[i].second - GROUP_FLOOR_DB) / -GROUP_FLOOR_DB, 0.0, 1.0);
            if (level <= 0.0) continue;
            // each line closes its 100 ms
            const int x0 = (int)std::floor((points[i].first - 0.1 - data.startSeconds) / axis * width);
            const int x1 = std::max(x0 + 1, (int)std::ceil((points[i].first - data.startSeconds) / axis * width));
            const int h = std::max(1, (int)std::lround(level * (GROUP_LANE_HEIGHT - 2)));
            canvas.fill(x0, GROUP_LANE_HEIGHT - 1 - h, x1 - x0, h, rgb);
        }
        canvas.text(2, 1, name, Palette::TEXT);
        append(result.image, lane);
        addLane("group:" + name, GROUP_LANE_HEIGHT);
    }

    addLane("markers", AudioImages::laneHeight(Lane::Markers));
    addLane("events", AudioImages::laneHeight(Lane::Events));
    append(result.image, AudioImages::render(input, {Lane::Markers, Lane::Events}, width));

    if (!options.failures.empty() || !options.warnings.empty() || !options.listen.empty()) {
        AudioImages::Image lane;
        AudioImages::blank(lane, width, CHECK_LANE_HEIGHT);
        AudioImages::Canvas canvas(lane);
        const auto tick = [&](const Tick& mark, Palette::Rgb rgb, int row) {
            const int x = (int)std::lround((mark.seconds - data.startSeconds) / axis * width);
            canvas.vline(x, 0, CHECK_LANE_HEIGHT - 1, rgb);
            canvas.text(x + 2, row, mark.text, rgb);
        };
        for (const Tick& mark : options.warnings) tick(mark, Palette::WARN, 2);
        for (const Tick& mark : options.failures) tick(mark, Palette::FAIL, 2);
        for (const Tick& mark : options.listen) {
            tick(mark, Palette::LISTEN, 13);
            result.image.listenLabels++;
        }
        append(result.image, lane);
        addLane("checks", CHECK_LANE_HEIGHT);
    }

    addLane("time", AudioImages::laneHeight(Lane::TimeAxis));
    append(result.image, AudioImages::render(input, {Lane::TimeAxis}, width));
    return result;
}
