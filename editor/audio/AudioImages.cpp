// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AudioImages.h"

#include "util/Base64.h"

#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

using Rgb = AudioImages::Palette::Rgb;

// Colours: the palette Theme reads (AudioImages::Palette)
constexpr Rgb TITLE_BACKGROUND = AudioImages::Palette::TITLE_BACKGROUND;
constexpr Rgb LANE_BACKGROUND = AudioImages::Palette::LANE_BACKGROUND;
constexpr Rgb AXIS_BACKGROUND = AudioImages::Palette::AXIS_BACKGROUND;
constexpr Rgb TEXT = AudioImages::Palette::TEXT;
constexpr Rgb DIM_TEXT = AudioImages::Palette::DIM_TEXT;
constexpr Rgb GRID = AudioImages::Palette::GRID;
constexpr Rgb CENTRE_LINE = AudioImages::Palette::CENTRE_LINE;
constexpr Rgb WAVE = AudioImages::Palette::WAVE;
constexpr Rgb WAVE_RMS = AudioImages::Palette::WAVE_RMS;
constexpr Rgb CLIPPED = AudioImages::Palette::CLIPPED;
constexpr Rgb MOMENTARY = AudioImages::Palette::MOMENTARY;
constexpr Rgb SHORT_TERM = AudioImages::Palette::SHORT_TERM;
constexpr Rgb ZERO_DBTP = AudioImages::Palette::ZERO_DBTP;
constexpr Rgb TRUE_PEAK = AudioImages::Palette::TRUE_PEAK;
constexpr Rgb MARKER = AudioImages::Palette::MARKER;
constexpr Rgb EVENT_START = AudioImages::Palette::EVENT_START;
constexpr Rgb EVENT_STOP = AudioImages::Palette::EVENT_STOP;
constexpr Rgb LABEL_BACKGROUND = AudioImages::Palette::LABEL_BACKGROUND;
constexpr Rgb NO_AUDIO = AudioImages::Palette::NO_AUDIO;
constexpr Rgb NO_AUDIO_HATCH = AudioImages::Palette::NO_AUDIO_HATCH;

// The loudness lane's scale, LUFS / dBTP from bottom to top
const double LOUDNESS_FLOOR = -60.0;
const double LOUDNESS_TOP = 6.0;
// The spectrogram's level scale and frequency range
const double SPECTRUM_FLOOR_DB = -90.0;
const double SPECTRUM_LOW_HZ = 20.0;
const double SPECTRUM_HIGH_HZ = 20000.0;
constexpr int GLYPH = AudioImages::GLYPH;
const int LABEL_ROWS = 3;
// A quiet waveform is drawn zoomed by a power of two, so its peak fills at least half its lane
// (at most this many times: +36 dB); the lane then says so ("x16 +24 dB")
const int WAVEFORM_MAX_ZOOM = 64;

// font8x8_basic, U+0020 to U+007E: public domain, by Daniel Hepper (github.com/dhepper/font8x8),
// after Marcel Sondaar's public-domain IBM VGA font. Row r of a glyph is byte r, bit 0 leftmost.
const unsigned char FONT[95][8] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},  // U+0020
    {0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00},  // U+0021
    {0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},  // U+0022
    {0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00},  // U+0023
    {0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00},  // U+0024
    {0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00},  // U+0025
    {0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00},  // U+0026
    {0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00},  // U+0027
    {0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00},  // U+0028
    {0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00},  // U+0029
    {0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00},  // U+002A
    {0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00},  // U+002B
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x06},  // U+002C
    {0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00},  // U+002D
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00},  // U+002E
    {0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00},  // U+002F
    {0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00},  // U+0030
    {0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00},  // U+0031
    {0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00},  // U+0032
    {0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00},  // U+0033
    {0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00},  // U+0034
    {0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00},  // U+0035
    {0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00},  // U+0036
    {0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00},  // U+0037
    {0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00},  // U+0038
    {0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00},  // U+0039
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00},  // U+003A
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x06},  // U+003B
    {0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00},  // U+003C
    {0x00, 0x00, 0x3F, 0x00, 0x00, 0x3F, 0x00, 0x00},  // U+003D
    {0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00},  // U+003E
    {0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00},  // U+003F
    {0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0x00},  // U+0040
    {0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00},  // U+0041
    {0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00},  // U+0042
    {0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00},  // U+0043
    {0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00},  // U+0044
    {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00},  // U+0045
    {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00},  // U+0046
    {0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00},  // U+0047
    {0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00},  // U+0048
    {0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},  // U+0049
    {0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00},  // U+004A
    {0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00},  // U+004B
    {0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00},  // U+004C
    {0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00},  // U+004D
    {0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00},  // U+004E
    {0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00},  // U+004F
    {0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00},  // U+0050
    {0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00},  // U+0051
    {0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00},  // U+0052
    {0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00},  // U+0053
    {0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},  // U+0054
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00},  // U+0055
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00},  // U+0056
    {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00},  // U+0057
    {0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00},  // U+0058
    {0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00},  // U+0059
    {0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00},  // U+005A
    {0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0x00},  // U+005B
    {0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00},  // U+005C
    {0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0x00},  // U+005D
    {0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00},  // U+005E
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF},  // U+005F
    {0x0C, 0x0C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00},  // U+0060
    {0x00, 0x00, 0x1E, 0x30, 0x3E, 0x33, 0x6E, 0x00},  // U+0061
    {0x07, 0x06, 0x06, 0x3E, 0x66, 0x66, 0x3B, 0x00},  // U+0062
    {0x00, 0x00, 0x1E, 0x33, 0x03, 0x33, 0x1E, 0x00},  // U+0063
    {0x38, 0x30, 0x30, 0x3E, 0x33, 0x33, 0x6E, 0x00},  // U+0064
    {0x00, 0x00, 0x1E, 0x33, 0x3F, 0x03, 0x1E, 0x00},  // U+0065
    {0x1C, 0x36, 0x06, 0x0F, 0x06, 0x06, 0x0F, 0x00},  // U+0066
    {0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x1F},  // U+0067
    {0x07, 0x06, 0x36, 0x6E, 0x66, 0x66, 0x67, 0x00},  // U+0068
    {0x0C, 0x00, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},  // U+0069
    {0x30, 0x00, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E},  // U+006A
    {0x07, 0x06, 0x66, 0x36, 0x1E, 0x36, 0x67, 0x00},  // U+006B
    {0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},  // U+006C
    {0x00, 0x00, 0x33, 0x7F, 0x7F, 0x6B, 0x63, 0x00},  // U+006D
    {0x00, 0x00, 0x1F, 0x33, 0x33, 0x33, 0x33, 0x00},  // U+006E
    {0x00, 0x00, 0x1E, 0x33, 0x33, 0x33, 0x1E, 0x00},  // U+006F
    {0x00, 0x00, 0x3B, 0x66, 0x66, 0x3E, 0x06, 0x0F},  // U+0070
    {0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x78},  // U+0071
    {0x00, 0x00, 0x3B, 0x6E, 0x66, 0x06, 0x0F, 0x00},  // U+0072
    {0x00, 0x00, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x00},  // U+0073
    {0x08, 0x0C, 0x3E, 0x0C, 0x0C, 0x2C, 0x18, 0x00},  // U+0074
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x33, 0x6E, 0x00},  // U+0075
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00},  // U+0076
    {0x00, 0x00, 0x63, 0x6B, 0x7F, 0x7F, 0x36, 0x00},  // U+0077
    {0x00, 0x00, 0x63, 0x36, 0x1C, 0x36, 0x63, 0x00},  // U+0078
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x3E, 0x30, 0x1F},  // U+0079
    {0x00, 0x00, 0x3F, 0x19, 0x0C, 0x26, 0x3F, 0x00},  // U+007A
    {0x38, 0x0C, 0x0C, 0x07, 0x0C, 0x0C, 0x38, 0x00},  // U+007B
    {0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00},  // U+007C
    {0x07, 0x0C, 0x0C, 0x38, 0x0C, 0x0C, 0x07, 0x00},  // U+007D
    {0x6E, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},  // U+007E
};

