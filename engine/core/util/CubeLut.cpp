// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
// Portions ported from OpenEngine, Copyright (c) 2026 Gamecrafters Guild Limited and contributors (MIT):
// Engine/Modules/Rendering/Source/Common/CubeLutFileParser.cpp (the parser), CubeLutColorDomain.cpp
// (the encoding proposal) and Shaders/Includes/cube_lut.glsl (the curves and gamut matrices)

#include "CubeLut.h"

#include "Log.h"
#include "io/Data.h"
#include "io/FileData.h"
#include "util/PostColorLut.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

using namespace jasperengine;

namespace{
    const int MAX_1D_SIZE = 65536;
    const int MAX_3D_SIZE = 129;
    const float GAMMA = 2.2f;

    // 0..1, NaN to 0 (a table index is taken from it)
    float saturate(float v){
        return v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f;
    }

    Vector3 maxZero(const Vector3& v){
        return Vector3(std::max(v.x, 0.0f), std::max(v.y, 0.0f), std::max(v.z, 0.0f));
    }

    // ---- .cube text

    bool isBlank(char c){
        return c == ' ' || c == '\t' || c == '\r';
    }

    std::string_view trim(std::string_view s){
        while (!s.empty() && isBlank(s.front()))
            s.remove_prefix(1);
        while (!s.empty() && isBlank(s.back()))
            s.remove_suffix(1);
        return s;
    }

    bool startsWith(std::string_view s, std::string_view prefix){
        return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
    }

    // the next blank-separated word of s, s left after it
    std::string_view nextWord(std::string_view& s){
        while (!s.empty() && isBlank(s.front()))
            s.remove_prefix(1);
        size_t end = 0;
        while (end < s.size() && !isBlank(s[end]))
            end++;
        const std::string_view word = s.substr(0, end);
        s.remove_prefix(end);
        return word;
    }

