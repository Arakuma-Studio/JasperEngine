// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "IesProfile.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <sstream>

using namespace jasperengine;

namespace {
    std::string trimIes(const std::string& s){
        size_t begin = 0;
        size_t end = s.size();
        while (begin < end && std::isspace((unsigned char)s[begin])) begin++;
        while (end > begin && std::isspace((unsigned char)s[end - 1])) end--;
        return s.substr(begin, end - begin);
    }

    std::string upperIes(std::string s){
        for (char& c : s) c = (char)std::toupper((unsigned char)c);
        return s;
    }

    // the bracketing index i (a[i] <= x <= a[i + 1]) and the weight of a[i + 1]
    void iesBracket(const std::vector<float>& a, float x, size_t& i, float& t){
        if (a.size() < 2 || x <= a.front()){
            i = 0;
            t = 0.0f;
            return;
        }
        if (x >= a.back()){
            i = a.size() - 2;
            t = 1.0f;
            return;
        }
        i = (size_t)(std::upper_bound(a.begin(), a.end(), x) - a.begin()) - 1;
        const float span = a[i + 1] - a[i];
        t = span > 0.0f ? (x - a[i]) / span : 0.0f;
    }
}

const char* IesProfile::symmetryName(Symmetry symmetry){
    switch (symmetry){
        case Symmetry::ROTATIONAL:   return "rotational";
        case Symmetry::QUADRANT:     return "quadrant";
        case Symmetry::BILATERAL:    return "bilateral";
        case Symmetry::BILATERAL_90: return "bilateral (90-270)";
        default:                     return "none";
    }
}

float IesProfile::sampleSource(float verticalDegrees, float horizontalDegrees) const{
    const size_t vCount = verticalAngles.size();
    const size_t hCount = horizontalAngles.size();
    if (vCount == 0 || hCount == 0 || candela.size() != vCount * hCount)
        return 0.0f;
    if (verticalDegrees < verticalAngles.front() - 1e-4f || verticalDegrees > verticalAngles.back() + 1e-4f)
        return 0.0f;

    // the horizontal angle the file has, by its symmetry
    float h = std::fmod(horizontalDegrees, 360.0f);
    if (h < 0.0f) h += 360.0f;
    switch (symmetry){
        case Symmetry::ROTATIONAL:
            h = horizontalAngles.front();
            break;
        case Symmetry::QUADRANT:
            if (h > 180.0f) h = 360.0f - h;
            if (h > 90.0f) h = 180.0f - h;
            break;
        case Symmetry::BILATERAL:
            if (h > 180.0f) h = 360.0f - h;
            break;
        case Symmetry::BILATERAL_90:
            // symmetric about the 90-270 plane: value(h) = value(180 - h)
            if (h < 90.0f) h = 180.0f - h;
            else if (h > 270.0f) h = 540.0f - h;
            break;
        default:
            break;
    }

    size_t vi = 0;
    float vt = 0.0f;
    iesBracket(verticalAngles, verticalDegrees, vi, vt);
    auto column = [&](size_t hi){
        const float* values = candela.data() + hi * vCount;
        if (vCount == 1)
            return values[0];
        return values[vi] * (1.0f - vt) + values[vi + 1] * vt;
    };
    if (hCount == 1)
        return column(0);

    if (symmetry == Symmetry::FULL && h > horizontalAngles.back()){
        // between the last angle and 360, which is the first one again
        const float span = 360.0f + horizontalAngles.front() - horizontalAngles.back();
        const float t = span > 0.0f ? (h - horizontalAngles.back()) / span : 0.0f;
        return column(hCount - 1) * (1.0f - t) + column(0) * t;
    }
    if (symmetry == Symmetry::FULL && h < horizontalAngles.front()){
        const float span = 360.0f + horizontalAngles.front() - horizontalAngles.back();
        const float t = span > 0.0f ? (h + 360.0f - horizontalAngles.back()) / span : 0.0f;
        return column(hCount - 1) * (1.0f - t) + column(0) * t;
    }
    size_t hi = 0;
    float ht = 0.0f;
    iesBracket(horizontalAngles, h, hi, ht);
    return column(hi) * (1.0f - ht) + column(hi + 1) * ht;
}