// matplotlib's magma colour map (256 entries, RGB), CC0, by Nathaniel J. Smith, Stefan van der
// Walt and Eric Firing
const unsigned char MAGMA[256 * 3] = {
    0x00,0x00,0x04, 0x01,0x00,0x05, 0x01,0x01,0x06, 0x01,0x01,0x08, 0x02,0x01,0x09, 0x02,0x02,0x0B, 0x02,0x02,0x0D, 0x03,0x03,0x0F,
    0x03,0x03,0x12, 0x04,0x04,0x14, 0x05,0x04,0x16, 0x06,0x05,0x18, 0x06,0x05,0x1A, 0x07,0x06,0x1C, 0x08,0x07,0x1E, 0x09,0x07,0x20,
    0x0A,0x08,0x22, 0x0B,0x09,0x24, 0x0C,0x09,0x26, 0x0D,0x0A,0x29, 0x0E,0x0B,0x2B, 0x10,0x0B,0x2D, 0x11,0x0C,0x2F, 0x12,0x0D,0x31,
    0x13,0x0D,0x34, 0x14,0x0E,0x36, 0x15,0x0E,0x38, 0x16,0x0F,0x3B, 0x18,0x0F,0x3D, 0x19,0x10,0x3F, 0x1A,0x10,0x42, 0x1C,0x10,0x44,
    0x1D,0x11,0x47, 0x1E,0x11,0x49, 0x20,0x11,0x4B, 0x21,0x11,0x4E, 0x22,0x11,0x50, 0x24,0x12,0x53, 0x25,0x12,0x55, 0x27,0x12,0x58,
    0x29,0x11,0x5A, 0x2A,0x11,0x5C, 0x2C,0x11,0x5F, 0x2D,0x11,0x61, 0x2F,0x11,0x63, 0x31,0x11,0x65, 0x33,0x10,0x67, 0x34,0x10,0x69,
    0x36,0x10,0x6B, 0x38,0x10,0x6C, 0x39,0x0F,0x6E, 0x3B,0x0F,0x70, 0x3D,0x0F,0x71, 0x3F,0x0F,0x72, 0x40,0x0F,0x74, 0x42,0x0F,0x75,
    0x44,0x0F,0x76, 0x45,0x10,0x77, 0x47,0x10,0x78, 0x49,0x10,0x78, 0x4A,0x10,0x79, 0x4C,0x11,0x7A, 0x4E,0x11,0x7B, 0x4F,0x12,0x7B,
    0x51,0x12,0x7C, 0x52,0x13,0x7C, 0x54,0x13,0x7D, 0x56,0x14,0x7D, 0x57,0x15,0x7E, 0x59,0x15,0x7E, 0x5A,0x16,0x7E, 0x5C,0x16,0x7F,
    0x5D,0x17,0x7F, 0x5F,0x18,0x7F, 0x60,0x18,0x80, 0x62,0x19,0x80, 0x64,0x1A,0x80, 0x65,0x1A,0x80, 0x67,0x1B,0x80, 0x68,0x1C,0x81,
    0x6A,0x1C,0x81, 0x6B,0x1D,0x81, 0x6D,0x1D,0x81, 0x6E,0x1E,0x81, 0x70,0x1F,0x81, 0x72,0x1F,0x81, 0x73,0x20,0x81, 0x75,0x21,0x81,
    0x76,0x21,0x81, 0x78,0x22,0x81, 0x79,0x22,0x82, 0x7B,0x23,0x82, 0x7C,0x23,0x82, 0x7E,0x24,0x82, 0x80,0x25,0x82, 0x81,0x25,0x81,
    0x83,0x26,0x81, 0x84,0x26,0x81, 0x86,0x27,0x81, 0x88,0x27,0x81, 0x89,0x28,0x81, 0x8B,0x29,0x81, 0x8C,0x29,0x81, 0x8E,0x2A,0x81,
    0x90,0x2A,0x81, 0x91,0x2B,0x81, 0x93,0x2B,0x80, 0x94,0x2C,0x80, 0x96,0x2C,0x80, 0x98,0x2D,0x80, 0x99,0x2D,0x80, 0x9B,0x2E,0x7F,
    0x9C,0x2E,0x7F, 0x9E,0x2F,0x7F, 0xA0,0x2F,0x7F, 0xA1,0x30,0x7E, 0xA3,0x30,0x7E, 0xA5,0x31,0x7E, 0xA6,0x31,0x7D, 0xA8,0x32,0x7D,
    0xAA,0x33,0x7D, 0xAB,0x33,0x7C, 0xAD,0x34,0x7C, 0xAE,0x34,0x7B, 0xB0,0x35,0x7B, 0xB2,0x35,0x7B, 0xB3,0x36,0x7A, 0xB5,0x36,0x7A,
    0xB7,0x37,0x79, 0xB8,0x37,0x79, 0xBA,0x38,0x78, 0xBC,0x39,0x78, 0xBD,0x39,0x77, 0xBF,0x3A,0x77, 0xC0,0x3A,0x76, 0xC2,0x3B,0x75,
    0xC4,0x3C,0x75, 0xC5,0x3C,0x74, 0xC7,0x3D,0x73, 0xC8,0x3E,0x73, 0xCA,0x3E,0x72, 0xCC,0x3F,0x71, 0xCD,0x40,0x71, 0xCF,0x40,0x70,
    0xD0,0x41,0x6F, 0xD2,0x42,0x6F, 0xD3,0x43,0x6E, 0xD5,0x44,0x6D, 0xD6,0x45,0x6C, 0xD8,0x45,0x6C, 0xD9,0x46,0x6B, 0xDB,0x47,0x6A,
    0xDC,0x48,0x69, 0xDE,0x49,0x68, 0xDF,0x4A,0x68, 0xE0,0x4C,0x67, 0xE2,0x4D,0x66, 0xE3,0x4E,0x65, 0xE4,0x4F,0x64, 0xE5,0x50,0x64,
    0xE7,0x52,0x63, 0xE8,0x53,0x62, 0xE9,0x54,0x62, 0xEA,0x56,0x61, 0xEB,0x57,0x60, 0xEC,0x58,0x60, 0xED,0x5A,0x5F, 0xEE,0x5B,0x5E,
    0xEF,0x5D,0x5E, 0xF0,0x5F,0x5E, 0xF1,0x60,0x5D, 0xF2,0x62,0x5D, 0xF2,0x64,0x5C, 0xF3,0x65,0x5C, 0xF4,0x67,0x5C, 0xF4,0x69,0x5C,
    0xF5,0x6B,0x5C, 0xF6,0x6C,0x5C, 0xF6,0x6E,0x5C, 0xF7,0x70,0x5C, 0xF7,0x72,0x5C, 0xF8,0x74,0x5C, 0xF8,0x76,0x5C, 0xF9,0x78,0x5D,
    0xF9,0x79,0x5D, 0xF9,0x7B,0x5D, 0xFA,0x7D,0x5E, 0xFA,0x7F,0x5E, 0xFA,0x81,0x5F, 0xFB,0x83,0x5F, 0xFB,0x85,0x60, 0xFB,0x87,0x61,
    0xFC,0x89,0x61, 0xFC,0x8A,0x62, 0xFC,0x8C,0x63, 0xFC,0x8E,0x64, 0xFC,0x90,0x65, 0xFD,0x92,0x66, 0xFD,0x94,0x67, 0xFD,0x96,0x68,
    0xFD,0x98,0x69, 0xFD,0x9A,0x6A, 0xFD,0x9B,0x6B, 0xFE,0x9D,0x6C, 0xFE,0x9F,0x6D, 0xFE,0xA1,0x6E, 0xFE,0xA3,0x6F, 0xFE,0xA5,0x71,
    0xFE,0xA7,0x72, 0xFE,0xA9,0x73, 0xFE,0xAA,0x74, 0xFE,0xAC,0x76, 0xFE,0xAE,0x77, 0xFE,0xB0,0x78, 0xFE,0xB2,0x7A, 0xFE,0xB4,0x7B,
    0xFE,0xB6,0x7C, 0xFE,0xB7,0x7E, 0xFE,0xB9,0x7F, 0xFE,0xBB,0x81, 0xFE,0xBD,0x82, 0xFE,0xBF,0x84, 0xFE,0xC1,0x85, 0xFE,0xC2,0x87,
    0xFE,0xC4,0x88, 0xFE,0xC6,0x8A, 0xFE,0xC8,0x8C, 0xFE,0xCA,0x8D, 0xFE,0xCC,0x8F, 0xFE,0xCD,0x90, 0xFE,0xCF,0x92, 0xFE,0xD1,0x94,
    0xFE,0xD3,0x95, 0xFE,0xD5,0x97, 0xFE,0xD7,0x99, 0xFE,0xD8,0x9A, 0xFD,0xDA,0x9C, 0xFD,0xDC,0x9E, 0xFD,0xDE,0xA0, 0xFD,0xE0,0xA1,
    0xFD,0xE2,0xA3, 0xFD,0xE3,0xA5, 0xFD,0xE5,0xA7, 0xFD,0xE7,0xA9, 0xFD,0xE9,0xAA, 0xFD,0xEB,0xAC, 0xFC,0xEC,0xAE, 0xFC,0xEE,0xB0,
    0xFC,0xF0,0xB2, 0xFC,0xF2,0xB4, 0xFC,0xF4,0xB6, 0xFC,0xF6,0xB8, 0xFC,0xF7,0xB9, 0xFC,0xF9,0xBB, 0xFC,0xFB,0xBD, 0xFC,0xFD,0xBF,
};