    // a decimal number (sign, digits, fraction, exponent) that is the whole word, read the same
    // in every locale (strtof and streams follow the C locale's decimal point)
    bool parseNumber(std::string_view word, float& value){
        static const double POW10[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
                                       1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
        size_t i = 0;
        const size_t n = word.size();
        bool negative = false;
        if (i < n && (word[i] == '+' || word[i] == '-')){
            negative = word[i] == '-';
            i++;
        }
        uint64_t mantissa = 0;
        int digits = 0;
        int exponent = 0;
        bool any = false;
        for (; i < n && word[i] >= '0' && word[i] <= '9'; i++){
            any = true;
            if (digits < 19){
                mantissa = mantissa * 10 + (uint64_t)(word[i] - '0');
                if (mantissa != 0)
                    digits++;
            }else{
                exponent++;
            }
        }
        if (i < n && word[i] == '.'){
            for (i++; i < n && word[i] >= '0' && word[i] <= '9'; i++){
                any = true;
                if (digits < 19){
                    mantissa = mantissa * 10 + (uint64_t)(word[i] - '0');
                    if (mantissa != 0)
                        digits++;
                    exponent--;
                }
            }
        }
        if (!any)
            return false;
        if (i < n && (word[i] == 'e' || word[i] == 'E')){
            i++;
            bool negativeExponent = false;
            if (i < n && (word[i] == '+' || word[i] == '-')){
                negativeExponent = word[i] == '-';
                i++;
            }
            int e = 0;
            bool exponentDigits = false;
            for (; i < n && word[i] >= '0' && word[i] <= '9'; i++){
                exponentDigits = true;
                e = std::min(e * 10 + (word[i] - '0'), 1000);
            }
            if (!exponentDigits)
                return false;
            exponent += negativeExponent ? -e : e;
        }
        if (i != n)
            return false;
        double v = (double)mantissa;
        if (mantissa != 0){
            if (exponent >= 0 && exponent <= 22)
                v *= POW10[exponent];
            else if (exponent < 0 && exponent >= -22)
                v /= POW10[-exponent];
            else
                v *= std::pow(10.0, (double)exponent);
        }
        value = (float)(negative ? -v : v);
        return std::isfinite(value);
    }

    // count numbers from the start of s (anything after them is ignored, as .cube readers do)
    bool parseNumbers(std::string_view s, float* values, int count){
        for (int i = 0; i < count; i++){
            if (!parseNumber(nextWord(s), values[i]))
                return false;
        }
        return true;
    }

    // a table size: the first word, a whole number (huge ones capped past every limit)
    bool parseCount(std::string_view s, int& value){
        const std::string_view word = nextWord(s);
        if (word.empty())
            return false;
        int v = 0;
        for (const char c : word){
            if (c < '0' || c > '9')
                return false;
            v = std::min(v * 10 + (c - '0'), MAX_1D_SIZE + 1);
        }
        value = v;
        return true;
    }

    enum class Header{
        NONE,       // not a header line: the data starts
        PARSED,
        FAILED      // a malformed header line (message set)
    };

    // one header line (TITLE, LUT_1D/3D_SIZE, LUT_1D/3D_INPUT_RANGE, DOMAIN_MIN/MAX) into the LUT
    Header parseHeader(std::string_view line, CubeLut& lut, std::string& message){
        if (startsWith(line, "TITLE")){
            // the quoted text, or the rest of the line when it is not quoted
            std::string_view rest = trim(line.substr(5));
            if (rest.size() >= 2 && rest.front() == '"' && rest.back() == '"')
                rest = rest.substr(1, rest.size() - 2);
            lut.title.assign(rest);
            return Header::PARSED;
        }
        std::string_view args = line;
        const std::string_view key = nextWord(args);
        if (key == "LUT_1D_SIZE" || key == "LUT_3D_SIZE"){
            const bool is3D = key == "LUT_3D_SIZE";
            int n = 0;
            if (!parseCount(args, n)){
                message = "Invalid " + std::string(key) + " line.";
                return Header::FAILED;
            }
            const int maxSize = is3D ? MAX_3D_SIZE : MAX_1D_SIZE;
            if (n < 2 || n > maxSize){
                message = std::string(key) + " out of supported range (2-" + std::to_string(maxSize) + ").";
                return Header::FAILED;
            }
            (is3D ? lut.size3D : lut.size1D) = n;
            return Header::PARSED;
        }
        if (key == "LUT_1D_INPUT_RANGE" || key == "LUT_3D_INPUT_RANGE"){
            float range[2];
            if (!parseNumbers(args, range, 2)){
                message = "Invalid " + std::string(key) + " line.";
                return Header::FAILED;
            }
            float* target = key == "LUT_3D_INPUT_RANGE" ? lut.in3D : lut.in1D;
            target[0] = range[0];
            target[1] = range[1];
            return Header::PARSED;
        }
        if (key == "DOMAIN_MIN" || key == "DOMAIN_MAX"){
            float rgb[3];
            if (!parseNumbers(args, rgb, 3)){
                message = "Invalid DOMAIN_MIN/DOMAIN_MAX line.";
                return Header::FAILED;
            }
            if (std::fabs(rgb[0] - rgb[1]) > 1e-6f || std::fabs(rgb[0] - rgb[2]) > 1e-6f){
                message = "Per-channel DOMAIN_MIN/DOMAIN_MAX is not supported.";
                return Header::FAILED;
            }
            const int end = key == "DOMAIN_MIN" ? 0 : 1;
            lut.in1D[end] = rgb[0];
            lut.in3D[end] = rgb[0];
            return Header::PARSED;
        }
        return Header::NONE;
    }

    // ---- sampling

    // the 3D table at normalised coordinates (0..1 per axis), trilinear
    Vector3 sample3D(const CubeLut& lut, float tr, float tg, float tb){
        const int n = lut.size3D;
        const float fr = saturate(tr) * (n - 1);
        const float fg = saturate(tg) * (n - 1);
        const float fb = saturate(tb) * (n - 1);
        const int r0 = std::min((int)fr, n - 2), g0 = std::min((int)fg, n - 2), b0 = std::min((int)fb, n - 2);
        const float wr = fr - r0, wg = fg - g0, wb = fb - b0;
        const size_t strideG = (size_t)n * 3;
        const size_t strideB = (size_t)n * n * 3;
        const float* base = lut.lut3D.data() + ((size_t)b0 * n * n + (size_t)g0 * n + (size_t)r0) * 3;
        float out[3];
        for (int c = 0; c < 3; c++){
            const float* p = base + c;
            const float c00 = p[0] + (p[3] - p[0]) * wr;
            const float c10 = p[strideG] + (p[strideG + 3] - p[strideG]) * wr;
            const float c01 = p[strideB] + (p[strideB + 3] - p[strideB]) * wr;
            const float c11 = p[strideB + strideG] + (p[strideB + strideG + 3] - p[strideB + strideG]) * wr;
            const float c0 = c00 + (c10 - c00) * wg;
            const float c1 = c01 + (c11 - c01) * wg;
            out[c] = c0 + (c1 - c0) * wb;
        }
        return Vector3(out[0], out[1], out[2]);
    }

    // a channel of the 1D table at a normalised coordinate (0..1), linear
    float sample1D(const CubeLut& lut, int channel, float t){
        const int n = lut.size1D;
        const float f = saturate(t) * (n - 1);
        const int i0 = std::min((int)f, n - 2);
        const float w = f - i0;
        const float a = lut.lut1D[(size_t)i0 * 3 + channel];
        const float b = lut.lut1D[(size_t)(i0 + 1) * 3 + channel];
        return a + (b - a) * w;
    }

    float normalise(float v, const float range[2]){
        return (v - range[0]) / std::max(range[1] - range[0], 1e-6f);
    }

    // ---- colour encodings (cube_lut.glsl)

    // rows of a 3x3 matrix times a vector
    Vector3 mul(const float m[9], const Vector3& v){
        return Vector3(m[0] * v.x + m[1] * v.y + m[2] * v.z,
                       m[3] * v.x + m[4] * v.y + m[5] * v.z,
                       m[6] * v.x + m[7] * v.y + m[8] * v.z);
    }

    // linear Rec. 709 <-> ARRI Wide Gamut 3 (LogC3)
    const float REC709_TO_AWG3[9] = {
        0.631320638f, 0.270800406f, 0.097878818f,
        0.036819917f, 0.793037090f, 0.170143018f,
        0.017369856f, 0.148789076f, 0.833840896f
    };
    const float AWG3_TO_REC709[9] = {
        1.617524436f, -0.537286235f, -0.080238194f,
        -0.070572684f, 1.334612859f, -0.264040156f,
        -0.021102017f, -0.226953556f, 1.248055903f
    };
    // linear Rec. 709 <-> DaVinci Wide Gamut (DaVinci Intermediate)
    const float REC709_TO_DWG[9] = {
        0.562767460f, 0.323516518f, 0.113715973f,
        0.077754626f, 0.749577398f, 0.172668004f,
        0.064669185f, 0.191998705f, 0.743332142f
    };
    const float DWG_TO_REC709[9] = {
        1.898614861f, -0.792176192f, -0.106438765f,
        -0.168948765f, 1.488975755f, -0.320026913f,
        -0.121539126f, -0.315675802f, 1.437214990f
    };
    // linear Rec. 709 <-> ACES AP1 (ACEScct), Bradford D65 <-> D60: the ACES / OCIO values
    // (cube_lut.glsl's pair is off by 1e-4 from inverting each other)
    const float REC709_TO_AP1[9] = {
        0.6130974024f, 0.3395231462f, 0.0473794514f,
        0.0701937225f, 0.9163538791f, 0.0134523985f,
        0.0206155929f, 0.1095697729f, 0.8698146342f
    };
    const float AP1_TO_REC709[9] = {
        1.7050509927f, -0.6217921207f, -0.0832588720f,
        -0.1302564175f, 1.1408047366f, -0.0105483191f,
        -0.0240033568f, -0.1289689761f, 1.1529723329f
    };

    float linearToSrgb(float x){
        x = std::max(x, 0.0f);
        return x <= 0.0031308f ? 12.92f * x : 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
    }

    float srgbToLinear(float x){
        x = std::max(x, 0.0f);
        return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
    }

    // the engine's display encoding (pow 1/2.2 of linear) to piecewise sRGB and back, one pow
    // each way: 1.055 (e^2.2)^(1/2.4) - 0.055 = 1.055 e^(2.2/2.4) - 0.055
    const float ENGINE_SRGB_CUT = std::pow(0.0031308f, 1.0f / GAMMA);

    float engineToSrgb(float e){
        e = std::max(e, 0.0f);
        return e <= ENGINE_SRGB_CUT ? 12.92f * std::pow(e, GAMMA) : 1.055f * std::pow(e, GAMMA / 2.4f) - 0.055f;
    }

    float srgbToEngine(float s){
        s = std::max(s, 0.0f);
        return s <= 0.04045f ? std::pow(s / 12.92f, 1.0f / GAMMA) : std::pow((s + 0.055f) / 1.055f, 2.4f / GAMMA);
    }

    // ARRI LogC3 (EI 800)
    float linearToLogC3(float x){
        x = std::max(x, 0.0f);
        return x > 0.010591f ? 0.247190f * std::log10(5.555556f * x + 0.052272f) + 0.385537f : 5.367655f * x + 0.092809f;
    }

    float logC3ToLinear(float x){
        return x > 0.149658f ? (std::pow(10.0f, (x - 0.385537f) / 0.247190f) - 0.052272f) / 5.555556f : (x - 0.092809f) / 5.367655f;
    }

    // DaVinci Intermediate as Blackmagic publishes it: x * M below the linear cut 0.00262409,
    // (log2(x + A) + B) * C above it, A = 0.0075, B = 7, C = 0.07329248, M = 10.44426855; the
    // two meet at the encoded cut 0.02740668
    float linearToDaVinci(float x){
        x = std::max(x, 0.0f);
        return x <= 0.00262409f ? x * 10.44426855f : (std::log2(x + 0.0075f) + 7.0f) * 0.07329248f;
    }

    float daVinciToLinear(float x){
        return x <= 0.02740668f ? x / 10.44426855f : std::exp2(x / 0.07329248f - 7.0f) - 0.0075f;
    }

    float linearToAcesCct(float x){
        x = std::max(x, 0.0f);
        return x <= 0.0078125f ? 10.540237742f * x + 0.072905534f : (std::log2(x) + 9.72f) / 17.52f;
    }

    float acesCctToLinear(float x){
        return x <= 0.155251142f ? (x - 0.072905534f) / 10.540237742f : std::exp2(x * 17.52f - 9.72f);
    }

    // Cineon film log: reference white 685, black 95, 0.002 density per code value, gamma 0.6
    const float CINEON_DENSITY_PER_CODE = 0.002f / 0.6f;
    const float CINEON_BLACK_OFFSET = std::pow(10.0f, (95.0f - 685.0f) * CINEON_DENSITY_PER_CODE);

    float linearToCineon(float x){
        const float v = std::max(x, 0.0f) * (1.0f - CINEON_BLACK_OFFSET) + CINEON_BLACK_OFFSET;
        return (std::log10(v) / CINEON_DENSITY_PER_CODE + 685.0f) / 1023.0f;
    }

    float cineonToLinear(float x){
        const float v = std::pow(10.0f, (x * 1023.0f - 685.0f) * CINEON_DENSITY_PER_CODE);
        return (v - CINEON_BLACK_OFFSET) / (1.0f - CINEON_BLACK_OFFSET);
    }

    Vector3 perChannel(float (*f)(float), const Vector3& c){
        return Vector3(f(c.x), f(c.y), f(c.z));
    }

    // linear Rec. 709 into a LUT's input encoding (its gamut, then its curve)
    Vector3 encodeInput(LutInputEncoding encoding, const Vector3& linear){
        switch (encoding){
            case LutInputEncoding::SRGB: return perChannel(linearToSrgb, linear);
            case LutInputEncoding::LOGC3: return perChannel(linearToLogC3, mul(REC709_TO_AWG3, linear));
            case LutInputEncoding::ACESCCT: return perChannel(linearToAcesCct, mul(REC709_TO_AP1, linear));
            case LutInputEncoding::CINEON: return perChannel(linearToCineon, linear);
            case LutInputEncoding::DAVINCI_INTERMEDIATE: return perChannel(linearToDaVinci, mul(REC709_TO_DWG, linear));
            case LutInputEncoding::LINEAR:
            case LutInputEncoding::AUTO:
            default: return linear;
        }
    }

    // a LUT's output in its encoding back to linear Rec. 709
    Vector3 decodeOutput(LutInputEncoding encoding, const Vector3& c){
        switch (encoding){
            case LutInputEncoding::SRGB: return perChannel(srgbToLinear, c);
            case LutInputEncoding::LOGC3: return mul(AWG3_TO_REC709, perChannel(logC3ToLinear, c));
            case LutInputEncoding::ACESCCT: return mul(AP1_TO_REC709, perChannel(acesCctToLinear, c));
            case LutInputEncoding::CINEON: return perChannel(cineonToLinear, c);
            case LutInputEncoding::DAVINCI_INTERMEDIATE: return mul(DWG_TO_REC709, perChannel(daVinciToLinear, c));
            case LutInputEncoding::LINEAR:
            case LutInputEncoding::AUTO:
            default: return c;
        }
    }

    // linear Rec. 709 through the LUT read in an encoding, linear Rec. 709 out (not negative)
    // the linear value at the top of a LUT's input range in an encoding (its shaper's range when
    // it has one); the gamut matrices into the encodings keep a colour's largest channel at most
    // its largest Rec. 709 channel, so a colour whose channels stay below it stays in range
    float inputPeak(const CubeLut& lut, LutInputEncoding encoding){
        const float top = lut.size1D >= 2 ? lut.in1D[1] : lut.in3D[1];
        switch (encoding){
            case LutInputEncoding::SRGB: return srgbToLinear(top);
            case LutInputEncoding::LOGC3: return logC3ToLinear(top);
            case LutInputEncoding::ACESCCT: return acesCctToLinear(top);
            case LutInputEncoding::CINEON: return cineonToLinear(top);
            case LutInputEncoding::DAVINCI_INTERMEDIATE: return daVinciToLinear(top);
            case LutInputEncoding::LINEAR:
            case LutInputEncoding::AUTO:
            default: return top;
        }
    }

    Vector3 applyEncoded(const CubeLut& lut, LutInputEncoding encoding, const Vector3& linear){
        return maxZero(decodeOutput(encoding, lut.sample(encodeInput(encoding, maxZero(linear)))));
    }

    // ---- encoding proposal (CubeLutColorDomain.cpp)

    const char* encodingLabel(LutInputEncoding encoding){
        switch (encoding){
            case LutInputEncoding::AUTO: return "Auto";
            case LutInputEncoding::SRGB: return "Rec.709 / sRGB";
            case LutInputEncoding::LINEAR: return "Linear";
            case LutInputEncoding::LOGC3: return "ARRI LogC3";
            case LutInputEncoding::ACESCCT: return "ACEScct";
            case LutInputEncoding::CINEON: return "Cineon";
            case LutInputEncoding::DAVINCI_INTERMEDIATE: return "DaVinci Intermediate";
        }
        return "Rec.709 / sRGB";
    }

    // an encoding a title can name: the words naming it (lower case, consecutive words of the
    // title) and, for one the engine does not have, its name in the reason
    struct TitleName{
        std::string_view words[3];
        LutInputEncoding encoding;
        std::string_view unsupported;   // empty for an encoding the engine has
    };

    const TitleName TITLE_NAMES[] = {
        {{"logc"}, LutInputEncoding::LOGC3, {}},
        {{"logc3"}, LutInputEncoding::LOGC3, {}},
        {{"acescct"}, LutInputEncoding::ACESCCT, {}},
        {{"aces", "cct"}, LutInputEncoding::ACESCCT, {}},
        {{"davinci", "intermediate"}, LutInputEncoding::DAVINCI_INTERMEDIATE, {}},
        {{"davinci", "wide", "gamut"}, LutInputEncoding::DAVINCI_INTERMEDIATE, {}},
        {{"dwg"}, LutInputEncoding::DAVINCI_INTERMEDIATE, {}},
        {{"cineon"}, LutInputEncoding::CINEON, {}},
        {{"rec709"}, LutInputEncoding::SRGB, {}},
        {{"rec", "709"}, LutInputEncoding::SRGB, {}},
        {{"709"}, LutInputEncoding::SRGB, {}},
        {{"srgb"}, LutInputEncoding::SRGB, {}},
        {{"linear"}, LutInputEncoding::LINEAR, {}},
        {{"logc4"}, LutInputEncoding::SRGB, "LogC4"},
        {{"slog3"}, LutInputEncoding::SRGB, "S-Log3"},
        {{"s", "log3"}, LutInputEncoding::SRGB, "S-Log3"},
        {{"acescc"}, LutInputEncoding::SRGB, "ACEScc"},
        {{"vlog"}, LutInputEncoding::SRGB, "V-Log"},
        {{"v", "log"}, LutInputEncoding::SRGB, "V-Log"},
        {{"log3g10"}, LutInputEncoding::SRGB, "Log3G10"},
        {{"flog"}, LutInputEncoding::SRGB, "F-Log"},
        {{"f", "log"}, LutInputEncoding::SRGB, "F-Log"},
    };

    // the title's words: runs of letters and digits, in lower case
    std::vector<std::string> titleWords(std::string_view title){
        std::vector<std::string> words;
        std::string word;
        for (const char c : title){
            if (std::isalnum((unsigned char)c)){
                word.push_back((char)std::tolower((unsigned char)c));
                continue;
            }
            if (!word.empty())
                words.push_back(word);
            word.clear();
        }
        if (!word.empty())
            words.push_back(word);
        return words;
    }

    bool namesAt(const std::vector<std::string>& words, size_t at, const TitleName& name){
        for (size_t i = 0; i < 3 && !name.words[i].empty(); i++){
            if (at + i >= words.size() || words[at + i] != name.words[i])
                return false;
        }
        return true;
    }

    // the first encoding named among the words before the first "to" (a title "X to Y"
    // converts from X)
    const TitleName* firstNamed(const std::vector<std::string>& words){
        const size_t end = (size_t)(std::find(words.begin(), words.end(), "to") - words.begin());
        for (size_t at = 0; at < end; at++){
            for (const TitleName& name : TITLE_NAMES){
                if (namesAt(words, at, name))
                    return &name;
            }
        }
        return nullptr;
    }

    // ---- .jlut

    const uint32_t JLUT_VERSION = 1;
    const size_t JLUT_HEADER_SIZE = 36;   // magic, version, 1D size, 3D size, 4 range ends, title length

    // float16 with round to nearest even; NaN becomes 0 and magnitudes past 65504 clamp to it
    uint16_t floatToHalf(float f){
        uint32_t x;
        std::memcpy(&x, &f, sizeof(x));
        const uint16_t sign = (uint16_t)((x >> 16) & 0x8000u);
        x &= 0x7fffffffu;
        if (x > 0x7f800000u)
            return 0;
        if (x >= 0x477fe000u)
            return (uint16_t)(sign | 0x7bffu);
        if (x < 0x38800000u){
            // below the smallest normal half (2^-14): a subnormal in units of 2^-24, or 0
            if (x <= 0x33000000u)
                return sign;
            const int shift = 126 - (int)(x >> 23);
            const uint32_t m = (x & 0x7fffffu) | 0x800000u;
            uint32_t h = m >> shift;
            const uint32_t rest = m & ((1u << shift) - 1u);
            const uint32_t half = 1u << (shift - 1);
            if (rest > half || (rest == half && (h & 1u)))
                h++;
            return (uint16_t)(sign | h);
        }
        uint32_t h = (x - 0x38000000u) >> 13;
        const uint32_t rest = x & 0x1fffu;
        if (rest > 0x1000u || (rest == 0x1000u && (h & 1u)))
            h++;
        return (uint16_t)(sign | h);
    }

    float halfToFloat(uint16_t h){
        const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
        const uint32_t exponent = (h >> 10) & 0x1fu;
        const uint32_t mantissa = h & 0x3ffu;
        if (exponent == 0){
            const float v = (float)mantissa * (1.0f / 16777216.0f);
            return sign ? -v : v;
        }
        const uint32_t x = exponent == 31 ? (sign | 0x7f800000u | (mantissa << 13))
                                          : (sign | ((exponent + 112u) << 23) | (mantissa << 13));
        float f;
        std::memcpy(&f, &x, sizeof(f));
        return f;
    }

    void putU32(std::vector<uint8_t>& out, uint32_t v){
        out.push_back((uint8_t)v);
        out.push_back((uint8_t)(v >> 8));
        out.push_back((uint8_t)(v >> 16));
        out.push_back((uint8_t)(v >> 24));
    }

    void putF32(std::vector<uint8_t>& out, float f){
        uint32_t v;
        std::memcpy(&v, &f, sizeof(v));
        putU32(out, v);
    }

    void putHalves(std::vector<uint8_t>& out, const float* values, size_t count){
        for (size_t i = 0; i < count; i++){
            const uint16_t h = floatToHalf(values[i]);
            out.push_back((uint8_t)h);
            out.push_back((uint8_t)(h >> 8));
        }
    }

    uint32_t getU32(const uint8_t* p){
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }

    float getF32(const uint8_t* p){
        const uint32_t v = getU32(p);
        float f;
        std::memcpy(&f, &v, sizeof(f));
        return f;
    }

    void getHalves(const uint8_t* p, std::vector<float>& values){
        for (size_t i = 0; i < values.size(); i++)
            values[i] = halfToFloat((uint16_t)(p[i * 2] | (p[i * 2 + 1] << 8)));
    }

    bool validRange(const float range[2]){
        return std::isfinite(range[0]) && std::isfinite(range[1]) && range[0] < range[1];
    }

    std::string lowerExtension(const std::string& path){
        std::string extension = FileData::getFilePathExtension(path);
        for (char& c : extension)
            c = (char)std::tolower((unsigned char)c);
        return extension;
    }

    // ---- cache

    struct CacheEntry{
        std::shared_ptr<const CubeLut> lut;
        std::string error;
    };

    struct Cache{
        std::mutex mutex;
        std::unordered_map<std::string, CacheEntry> entries;
        uint64_t generation = 0;    // bumped by every invalidation: a load that overlaps one is not kept
    };

    Cache& cache(){
        static Cache instance;
        return instance;
    }
}

bool CubeLut::empty() const{
    return size1D < 2 && size3D < 2;
}

float CubeLut::inputMax() const{
    return std::max(size1D > 0 ? in1D[1] : 0.0f, size3D > 0 ? in3D[1] : 0.0f);
}

Vector3 CubeLut::sample(const Vector3& encoded) const{
    Vector3 c = encoded;
    if (size1D >= 2){
        c = Vector3(sample1D(*this, 0, normalise(c.x, in1D)),
                    sample1D(*this, 1, normalise(c.y, in1D)),
                    sample1D(*this, 2, normalise(c.z, in1D)));
    }
    if (size3D >= 2)
        c = sample3D(*this, normalise(c.x, in3D), normalise(c.y, in3D), normalise(c.z, in3D));
    return c;
}

bool jasperengine::parseCubeLut(std::string_view text, CubeLut& out, std::string& error){
    out = CubeLut();
    error.clear();
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        text.remove_prefix(3);

    enum class Phase{ HEADER, DATA_1D, DATA_3D, DONE } phase = Phase::HEADER;
    int line = 0;
    size_t count = 0;   // entries read of the current table
    size_t total3D = 0;
    auto fail = [&](const std::string& message){
        error = "line " + std::to_string(std::max(line, 1)) + ": " + message;
        out = CubeLut();
        return false;
    };

    size_t pos = 0;
    while (pos < text.size()){
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos)
            end = text.size();
        const std::string_view s = trim(text.substr(pos, end - pos));
        pos = end + 1;
        line++;
        if (s.empty() || s[0] == '#')
            continue;

        if (phase == Phase::HEADER){
            std::string message;
            const Header header = parseHeader(s, out, message);
            if (header == Header::FAILED)
                return fail(message);
            if (header == Header::PARSED)
                continue;
            // the first data line
            if (out.size1D == 0 && out.size3D == 0)
                return fail("Missing LUT_1D_SIZE and/or LUT_3D_SIZE.");
            if ((out.size1D > 0 && !validRange(out.in1D)) || (out.size3D > 0 && !validRange(out.in3D)))
                return fail("The input range's minimum must be below its maximum.");
            out.lut1D.resize((size_t)out.size1D * 3);
            total3D = (size_t)out.size3D * out.size3D * out.size3D;
            out.lut3D.resize(total3D * 3);
            phase = out.size1D > 0 ? Phase::DATA_1D : Phase::DATA_3D;
        }

        if (phase == Phase::DONE)
            return fail("Unexpected content after LUT data.");

        float rgb[3];
        if (!parseNumbers(s, rgb, 3))
            return fail(phase == Phase::DATA_1D ? "Invalid RGB line in 1D LUT section." : "Invalid RGB line in 3D LUT section.");
        float* target = (phase == Phase::DATA_1D ? out.lut1D.data() : out.lut3D.data()) + count * 3;
        target[0] = rgb[0];
        target[1] = rgb[1];
        target[2] = rgb[2];
        count++;
        if (phase == Phase::DATA_1D && count == (size_t)out.size1D){
            count = 0;
            phase = out.size3D > 0 ? Phase::DATA_3D : Phase::DONE;
        }else if (phase == Phase::DATA_3D && count == total3D){
            phase = Phase::DONE;
        }
    }