float IesProfile::sample(float verticalDegrees, float horizontalDegrees) const{
    if ((int)grid.size() != GRID_ROWS * GRID_COLUMNS)
        return 0.0f;
    const float x = std::clamp(verticalDegrees / 180.0f, 0.0f, 1.0f) * (float)(GRID_COLUMNS - 1);
    float h = std::fmod(horizontalDegrees, 360.0f);
    if (h < 0.0f) h += 360.0f;
    const float y = h / 360.0f * (float)(GRID_ROWS - 1);
    const int x0 = std::min((int)x, GRID_COLUMNS - 2);
    const int y0 = std::min((int)y, GRID_ROWS - 2);
    const float fx = x - (float)x0;
    const float fy = y - (float)y0;
    auto at = [&](int c, int r){ return grid[(size_t)r * GRID_COLUMNS + c]; };
    const float top = at(x0, y0) * (1.0f - fx) + at(x0 + 1, y0) * fx;
    const float bottom = at(x0, y0 + 1) * (1.0f - fx) + at(x0 + 1, y0 + 1) * fx;
    return top * (1.0f - fy) + bottom * fy;
}

bool IesProfile::parse(const std::string& text, IesProfile& out, std::string& error){
    out = IesProfile();
    std::istringstream stream(text);
    std::string line;
    bool first = true;
    bool tiltFound = false;
    std::string lastKeyword;
    while (std::getline(stream, line)){
        line = trimIes(line);
        if (first){
            first = false;
            const std::string upper = upperIes(line);
            if (upper.rfind("IESNA:LM-63-2002", 0) == 0) out.format = "LM-63-2002";
            else if (upper.rfind("IESNA:LM-63-1995", 0) == 0) out.format = "LM-63-1995";
            else if (upper.rfind("IESNA91", 0) == 0) out.format = "LM-63-1991";
            else if (upper.rfind("IESNA", 0) == 0) out.format = line;
            else out.format = "LM-63-1986";
            if (upper.rfind("IESNA", 0) == 0)
                continue;
        }
        if (line.empty())
            continue;
        const std::string upper = upperIes(line);
        if (upper.rfind("TILT", 0) == 0){
            const size_t eq = line.find('=');
            const std::string value = upperIes(trimIes(eq == std::string::npos ? std::string() : line.substr(eq + 1)));
            if (value == "INCLUDE"){
                error = "TILT=INCLUDE is not supported: the profile must be exported with TILT=NONE";
                return false;
            }
            if (value != "NONE"){
                error = "TILT=" + trimIes(line.substr(eq + 1)) + " (a tilt file) is not supported: the profile must be exported with TILT=NONE";
                return false;
            }
            tiltFound = true;
            break;
        }
        if (line[0] == '['){
            const size_t close = line.find(']');
            if (close == std::string::npos)
                continue;
            const std::string keyword = upperIes(line.substr(1, close - 1));
            const std::string value = trimIes(line.substr(close + 1));
            if (keyword == "MORE" && !lastKeyword.empty()){
                std::string* target = lastKeyword == "MANUFAC" ? &out.manufacturer :
                    ((lastKeyword == "LUMINAIRE" || lastKeyword == "LUMCAT") ? &out.luminaire :
                    ((lastKeyword == "LAMP" || lastKeyword == "LAMPCAT") ? &out.lamp : nullptr));
                if (target) *target += " " + value;
                continue;
            }
            lastKeyword = keyword;
            if (keyword == "MANUFAC") out.manufacturer = value;
            else if (keyword == "LUMINAIRE") out.luminaire = value;
            else if (keyword == "LUMCAT" && out.luminaire.empty()) out.luminaire = value;
            else if (keyword == "LAMP") out.lamp = value;
            else if (keyword == "LAMPCAT" && out.lamp.empty()) out.lamp = value;
        }
    }
    if (!tiltFound){
        error = "no TILT= line: not an IESNA LM-63 photometric file";
        return false;
    }

    // the rest is numbers, separated by blanks, line breaks or commas
    std::vector<double> numbers;
    std::string rest((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    for (char& c : rest){
        if (c == ',') c = ' ';
    }
    std::istringstream values(rest);
    std::string token;
    while (values >> token){
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (end == token.c_str() || *end != '\0'){
            error = "'" + token + "' is not a number in the photometric data";
            return false;
        }
        numbers.push_back(value);
    }
    if (numbers.size() < 13){
        error = "the photometric data ends before its 13 header values";
        return false;
    }
    out.lamps = (int)numbers[0];
    out.lumensPerLamp = (float)numbers[1];
    const double multiplier = numbers[2];
    const int vCount = (int)numbers[3];
    const int hCount = (int)numbers[4];
    const int photometricType = (int)numbers[5];
    // numbers 6..9: units, width, length, height; 10..12: ballast factor, ballast-lamp factor, watts
    if (photometricType != 1){
        error = std::string("photometric type ") + (photometricType == 2 ? "B" : (photometricType == 3 ? "A" : std::to_string(photometricType))) +
            " is not supported: only Type C (1), the luminaire photometry of LM-63";
        return false;
    }
    if (vCount < 1 || hCount < 1 || vCount > 4096 || hCount > 4096){
        error = "the angle counts (" + std::to_string(vCount) + " vertical, " + std::to_string(hCount) + " horizontal) are out of range";
        return false;
    }
    const size_t needed = 13 + (size_t)vCount + (size_t)hCount + (size_t)vCount * (size_t)hCount;
    if (numbers.size() < needed){
        error = "the photometric data has " + std::to_string(numbers.size() - 13) + " values after its header, " +
            std::to_string(needed - 13) + " expected";
        return false;
    }
    size_t at = 13;
    for (int i = 0; i < vCount; i++) out.verticalAngles.push_back((float)numbers[at++]);
    for (int i = 0; i < hCount; i++) out.horizontalAngles.push_back((float)numbers[at++]);
    if (!std::is_sorted(out.verticalAngles.begin(), out.verticalAngles.end()) ||
            !std::is_sorted(out.horizontalAngles.begin(), out.horizontalAngles.end())){
        error = "the angles are not in increasing order";
        return false;
    }
    if (out.verticalAngles.front() < 0.0f || out.verticalAngles.back() > 180.0f){
        error = "a vertical angle is outside 0..180 degrees";
        return false;
    }
    std::vector<float> raw((size_t)vCount * hCount);
    double peak = 0.0;
    for (size_t i = 0; i < raw.size(); i++){
        const double value = std::max(numbers[at++] * multiplier, 0.0);
        raw[i] = (float)value;
        peak = std::max(peak, value);
    }
    if (peak <= 0.0){
        error = "every candela value is zero";
        return false;
    }
    out.peakCandela = (float)peak;
    out.candela.resize(raw.size());
    for (size_t i = 0; i < raw.size(); i++){
        out.candela[i] = (float)(raw[i] / peak);
    }

    const float firstH = out.horizontalAngles.front();
    const float lastH = out.horizontalAngles.back();
    if (hCount == 1) out.symmetry = Symmetry::ROTATIONAL;
    else if (firstH == 0.0f && lastH == 90.0f) out.symmetry = Symmetry::QUADRANT;
    else if (firstH == 0.0f && lastH == 180.0f) out.symmetry = Symmetry::BILATERAL;
    else if (firstH == 90.0f && lastH == 270.0f) out.symmetry = Symmetry::BILATERAL_90;
    else out.symmetry = Symmetry::FULL;

    out.grid.resize((size_t)GRID_ROWS * GRID_COLUMNS);
    for (int r = 0; r < GRID_ROWS; r++){
        const float h = (float)r * 360.0f / (float)(GRID_ROWS - 1);
        for (int c = 0; c < GRID_COLUMNS; c++){
            const float v = (float)c * 180.0f / (float)(GRID_COLUMNS - 1);
            out.grid[(size_t)r * GRID_COLUMNS + c] = std::clamp(out.sampleSource(v, h), 0.0f, 1.0f);
        }
    }

    if (out.lumensPerLamp > 0.0f && out.lamps > 0){
        out.lumens = out.lumensPerLamp * (float)out.lamps;
    }else{
        // absolute photometry: the flux of the candela over the sphere
        double flux = 0.0;
        const int steps = 180;
        const double pi = 3.14159265358979;
        for (int i = 0; i < steps; i++){
            const double theta = (i + 0.5) / steps * pi;
            const double ring = std::sin(theta) * (pi / steps) * (2.0 * pi / 72.0);
            for (int j = 0; j < 72; j++){
                flux += out.sampleSource((float)(theta * 180.0 / pi), (float)((j + 0.5) * 5.0)) * ring;
            }
        }
        out.lumens = (float)(flux * peak);
    }
    return true;
}