using Canvas = AudioImages::Canvas;

int textWidth(const std::string& s) {
    return (int)s.size() * GLYPH;
}

Rgb magma(double share) {
    const int i = std::clamp((int)std::lround(share * 255.0), 0, 255);
    return {MAGMA[i * 3], MAGMA[i * 3 + 1], MAGMA[i * 3 + 2]};
}

// the text that fits in width px, cut with "..." when needed
std::string fitText(const std::string& s, int width) {
    const int chars = std::max(0, width / GLYPH);
    if ((int)s.size() <= chars) return s;
    if (chars <= 3) return s.substr(0, (size_t)chars);
    return "..." + s.substr(s.size() - (size_t)(chars - 3));
}

std::string formatNumber(const char* format, double value) {
    char text[64];
    std::snprintf(text, sizeof(text), format, value);
    return text;
}

void drawTitle(Canvas& canvas, int x0, int y0, int w, int h, const std::string& title) {
    canvas.fill(x0, y0, w, h, TITLE_BACKGROUND);
    canvas.text(x0 + 4, y0 + (h - GLYPH) / 2, fitText(title, w - 8), TEXT, x0, x0 + w);
}

void drawWaveform(Canvas& canvas, int x0, int y0, int w, int h, const AudioAnalysis::Signal& signal) {
    canvas.fill(x0, y0, w, h, LANE_BACKGROUND);
    const unsigned int channels = std::max(1u, signal.channels);
    const size_t n = signal.frames;
    // linear amplitude, zoomed for a quiet signal: a mix at -30 dBFS would be 2 px tall
    float largest = 0.0f;
    for (float v : signal.samples) largest = std::max(largest, std::fabs(v));
    int zoom = 1;
    while (largest > 0.0f && zoom < WAVEFORM_MAX_ZOOM && (double)largest * zoom * 2.0 <= 1.0) zoom *= 2;
    const double grid[3] = {std::pow(10.0, -6.0 / 20.0), std::pow(10.0, -12.0 / 20.0), std::pow(10.0, -24.0 / 20.0)};
    for (unsigned int c = 0; c < channels; c++) {
        const int top = y0 + (int)((long)h * c / channels);
        const int bottom = y0 + (int)((long)h * (c + 1) / channels);
        const int sliceHeight = bottom - top;
        const double centre = top + (sliceHeight - 1) / 2.0;
        const double half = std::max(1.0, (sliceHeight - 2) / 2.0);
        auto yOf = [&](double v) { return (int)std::lround(centre - std::clamp(v * zoom, -1.0, 1.0) * half); };
        for (double g : grid) {
            canvas.hline(x0, x0 + w - 1, yOf(g / zoom), GRID);
            canvas.hline(x0, x0 + w - 1, yOf(-g / zoom), GRID);
        }
        canvas.hline(x0, x0 + w - 1, yOf(0.0), CENTRE_LINE);
        if (c > 0) canvas.hline(x0, x0 + w - 1, top, CENTRE_LINE);
        if (n == 0 || c >= signal.channels) continue;

        const float* x = signal.channel(c);
        for (int col = 0; col < w; col++) {
            const size_t from = (size_t)((double)col * n / w);
            const size_t to = std::min(n, std::max(from + 1, (size_t)((double)(col + 1) * n / w)));
            float lo = x[from], hi = x[from];
            double square = 0.0;
            bool clipped = false;
            for (size_t i = from; i < to; i++) {
                lo = std::min(lo, x[i]);
                hi = std::max(hi, x[i]);
                square += (double)x[i] * x[i];
                if (std::fabs(x[i]) >= AudioMeter::CLIP_LEVEL) clipped = true;
            }
            const double rms = std::sqrt(square / (double)(to - from));
            canvas.vline(x0 + col, yOf(hi), yOf(lo), WAVE);
            canvas.vline(x0 + col, yOf(std::min((double)hi, rms)), yOf(std::max((double)lo, -rms)), WAVE_RMS);
            if (clipped) canvas.vline(x0 + col, top, top + 3, CLIPPED);
        }
    }
    if (zoom > 1) {
        const std::string label = "x" + std::to_string(zoom) + " +" + std::to_string((int)std::lround(20.0 * std::log10((double)zoom))) + " dB";
        const int x = std::max(x0, x0 + w - textWidth(label) - 3);
        canvas.fill(x - 1, y0 + 1, std::min(textWidth(label) + 2, w), GLYPH, LABEL_BACKGROUND);
        canvas.text(x, y0 + 1, label, DIM_TEXT, x0, x0 + w);
    }
}