    switch (phase){
        case Phase::HEADER:
            if (out.size1D == 0 && out.size3D == 0)
                return fail("Missing LUT_1D_SIZE and/or LUT_3D_SIZE.");
            return fail(out.size1D > 0 ? "Incomplete 1D LUT data (0 of " + std::to_string(out.size1D) + " entries)."
                                       : "Incomplete 3D LUT data (0 of " + std::to_string((size_t)out.size3D * out.size3D * out.size3D) + " entries).");
        case Phase::DATA_1D:
            return fail("Incomplete 1D LUT data (" + std::to_string(count) + " of " + std::to_string(out.size1D) + " entries).");
        case Phase::DATA_3D:
            return fail("Incomplete 3D LUT data (" + std::to_string(count) + " of " + std::to_string(total3D) + " entries).");
        case Phase::DONE:
            break;
    }
    return true;
}

LutInputEncoding jasperengine::proposeEncoding(const CubeLut& lut, std::string& reason){
    if (lut.fromStrip){
        reason = "a PNG strip holds display colour";
        return LutInputEncoding::SRGB;
    }
    if (const TitleName* named = firstNamed(titleWords(lut.title))){
        reason = named->unsupported.empty()
                     ? "the title names " + std::string(encodingLabel(named->encoding))
                     : "the title names " + std::string(named->unsupported) + ", which is not supported, so it is treated as Rec.709 / sRGB";
        return named->encoding;
    }
    const float rangeMax = lut.inputMax();
    if (rangeMax > 1.0f){
        char text[32];
        std::snprintf(text, sizeof(text), "%g", (double)rangeMax);
        reason = std::string("the input range reaches ") + text;
        return LutInputEncoding::LINEAR;
    }
    reason = "a 0 to 1 input range and no encoding named in the title";
    return LutInputEncoding::SRGB;
}

