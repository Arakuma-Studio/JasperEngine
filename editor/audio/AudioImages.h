// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "AudioAnalysis.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    // Pictures of sound for agents that cannot hear: RGBA8 images
    // made of lanes stacked top to bottom, every one as wide as the image, encoded as PNG. Labels
    // use the public-domain font8x8_basic glyphs. Plain functions, safe on any thread.
    class AudioImages {
    public:
        // The images' colours. Theme::Colors' waveform colours are
        // set from them (Theme::apply), so the editor's waveforms and the images the tools return
        // agree and the PNGs never depend on ImGui state. The last group is the editor's own: the
        // Sound Browser's overlays, which no image draws.
        struct Palette {
            struct Rgb {
                unsigned char r, g, b;
            };
            static constexpr Rgb TITLE_BACKGROUND = {36, 36, 44};
            static constexpr Rgb LANE_BACKGROUND = {20, 20, 24};
            static constexpr Rgb AXIS_BACKGROUND = {28, 28, 34};
            static constexpr Rgb TEXT = {220, 220, 228};
            static constexpr Rgb DIM_TEXT = {150, 150, 162};
            static constexpr Rgb GRID = {52, 52, 62};
            static constexpr Rgb CENTRE_LINE = {70, 70, 82};
            static constexpr Rgb WAVE = {86, 156, 214};
            static constexpr Rgb WAVE_RMS = {170, 214, 250};
            static constexpr Rgb CLIPPED = {235, 64, 52};
            static constexpr Rgb MOMENTARY = {80, 210, 200};
            static constexpr Rgb SHORT_TERM = {240, 200, 80};
            static constexpr Rgb ZERO_DBTP = {200, 70, 70};
            static constexpr Rgb TRUE_PEAK = {250, 140, 60};
            static constexpr Rgb MARKER = {120, 220, 120};
            static constexpr Rgb EVENT_START = {240, 200, 80};
            static constexpr Rgb EVENT_STOP = {200, 150, 90};
            static constexpr Rgb LABEL_BACKGROUND = {44, 44, 54};
            // the part of a timeline after its audio (a truncated or a live capture): hatched
            static constexpr Rgb NO_AUDIO = {32, 30, 30};
            static constexpr Rgb NO_AUDIO_HATCH = {58, 50, 50};

            // the editor's overlays: the trimmed-away parts (drawn over the waveform at 60%), the
            // loop region's flags, the trim and fade handles, onset ticks, the peak marker, and the
            // type badge behind a card's type name
            static constexpr Rgb TRIM_SHADE = {12, 12, 14};
            static constexpr Rgb LOOP = {110, 200, 110};
            static constexpr Rgb HANDLE = {236, 236, 242};
            static constexpr Rgb ONSET = {196, 150, 232};
            static constexpr Rgb PEAK = TRUE_PEAK;
            static constexpr Rgb TYPE_BADGE = {62, 84, 112};

            // bus chips and lanes: distinct hues at one lightness,
            // none of them a meter colour; Master is BUS[0], user bus i BUS[(i + 1) % 8] unless it
            // has its own colour (AudioMixerWindow::busColor). Theme::Colors::BusPalette reads them.
            static constexpr int BUS_COUNT = 8;
            static constexpr Rgb BUS[BUS_COUNT] = {
                {214, 166, 74},   // amber (Master)
                {76, 157, 224},   // sky
                {178, 127, 219},  // lavender
                {62, 189, 175},   // teal
                {224, 114, 124},  // rose
                {147, 191, 74},   // lime
                {226, 144, 82},   // apricot
                {114, 135, 224},  // periwinkle
            };
            // a check's verdict (Theme's MeterLow, MeterMid and MeterHigh hues) and the listen
            // moments (the marker green)
            static constexpr Rgb PASS = {79, 179, 92};
            static constexpr Rgb WARN = {227, 193, 75};
            static constexpr Rgb FAIL = {227, 83, 63};
            static constexpr Rgb LISTEN = MARKER;
        };

        enum class Lane {
            Title,        // 16 px: n, path or session id, duration
            Waveform,     // 128: min/max per column and channel, RMS, dBFS grid, clipped columns; a
                          // quiet one zoomed by a power of two (labelled, at most +36 dB)
            Spectrogram,  // 160: log frequency, magma, -90..0 dBFS
            Loudness,     // 96: momentary and short-term LUFS, the 0 dBTP line and the true peak
            Markers,      // 24: M<n> at marker and input times
            Events,       // 24: E<n> at voice starts (filled) and stops (open)
            TimeAxis      // 16: seconds
        };

        // a numbered label on the markers or events lane; stop draws the open triangle
        struct Label {
            double seconds = 0.0;
            std::string text;
            bool stop = false;
        };

        struct Input {
            const AudioAnalysis::Signal* signal = nullptr;
            std::string title;
            // the time of the signal's first frame on the axis (a capture range's start)
            double startSeconds = 0.0;
            // the axis length when it is longer than the signal (a report range whose audio stops
            // at a truncation cap, or is not flushed yet): the lanes after the audio are hatched
            // "no audio", labels keep their times. 0: the signal's length.
            double axisSeconds = 0.0;
            // LUFS every AudioMeter::CURVE_STEP, the point i ending at (i + 1) steps; NaN undefined
            std::vector<double> momentary;
            std::vector<double> shortTerm;
            double truePeakDbtp = AudioMeter::SILENCE_DB;
            std::vector<Label> markers;
            std::vector<Label> events;
        };

        struct Image {
            int width = 0;
            int height = 0;
            std::vector<unsigned char> rgba;
            // labels drawn on the markers and events lanes, and those that did not fit (+k)
            int markerLabels = 0;
            int eventLabels = 0;
            int hiddenLabels = 0;
            // L<n> listen moments drawn (AudioTimelineImage)
            int listenLabels = 0;
        };

        struct SheetItem {
            const AudioAnalysis::Signal* signal = nullptr;
            // "n stem"
            std::string label;
        };

        static constexpr int MIN_WIDTH = 256;
        static constexpr int MAX_WIDTH = 2048;
        static constexpr int SHEET_GAP = 8;
        static constexpr int SHEET_LABEL_HEIGHT = 14;
        static constexpr int SHEET_MAX_COLUMNS = 4;

        static int laneHeight(Lane lane);
        // file images: "waveform", "spectrogram" or "both"; capture images add the loudness lane;
        // the audio_session report timeline has every lane. Empty for another mode.
        static std::vector<Lane> fileLanes(const std::string& mode);
        static std::vector<Lane> captureLanes(const std::string& mode);
        static std::vector<Lane> timelineLanes();
        static int height(const std::vector<Lane>& lanes);

        static Image render(const Input& input, const std::vector<Lane>& lanes, int width);

        // a grid of min(4, items) columns: each tile a label, a waveform and a spectrogram
        static Image sheet(const std::vector<SheetItem>& items, int width);
        static int sheetColumns(int items);
        static int sheetTileWidth(int items, int width);
        static int sheetHeight(int items, int width);

        // A fit (generate_sound like): title 16, the bands as paired bars 120 (the reference grey, the
        // result in the accent colour, -60..0 dB), both envelopes from the head 96, time axis 16: 248 px
        struct CompareInput {
            std::string title;
            std::vector<double> referenceBandsDb;
            std::vector<double> resultBandsDb;
            // centre of each band, Hz
            std::vector<double> bandHz;
            // dB re the loudest frame, one point per frameSeconds
            std::vector<double> referenceEnvelopeDb;
            std::vector<double> resultEnvelopeDb;
            double frameSeconds = 0.005;
        };
        static Image renderCompare(const CompareInput& input, int width);

        // A file's slices (edit_sound_event slice preview): title 16, the waveform 128, the slices 24
        // (each shaded and numbered S<n>, a red tick on an edge that needs a fade), time axis 16: 184 px
        struct SliceMark {
            double startSeconds = 0.0;
            double endSeconds = 0.0;
            bool fadeIn = false;
            bool fadeOut = false;
        };
        static Image renderSlices(const AudioAnalysis::Signal& signal, const std::string& title, const std::vector<SliceMark>& slices, int width);

        // PNG (stbi_write_png_to_func), base64; false when encoding fails
        static bool encodePng(const Image& image, std::string& base64);

        // -- drawing primitives, for images made of other lanes (S35's timeline image): the font8x8 text and the lanes these images draw

        // the glyphs' size, px (one line of text)
        static constexpr int GLYPH = 8;

        // Draws into an image; everything outside it is clipped
        class Canvas {
        public:
            explicit Canvas(Image& image): image(image) {}

            int width() const { return image.width; }
            int height() const { return image.height; }
            void pixel(int x, int y, Palette::Rgb c);
            void fill(int x, int y, int w, int h, Palette::Rgb c);
            void hline(int x0, int x1, int y, Palette::Rgb c);
            void vline(int x, int y0, int y1, Palette::Rgb c);
            void line(int x0, int y0, int x1, int y1, Palette::Rgb c);
            // printable ASCII (others draw as '?'), inside [clipLeft, clipRight) (-1: the width)
            void text(int x, int y, const std::string& s, Palette::Rgb c, int clipLeft = 0, int clipRight = -1);

        private:
            Image& image;
        };

        // width x height of LANE_BACKGROUND
        static void blank(Image& image, int width, int height);
        static int textWidth(const std::string& s);
        // the text that fits in width px, cut at the front with "..." when needed
        static std::string fitText(const std::string& s, int width);
        // the title lane's background and text
        static void drawTitle(Canvas& canvas, int x0, int y0, int w, int h, const std::string& title);
        // the waveform lane (min/max per column and channel, RMS, the dBFS grid, clipped columns
        // ticked red, a quiet signal zoomed)
        static void drawWaveform(Canvas& canvas, int x0, int y0, int w, int h, const AudioAnalysis::Signal& signal);
        // columns without audio: hatched, "no audio" written when `label`
        static void drawNoAudio(Canvas& canvas, int x0, int y0, int w, int h, bool label);
        // the time axis of [start, start + duration) seconds over the whole width
        static void drawTimeAxis(Canvas& canvas, int y0, int w, int h, double start, double duration);
        // the markers lane's labels (M<n>): up to three rows, "+k" for those that
        // fit in none; drawn and hidden count them
        static void drawMarkerLabels(Canvas& canvas, int y0, int w, int h, const std::vector<Label>& labels, double start,
                                     double duration, int& drawn, int& hidden);
    };

}