// a lane's columns from x0 on have no audio: hatched, "no audio" in the waveform lane
void drawNoAudio(Canvas& canvas, int x0, int y0, int w, int h, bool label) {
    if (w <= 0) return;
    canvas.fill(x0, y0, w, h, NO_AUDIO);
    for (int x = x0; x < x0 + w; x++) {
        for (int y = y0; y < y0 + h; y++) {
            if ((x + y) % 8 == 0) canvas.pixel(x, y, NO_AUDIO_HATCH);
        }
    }
    const std::string text = "no audio";
    if (label && w >= textWidth(text) + 8) {
        const int x = x0 + (w - textWidth(text)) / 2;
        const int y = y0 + (h - GLYPH) / 2;
        canvas.fill(x - 2, y - 1, textWidth(text) + 4, GLYPH + 2, LABEL_BACKGROUND);
        canvas.text(x, y, text, DIM_TEXT);
    }
}

void drawSpectrogram(Canvas& canvas, int x0, int y0, int w, int h, const AudioAnalysis::Signal& signal, bool labels) {
    canvas.fill(x0, y0, w, h, magma(0.0));
    const size_t n = signal.frames;
    const double rate = signal.sampleRate;
    const double high = std::min(SPECTRUM_HIGH_HZ, rate / 2.0);
    const double low = SPECTRUM_LOW_HZ;
    if (n > 0 && rate > 0 && high > low) {
        const std::vector<float> mono = signal.mono();
        const int bins = AudioAnalysis::FFT_SIZE / 2 + 1;
        const size_t frames = AudioAnalysis::spectralFrames(n);
        // a column is the mean power of the frames whose centre falls in it
        std::vector<float> power((size_t)w * bins, 0.0f);
        std::vector<int> count(w, 0);
        std::vector<float> magnitude(bins);
        for (size_t f = 0; f < frames; f++) {
            const double centre = std::min((double)n - 1, (double)f * AudioAnalysis::FFT_HOP + AudioAnalysis::FFT_SIZE / 2.0);
            const int col = std::clamp((int)(centre * w / (double)n), 0, w - 1);
            AudioAnalysis::magnitudes(mono.data(), n, f, magnitude.data());
            float* column = &power[(size_t)col * bins];
            for (int k = 0; k < bins; k++) column[k] += magnitude[k] * magnitude[k];
            count[col]++;
        }
        // columns without a frame of their own show the nearest one that has
        std::vector<int> source(w, -1);
        for (int col = 0; col < w; col++) {
            if (count[col] > 0) source[col] = col;
        }
        for (int col = 0; col < w; col++) {
            if (source[col] >= 0) continue;
            int best = -1;
            for (int d = 1; d < w && best < 0; d++) {
                if (col - d >= 0 && count[col - d] > 0) best = col - d;
                else if (col + d < w && count[col + d] > 0) best = col + d;
            }
            source[col] = best;
        }
        // |X| of a full-scale sine through the Hann window: FFT_SIZE / 4
        const double reference = (AudioAnalysis::FFT_SIZE / 4.0) * (AudioAnalysis::FFT_SIZE / 4.0);
        const double binHz = rate / AudioAnalysis::FFT_SIZE;
        const double ratio = high / low;
        for (int row = 0; row < h; row++) {
            const double fLow = low * std::pow(ratio, (double)row / h);
            const double fHigh = low * std::pow(ratio, (double)(row + 1) / h);
            const int k0 = std::clamp((int)std::floor(fLow / binHz + 0.5), 0, bins - 1);
            const int k1 = std::clamp((int)std::floor(fHigh / binHz + 0.5), k0, bins - 1);
            const int y = y0 + h - 1 - row;
            for (int col = 0; col < w; col++) {
                if (source[col] < 0) continue;
                const float* column = &power[(size_t)source[col] * bins];
                double p = 0.0;
                for (int k = k0; k <= k1; k++) p = std::max(p, (double)column[k]);
                p /= count[source[col]];
                const double db = p > 0.0 ? 10.0 * std::log10(p / reference) : SPECTRUM_FLOOR_DB;
                if (db <= SPECTRUM_FLOOR_DB) continue;
                canvas.pixel(x0 + col, y, magma((db - SPECTRUM_FLOOR_DB) / -SPECTRUM_FLOOR_DB));
            }
        }
    }
    // labelled ticks on the left
    const double ticks[] = {50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};
    const char* names[] = {"50", "100", "200", "500", "1k", "2k", "5k", "10k", "20k"};
    int lastLabel = 1 << 30;
    for (int i = 0; i < 9; i++) {
        if (ticks[i] < low || ticks[i] > high) continue;
        const int y = y0 + h - 1 - (int)std::lround(std::log(ticks[i] / low) / std::log(high / low) * (h - 1));
        canvas.hline(x0, x0 + 3, y, TEXT);
        if (labels && lastLabel - y >= GLYPH + 1 && y - GLYPH / 2 >= y0 && y + GLYPH / 2 <= y0 + h) {
            canvas.fill(x0 + 5, y - GLYPH / 2, textWidth(names[i]) + 2, GLYPH, LABEL_BACKGROUND);
            canvas.text(x0 + 6, y - GLYPH / 2, names[i], DIM_TEXT);
            lastLabel = y;
        }
    }
}