LutInputEncoding jasperengine::resolveEncoding(LutInputEncoding setting, const CubeLut& lut, std::string* reason){
    if (setting == LutInputEncoding::AUTO || lut.fromStrip){
        std::string proposal;
        const LutInputEncoding encoding = proposeEncoding(lut, proposal);
        if (reason)
            *reason = proposal;
        return encoding;
    }
    if (reason)
        *reason = std::string("set to ") + encodingLabel(setting);
    return setting;
}

LutStage jasperengine::stageOf(LutInputEncoding encoding, const CubeLut& lut){
    if (encoding == LutInputEncoding::AUTO || lut.fromStrip)
        encoding = resolveEncoding(encoding, lut);
    if (encoding == LutInputEncoding::SRGB)
        return LutStage::DISPLAY;
    if (encoding == LutInputEncoding::LINEAR)
        return lut.inputMax() <= 1.0f ? LutStage::DISPLAY : LutStage::SCENE;
    return LutStage::SCENE;
}

Vector3 jasperengine::applyDisplayStage(const CubeLut& lut, LutInputEncoding encoding, const Vector3& engineDisplay){
    if (encoding == LutInputEncoding::AUTO)
        encoding = resolveEncoding(encoding, lut);
    const Vector3 e(saturate(engineDisplay.x), saturate(engineDisplay.y), saturate(engineDisplay.z));
    if (encoding == LutInputEncoding::SRGB){
        const Vector3 c = lut.sample(Vector3(engineToSrgb(e.x), engineToSrgb(e.y), engineToSrgb(e.z)));
        return Vector3(srgbToEngine(c.x), srgbToEngine(c.y), srgbToEngine(c.z));
    }
    const Vector3 linear(std::pow(e.x, GAMMA), std::pow(e.y, GAMMA), std::pow(e.z, GAMMA));
    const Vector3 c = applyEncoded(lut, encoding, linear);
    return Vector3(std::pow(c.x, 1.0f / GAMMA), std::pow(c.y, 1.0f / GAMMA), std::pow(c.z, 1.0f / GAMMA));
}