void drawLoudness(Canvas& canvas, int y0, int w, int h, const AudioImages::Input& input, double duration) {
    canvas.fill(0, y0, w, h, LANE_BACKGROUND);
    auto yOf = [&](double level) {
        const double v = std::clamp(level, LOUDNESS_FLOOR, LOUDNESS_TOP);
        return y0 + (int)std::lround((LOUDNESS_TOP - v) / (LOUDNESS_TOP - LOUDNESS_FLOOR) * (h - 1));
    };
    for (int level = -10; level >= -40; level -= 10) {
        canvas.hline(0, w - 1, yOf(level), GRID);
        canvas.text(2, yOf(level) - GLYPH - 1 >= y0 ? yOf(level) - GLYPH : yOf(level) + 1, std::to_string(level), DIM_TEXT);
    }
    canvas.hline(0, w - 1, yOf(0.0), ZERO_DBTP);
    if (input.truePeakDbtp > LOUDNESS_FLOOR) {
        const int y = yOf(input.truePeakDbtp);
        for (int x = 0; x < w; x++) {
            if ((x / 6) % 2 == 0) canvas.pixel(x, y, TRUE_PEAK);
        }
    }
    if (duration <= 0.0) return;
    auto drawCurve = [&](const std::vector<double>& curve, Rgb colour) {
        bool have = false;
        int px = 0, py = 0;
        for (size_t i = 0; i < curve.size(); i++) {
            if (!std::isfinite(curve[i])) {
                have = false;
                continue;
            }
            const double t = (double)(i + 1) * AudioMeter::CURVE_STEP;
            const int x = std::clamp((int)std::lround(t / duration * w) - 1, 0, w - 1);
            const int y = yOf(curve[i]);
            if (have) canvas.line(px, py, x, y, colour);
            else canvas.pixel(x, y, colour);
            have = true;
            px = x;
            py = y;
        }
    };
    drawCurve(input.shortTerm, SHORT_TERM);
    drawCurve(input.momentary, MOMENTARY);
}