Vector3 jasperengine::applySceneStage(const CubeLut& lut, LutInputEncoding encoding, const Vector3& sceneLinear){
    if (encoding == LutInputEncoding::AUTO)
        encoding = resolveEncoding(encoding, lut);
    // A colour brighter than the LUT's input range (ACEScct ends near linear 222, LogC3 near 55)
    // goes through it scaled down into the range and back up, so its hue survives: clamping each
    // channel alone would turn a very bright red into white
    const float peak = std::max(std::max(sceneLinear.x, sceneLinear.y), sceneLinear.z);
    const float limit = inputPeak(lut, encoding);
    if (peak > limit && limit > 0.0f){
        const float k = limit / peak;
        return applyEncoded(lut, encoding, sceneLinear * k) * (1.0f / k);
    }
    return applyEncoded(lut, encoding, sceneLinear);
}

bool jasperengine::writeJlut(const CubeLut& lut, std::vector<uint8_t>& out){
    out.clear();
    if (lut.empty())
        return false;
    const int size1D = lut.size1D >= 2 ? lut.size1D : 0;
    const int size3D = lut.size3D >= 2 ? std::min(lut.size3D, JLUT_MAX_3D_SIZE) : 0;
    const size_t values1D = (size_t)size1D * 3;
    const size_t values3D = (size_t)size3D * size3D * size3D * 3;
    out.reserve(JLUT_HEADER_SIZE + lut.title.size() + (values1D + values3D) * 2);
    const uint8_t magic[4] = {'J', 'L', 'U', 'T'};
    out.insert(out.end(), magic, magic + 4);
    putU32(out, JLUT_VERSION);
    putU32(out, (uint32_t)size1D);
    putU32(out, (uint32_t)size3D);
    putF32(out, lut.in1D[0]);
    putF32(out, lut.in1D[1]);
    putF32(out, lut.in3D[0]);
    putF32(out, lut.in3D[1]);
    putU32(out, (uint32_t)lut.title.size());
    out.insert(out.end(), lut.title.begin(), lut.title.end());
    putHalves(out, lut.lut1D.data(), values1D);
    if (size3D == lut.size3D){
        putHalves(out, lut.lut3D.data(), values3D);
    }else if (size3D > 0){
        // resampled on the smaller grid: its texel i sits at i / (size3D - 1) of the table
        const float step = 1.0f / (size3D - 1);
        for (int b = 0; b < size3D; b++){
            for (int g = 0; g < size3D; g++){
                for (int r = 0; r < size3D; r++){
                    const Vector3 c = sample3D(lut, r * step, g * step, b * step);
                    const float rgb[3] = {c.x, c.y, c.z};
                    putHalves(out, rgb, 3);
                }
            }
        }
    }
    return true;
}

bool jasperengine::readJlut(const uint8_t* data, size_t size, CubeLut& out, std::string& error){
    out = CubeLut();
    error.clear();
    if (!data || size < JLUT_HEADER_SIZE || std::memcmp(data, "JLUT", 4) != 0){
        error = "Not a .jlut file.";
        return false;
    }
    const uint32_t version = getU32(data + 4);
    if (version != JLUT_VERSION){
        error = "Unsupported .jlut version " + std::to_string(version) + ".";
        return false;
    }
    const uint32_t size1D = getU32(data + 8);
    const uint32_t size3D = getU32(data + 12);
    if ((size1D != 0 && (size1D < 2 || size1D > (uint32_t)MAX_1D_SIZE)) || (size3D != 0 && (size3D < 2 || size3D > (uint32_t)MAX_3D_SIZE)) ||
        (size1D == 0 && size3D == 0)){
        error = "Invalid .jlut table sizes.";
        return false;
    }
    out.in1D[0] = getF32(data + 16);
    out.in1D[1] = getF32(data + 20);
    out.in3D[0] = getF32(data + 24);
    out.in3D[1] = getF32(data + 28);
    if ((size1D > 0 && !validRange(out.in1D)) || (size3D > 0 && !validRange(out.in3D))){
        error = "Invalid .jlut input range.";
        out = CubeLut();
        return false;
    }
    const size_t titleLength = getU32(data + 32);
    const size_t values1D = (size_t)size1D * 3;
    const size_t values3D = (size_t)size3D * size3D * size3D * 3;
    if (titleLength > size - JLUT_HEADER_SIZE || size - JLUT_HEADER_SIZE - titleLength != (values1D + values3D) * 2){
        error = "Truncated or damaged .jlut file.";
        out = CubeLut();
        return false;
    }
    const uint8_t* p = data + JLUT_HEADER_SIZE;
    out.title.assign((const char*)p, titleLength);
    p += titleLength;
    out.size1D = (int)size1D;
    out.size3D = (int)size3D;
    out.lut1D.resize(values1D);
    out.lut3D.resize(values3D);
    getHalves(p, out.lut1D);
    getHalves(p + values1D * 2, out.lut3D);
    return true;
}