// Labels in up to three rows of 8 px; a label that fits in no row is counted and "+k" shows them
void drawLabels(Canvas& canvas, int y0, int w, int h, const std::vector<AudioImages::Label>& labels, double start,
                double duration, bool events, int& drawn, int& hidden) {
    canvas.fill(0, y0, w, h, LANE_BACKGROUND);
    const int rows = std::max(1, std::min(LABEL_ROWS, h / GLYPH));
    std::vector<int> rowEnd(rows, -1);
    std::vector<const AudioImages::Label*> sorted;
    for (const AudioImages::Label& label : labels) sorted.push_back(&label);
    std::stable_sort(sorted.begin(), sorted.end(), [](const AudioImages::Label* a, const AudioImages::Label* b) {
        return a->seconds < b->seconds;
    });
    const int markWidth = events ? 6 : 3;
    int lost = 0;
    for (const AudioImages::Label* label : sorted) {
        const double t = duration > 0.0 ? (label->seconds - start) / duration : 0.0;
        const int x = std::clamp((int)std::lround(t * w), 0, w - 1);
        const int width = markWidth + textWidth(label->text);
        const int left = std::min(x, std::max(0, w - width));
        int row = -1;
        for (int r = 0; r < rows; r++) {
            if (rowEnd[r] < left) {
                row = r;
                break;
            }
        }
        if (row < 0) {
            lost++;
            continue;
        }
        const int y = y0 + (h - rows * GLYPH) / 2 + row * GLYPH;
        rowEnd[row] = left + width + 1;
        const Rgb colour = events ? (label->stop ? EVENT_STOP : EVENT_START) : MARKER;
        if (events) {
            // a triangle pointing down at x: filled for a start, small and open for a stop
            if (label->stop) {
                canvas.hline(x - 2, x + 2, y + 1, colour);
                canvas.line(x - 2, y + 1, x, y + 4, colour);
                canvas.line(x + 2, y + 1, x, y + 4, colour);
            } else {
                for (int k = 0; k < 4; k++) canvas.hline(x - (3 - k), x + (3 - k), y + 1 + k, colour);
            }
        } else {
            canvas.vline(x, y, y + GLYPH - 1, colour);
        }
        canvas.text(left + markWidth - (events ? 2 : 1), y, label->text, colour);
        drawn++;
    }
    if (lost > 0) {
        const std::string more = "+" + std::to_string(lost);
        const int x = w - textWidth(more) - 2;
        const int y = y0 + h - GLYPH;
        canvas.fill(x - 1, y, textWidth(more) + 3, GLYPH, LABEL_BACKGROUND);
        canvas.text(x, y, more, TEXT);
    }
    hidden += lost;
}

void drawTimeAxis(Canvas& canvas, int y0, int w, int h, double start, double duration) {
    canvas.fill(0, y0, w, h, AXIS_BACKGROUND);
    canvas.hline(0, w - 1, y0, GRID);
    if (duration <= 0.0) return;
    // 1, 2, 5 x 10^k seconds, at least 64 px apart
    double step = 1e-4;
    for (int i = 0; step / duration * w < 64.0 && i < 40; i++) {
        const double base = std::pow(10.0, std::floor(std::log10(step) + 1e-9));
        const double lead = step / base;
        step = lead < 1.5 ? 2.0 * base : (lead < 3.5 ? 5.0 * base : 10.0 * base);
    }
    const int decimals = std::max(0, (int)std::ceil(-std::log10(step) - 1e-9));
    const double first = std::ceil(start / step - 1e-9) * step;
    for (double t = first; t <= start + duration + 1e-9; t += step) {
        const int x = std::clamp((int)std::lround((t - start) / duration * w), 0, w - 1);
        canvas.vline(x, y0, y0 + 3, TEXT);
        const double shown = std::fabs(t) < step * 1e-6 ? 0.0 : t;
        std::string text = formatNumber(("%." + std::to_string(decimals) + "f").c_str(), shown) + "s";
        const int left = std::clamp(x - textWidth(text) / 2, 0, std::max(0, w - textWidth(text)));
        canvas.text(left, y0 + h - GLYPH - 1, text, DIM_TEXT);
    }
}

void blank(AudioImages::Image& image, int width, int height) {
    image.width = width;
    image.height = height;
    image.rgba.assign((size_t)width * height * 4, 255);
    for (size_t i = 0; i < image.rgba.size(); i += 4) {
        image.rgba[i] = LANE_BACKGROUND.r;
        image.rgba[i + 1] = LANE_BACKGROUND.g;
        image.rgba[i + 2] = LANE_BACKGROUND.b;
    }
}

} // namespace

// -- the drawing primitives

void AudioImages::Canvas::pixel(int x, int y, Rgb c) {
    if (x < 0 || y < 0 || x >= image.width || y >= image.height) return;
    unsigned char* p = &image.rgba[((size_t)y * image.width + x) * 4];
    p[0] = c.r;
    p[1] = c.g;
    p[2] = c.b;
    p[3] = 255;
}

void AudioImages::Canvas::fill(int x, int y, int w, int h, Rgb c) {
    for (int j = std::max(0, y); j < std::min(image.height, y + h); j++) {
        for (int i = std::max(0, x); i < std::min(image.width, x + w); i++) pixel(i, j, c);
    }
}

void AudioImages::Canvas::hline(int x0, int x1, int y, Rgb c) {
    for (int x = std::min(x0, x1); x <= std::max(x0, x1); x++) pixel(x, y, c);
}

void AudioImages::Canvas::vline(int x, int y0, int y1, Rgb c) {
    for (int y = std::min(y0, y1); y <= std::max(y0, y1); y++) pixel(x, y, c);
}

void AudioImages::Canvas::line(int x0, int y0, int x1, int y1, Rgb c) {
    const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    while (true) {
        pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * error;
        if (e2 >= dy) {
            error += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

void AudioImages::Canvas::text(int x, int y, const std::string& s, Rgb c, int clipLeft, int clipRight) {
    if (clipRight < 0) clipRight = image.width;
    for (size_t i = 0; i < s.size(); i++) {
        const unsigned char ch = (unsigned char)s[i];
        const int index = (ch >= 0x20 && ch <= 0x7E) ? ch - 0x20 : '?' - 0x20;
        const int gx = x + (int)i * GLYPH;
        for (int row = 0; row < GLYPH; row++) {
            const unsigned char bits = FONT[index][row];
            for (int col = 0; col < GLYPH; col++) {
                const int px = gx + col;
                if ((bits >> col) & 1 && px >= clipLeft && px < clipRight) pixel(px, y + row, c);
            }
        }
    }
}

void AudioImages::blank(Image& image, int width, int height) {
    ::blank(image, width, height);
}

int AudioImages::textWidth(const std::string& s) {
    return ::textWidth(s);
}

std::string AudioImages::fitText(const std::string& s, int width) {
    return ::fitText(s, width);
}

void AudioImages::drawTitle(Canvas& canvas, int x0, int y0, int w, int h, const std::string& title) {
    ::drawTitle(canvas, x0, y0, w, h, title);
}

void AudioImages::drawWaveform(Canvas& canvas, int x0, int y0, int w, int h, const AudioAnalysis::Signal& signal) {
    ::drawWaveform(canvas, x0, y0, w, h, signal);
}

void AudioImages::drawNoAudio(Canvas& canvas, int x0, int y0, int w, int h, bool label) {
    ::drawNoAudio(canvas, x0, y0, w, h, label);
}

void AudioImages::drawTimeAxis(Canvas& canvas, int y0, int w, int h, double start, double duration) {
    ::drawTimeAxis(canvas, y0, w, h, start, duration);
}

void AudioImages::drawMarkerLabels(Canvas& canvas, int y0, int w, int h, const std::vector<Label>& labels, double start,
                                   double duration, int& drawn, int& hidden) {
    ::drawLabels(canvas, y0, w, h, labels, start, duration, false, drawn, hidden);
}

int AudioImages::laneHeight(Lane lane) {
    switch (lane) {
        case Lane::Title: return 16;
        case Lane::Waveform: return 128;
        case Lane::Spectrogram: return 160;
        case Lane::Loudness: return 96;
        case Lane::Markers: return 24;
        case Lane::Events: return 24;
        case Lane::TimeAxis: return 16;
    }
    return 0;
}

std::vector<AudioImages::Lane> AudioImages::fileLanes(const std::string& mode) {
    if (mode == "waveform") return {Lane::Title, Lane::Waveform, Lane::TimeAxis};
    if (mode == "spectrogram") return {Lane::Title, Lane::Spectrogram, Lane::TimeAxis};
    if (mode == "both") return {Lane::Title, Lane::Waveform, Lane::Spectrogram, Lane::TimeAxis};
    return {};
}

std::vector<AudioImages::Lane> AudioImages::captureLanes(const std::string& mode) {
    if (mode == "waveform") return {Lane::Title, Lane::Waveform, Lane::Loudness, Lane::TimeAxis};
    if (mode == "spectrogram") return {Lane::Title, Lane::Spectrogram, Lane::Loudness, Lane::TimeAxis};
    if (mode == "both") return {Lane::Title, Lane::Waveform, Lane::Spectrogram, Lane::Loudness, Lane::TimeAxis};
    return {};
}

std::vector<AudioImages::Lane> AudioImages::timelineLanes() {
    return {Lane::Title, Lane::Waveform, Lane::Spectrogram, Lane::Loudness, Lane::Markers, Lane::Events, Lane::TimeAxis};
}

int AudioImages::height(const std::vector<Lane>& lanes) {
    int total = 0;
    for (Lane lane : lanes) total += laneHeight(lane);
    return total;
}

AudioImages::Image AudioImages::render(const Input& input, const std::vector<Lane>& lanes, int width) {
    Image image;
    width = std::clamp(width, MIN_WIDTH, MAX_WIDTH);
    blank(image, width, height(lanes));
    Canvas canvas(image);
    static const AudioAnalysis::Signal empty;
    const AudioAnalysis::Signal& signal = input.signal ? *input.signal : empty;
    // the axis: the signal, or a longer range whose audio stops early
    const double duration = std::max(input.axisSeconds, signal.seconds());
    const int audioWidth = duration > 0.0 ? std::clamp((int)std::lround(signal.seconds() / duration * width), 0, width) : width;
    int y = 0;
    for (Lane lane : lanes) {
        const int h = laneHeight(lane);
        switch (lane) {
            case Lane::Title: drawTitle(canvas, 0, y, width, h, input.title); break;
            case Lane::Waveform:
                if (audioWidth > 0) drawWaveform(canvas, 0, y, audioWidth, h, signal);
                drawNoAudio(canvas, audioWidth, y, width - audioWidth, h, true);
                break;
            case Lane::Spectrogram:
                if (audioWidth > 0) drawSpectrogram(canvas, 0, y, audioWidth, h, signal, true);
                drawNoAudio(canvas, audioWidth, y, width - audioWidth, h, false);
                break;
            case Lane::Loudness:
                drawLoudness(canvas, y, width, h, input, duration);
                drawNoAudio(canvas, audioWidth, y, width - audioWidth, h, false);
                break;
            case Lane::Markers:
                drawLabels(canvas, y, width, h, input.markers, input.startSeconds, duration, false, image.markerLabels, image.hiddenLabels);
                break;
            case Lane::Events:
                drawLabels(canvas, y, width, h, input.events, input.startSeconds, duration, true, image.eventLabels, image.hiddenLabels);
                break;
            case Lane::TimeAxis: drawTimeAxis(canvas, y, width, h, input.startSeconds, duration); break;
        }
        y += h;
    }
    return image;
}

int AudioImages::sheetColumns(int items) {
    return std::clamp(items, 1, SHEET_MAX_COLUMNS);
}

int AudioImages::sheetTileWidth(int items, int width) {
    const int cols = sheetColumns(items);
    return (width - SHEET_GAP * (cols + 1)) / cols;
}

int AudioImages::sheetHeight(int items, int width) {
    const int cols = sheetColumns(items);
    const int rows = (std::max(1, items) + cols - 1) / cols;
    const int lane = (int)std::lround(sheetTileWidth(items, width) / 4.0);
    return SHEET_GAP + rows * (SHEET_LABEL_HEIGHT + 2 * lane + SHEET_GAP);
}

AudioImages::Image AudioImages::renderCompare(const CompareInput& input, int width) {
    const int TITLE = 16, BALANCE = 120, ENVELOPE = 96, AXIS = 16;
    const double FLOOR = -60.0;
    Image image;
    width = std::clamp(width, MIN_WIDTH, MAX_WIDTH);
    blank(image, width, TITLE + BALANCE + ENVELOPE + AXIS);
    Canvas canvas(image);
    drawTitle(canvas, 0, 0, width, TITLE, input.title);

    auto yOf = [FLOOR](double db, int y0, int h) { return y0 + (int)std::lround(std::clamp(db / FLOOR, 0.0, 1.0) * (h - 1)); };

    // -- balance
    const int balanceY = TITLE;
    canvas.fill(0, balanceY, width, BALANCE, LANE_BACKGROUND);
    for (double db : {-20.0, -40.0}) canvas.hline(0, width - 1, yOf(db, balanceY, BALANCE), GRID);
    const int bands = (int)std::min(input.referenceBandsDb.size(), input.resultBandsDb.size());
    for (int b = 0; b < bands; b++) {
        const int x0 = b * width / std::max(bands, 1);
        const int x1 = (b + 1) * width / std::max(bands, 1);
        const int bar = std::max(1, (x1 - x0 - 2) / 2);
        const int yr = yOf(input.referenceBandsDb[(size_t)b], balanceY, BALANCE);
        const int yc = yOf(input.resultBandsDb[(size_t)b], balanceY, BALANCE);
        canvas.fill(x0 + 1, yr, bar, balanceY + BALANCE - yr, DIM_TEXT);
        canvas.fill(x0 + 1 + bar, yc, bar, balanceY + BALANCE - yc, WAVE);
    }
    canvas.text(4, balanceY + 2, "bands: reference grey, result colour", TEXT);

    // -- envelopes
    const int envelopeY = TITLE + BALANCE;
    canvas.fill(0, envelopeY, width, ENVELOPE, LANE_BACKGROUND);
    for (double db : {-20.0, -40.0}) canvas.hline(0, width - 1, yOf(db, envelopeY, ENVELOPE), GRID);
    const size_t points = std::max(input.referenceEnvelopeDb.size(), input.resultEnvelopeDb.size());
    const double duration = (double)points * input.frameSeconds;
    auto curve = [&](const std::vector<double>& db, Rgb colour) {
        for (size_t k = 1; k < db.size() && points > 1; k++) {
            const int x0 = (int)((k - 1) * (size_t)(width - 1) / (points - 1));
            const int x1 = (int)(k * (size_t)(width - 1) / (points - 1));
            canvas.line(x0, yOf(db[k - 1], envelopeY, ENVELOPE), x1, yOf(db[k], envelopeY, ENVELOPE), colour);
        }
    };
    curve(input.referenceEnvelopeDb, DIM_TEXT);
    curve(input.resultEnvelopeDb, WAVE);
    canvas.text(4, envelopeY + 2, "envelope from the head", TEXT);

    drawTimeAxis(canvas, TITLE + BALANCE + ENVELOPE, width, AXIS, 0.0, duration);
    return image;
}

AudioImages::Image AudioImages::renderSlices(const AudioAnalysis::Signal& signal, const std::string& title, const std::vector<SliceMark>& slices, int width) {
    const int TITLE = 16, WAVEFORM = 128, SLICES = 24, AXIS = 16;
    Image image;
    width = std::clamp(width, MIN_WIDTH, MAX_WIDTH);
    blank(image, width, TITLE + WAVEFORM + SLICES + AXIS);
    Canvas canvas(image);
    drawTitle(canvas, 0, 0, width, TITLE, title);
    drawWaveform(canvas, 0, TITLE, width, WAVEFORM, signal);
    const int y = TITLE + WAVEFORM;
    canvas.fill(0, y, width, SLICES, LANE_BACKGROUND);
    const double duration = std::max(signal.seconds(), 1e-6);
    int n = 0;
    for (const SliceMark& slice : slices) {
        n++;
        const int x0 = (int)std::lround(slice.startSeconds / duration * (width - 1));
        const int x1 = std::max(x0 + 1, (int)std::lround(slice.endSeconds / duration * (width - 1)));
        canvas.fill(x0, y + 2, x1 - x0, SLICES - 4, n % 2 == 0 ? GRID : LABEL_BACKGROUND);
        canvas.text(x0 + 2, y + (SLICES - GLYPH) / 2, "S" + std::to_string(n), TEXT, x0, x1);
        if (slice.fadeIn) canvas.vline(x0, y, y + SLICES - 1, CLIPPED);
        if (slice.fadeOut) canvas.vline(x1 - 1, y, y + SLICES - 1, CLIPPED);
    }
    drawTimeAxis(canvas, y + SLICES, width, AXIS, 0.0, signal.seconds());
    return image;
}

AudioImages::Image AudioImages::sheet(const std::vector<SheetItem>& items, int width) {
    Image image;
    width = std::clamp(width, MIN_WIDTH, MAX_WIDTH);
    const int count = (int)items.size();
    blank(image, width, sheetHeight(count, width));
    Canvas canvas(image);
    canvas.fill(0, 0, image.width, image.height, AXIS_BACKGROUND);
    const int cols = sheetColumns(count);
    const int tile = sheetTileWidth(count, width);
    const int lane = (int)std::lround(tile / 4.0);
    const int tileHeight = SHEET_LABEL_HEIGHT + 2 * lane;
    static const AudioAnalysis::Signal empty;
    for (int i = 0; i < count; i++) {
        const int x = SHEET_GAP + (i % cols) * (tile + SHEET_GAP);
        const int y = SHEET_GAP + (i / cols) * (tileHeight + SHEET_GAP);
        const AudioAnalysis::Signal& signal = items[i].signal ? *items[i].signal : empty;
        drawTitle(canvas, x, y, tile, SHEET_LABEL_HEIGHT, items[i].label);
        drawWaveform(canvas, x, y + SHEET_LABEL_HEIGHT, tile, lane, signal);
        drawSpectrogram(canvas, x, y + SHEET_LABEL_HEIGHT + lane, tile, lane, signal, lane >= 96);
    }
    return image;
}

bool AudioImages::encodePng(const Image& image, std::string& base64) {
    if (image.width <= 0 || image.height <= 0) return false;
    std::string png;
    auto append = [](void* context, void* data, int size) {
        static_cast<std::string*>(context)->append(static_cast<const char*>(data), static_cast<size_t>(size));
    };
    if (!stbi_write_png_to_func(append, &png, image.width, image.height, 4, image.rgba.data(), image.width * 4)) return false;
    base64 = Base64::encode(reinterpret_cast<const unsigned char*>(png.data()), png.size());
    return true;
}