bool jasperengine::loadColorLut(const std::string& path, CubeLut& out, std::string* error){
    out = CubeLut();
    std::string message;
    bool loaded = false;
    const std::string extension = lowerExtension(path);
    if (path.empty()){
        message = "No LUT path.";
    }else if (extension == "cube" || extension == "jlut"){
        // an exported game ships the cooked <name>.jlut in place of <name>.cube (the editor
        // reads the project's .cube, whatever else lies beside it)
        Data data;
        std::string file = path;
        unsigned int opened = FileErrors::FILE_NOT_FOUND;
#ifndef JASPERENGINE_EDITOR
        if (extension == "cube"){
            const std::string cooked = path.substr(0, path.size() - 4) + "jlut";
            opened = data.open(cooked.c_str());
            if (opened == FileErrors::FILEDATA_OK)
                file = cooked;
        }
#endif
        if (opened != FileErrors::FILEDATA_OK)
            opened = data.open(path.c_str());
        if (opened != FileErrors::FILEDATA_OK){
            message = "File not found.";
        }else if (file != path || extension == "jlut"){
            loaded = readJlut(data.getMemPtr(), data.length(), out, message);
        }else{
            loaded = parseCubeLut(std::string_view((const char*)data.getMemPtr(), data.length()), out, message);
        }
        if (!loaded)
            Log::error("Post-processing LUT %s: %s", file.c_str(), message.c_str());
    }else{
        // an image strip (loadStrip logs its own errors)
        PostColorLut::Strip strip;
        loaded = PostColorLut::loadStrip(path, strip, &message);
        if (loaded)
            PostColorLut::stripToCubeLut(strip, out);
    }
    if (!loaded){
        out = CubeLut();
        if (error)
            *error = message;
    }
    return loaded;
}

std::shared_ptr<const CubeLut> jasperengine::getCachedColorLut(const std::string& path){
    if (path.empty())
        return nullptr;
    Cache& c = cache();
    uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(c.mutex);
        const auto it = c.entries.find(path);
        if (it != c.entries.end())
            return it->second.lut;
        generation = c.generation;
    }
    // loaded without the lock: other paths stay available meanwhile
    CacheEntry entry;
    std::shared_ptr<CubeLut> lut = std::make_shared<CubeLut>();
    if (loadColorLut(path, *lut, &entry.error))
        entry.lut = std::move(lut);
    std::lock_guard<std::mutex> lock(c.mutex);
    if (generation != c.generation)
        return entry.lut;
    return c.entries.emplace(path, std::move(entry)).first->second.lut;
}

std::string jasperengine::getCachedColorLutError(const std::string& path){
    Cache& c = cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    const auto it = c.entries.find(path);
    return it != c.entries.end() ? it->second.error : std::string();
}

void jasperengine::invalidateColorLutCache(const std::string& path){
    Cache& c = cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    c.generation++;
    if (path.empty())
        c.entries.clear();
    else
        c.entries.erase(path);
}
