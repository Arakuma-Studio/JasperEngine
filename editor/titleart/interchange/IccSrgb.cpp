// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Written from ICC.1:2001-04 (version 2 profiles) and IEC 61966-2-1 (sRGB).

#include "IccSrgb.h"

#include <cmath>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // the ICC PCS illuminant (D50, as ICC.1 gives it)
    constexpr double D50[3] = {0.9642, 1.0, 0.8249};
    constexpr int TRC_ENTRIES = 1024;

    void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
        for (int i = 0; i < 4; i++) b[at + i] = (uint8_t)(v >> (24 - 8 * i));
    }
    void push32(std::vector<uint8_t>& b, uint32_t v) {
        for (int s = 24; s >= 0; s -= 8) b.push_back((uint8_t)(v >> s));
    }
    void push16(std::vector<uint8_t>& b, uint16_t v) {
        b.push_back((uint8_t)(v >> 8));
        b.push_back((uint8_t)v);
    }
    void pushSig(std::vector<uint8_t>& b, const char* s) {
        b.insert(b.end(), s, s + 4);
    }
    uint32_t s15(double v) {
        return (uint32_t)(int32_t)std::lround(v * 65536.0);
    }

    void invert(const double m[3][3], double out[3][3]) {
        const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        out[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det;
        out[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
        out[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
        out[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det;
        out[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
        out[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
        out[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det;
        out[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
        out[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
    }

    void multiply(const double a[3][3], const double b[3][3], double out[3][3]) {
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) out[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
        }
    }

    // a v2 'desc' (textDescriptionType) tag
    std::vector<uint8_t> descTag(const std::string& text) {
        std::vector<uint8_t> b;
        pushSig(b, "desc");
        push32(b, 0);
        push32(b, (uint32_t)text.size() + 1);
        b.insert(b.end(), text.begin(), text.end());
        b.push_back(0);
        push32(b, 0);       // Unicode language
        push32(b, 0);       // Unicode count
        push16(b, 0);       // ScriptCode code
        b.push_back(0);     // ScriptCode count
        b.insert(b.end(), 67, 0);
        return b;
    }

    std::vector<uint8_t> textTag(const std::string& text) {
        std::vector<uint8_t> b;
        pushSig(b, "text");
        push32(b, 0);
        b.insert(b.end(), text.begin(), text.end());
        b.push_back(0);
        return b;
    }

    std::vector<uint8_t> xyzTag(double x, double y, double z) {
        std::vector<uint8_t> b;
        pushSig(b, "XYZ ");
        push32(b, 0);
        push32(b, s15(x));
        push32(b, s15(y));
        push32(b, s15(z));
        return b;
    }

    std::vector<uint8_t> curvTag() {
        std::vector<uint8_t> b;
        pushSig(b, "curv");
        push32(b, 0);
        push32(b, TRC_ENTRIES);
        for (int i = 0; i < TRC_ENTRIES; i++) {
            const double v = srgbIccTransfer((double)i / (TRC_ENTRIES - 1));
            push16(b, (uint16_t)std::lround(v * 65535.0));
        }
        return b;
    }

    std::vector<uint8_t> build() {
        double m[3][3];
        srgbIccMatrix(m);
        struct Tag {
            const char* sig;
            int data;       // index into `blocks`
        };
        std::vector<std::vector<uint8_t>> blocks = {descTag("sRGB (JasperEngine)"), textTag("No copyright, use freely"), xyzTag(D50[0], D50[1], D50[2]),
                                                    xyzTag(m[0][0], m[1][0], m[2][0]), xyzTag(m[0][1], m[1][1], m[2][1]),
                                                    xyzTag(m[0][2], m[1][2], m[2][2]), curvTag()};
        const Tag tags[] = {{"desc", 0}, {"cprt", 1}, {"wtpt", 2}, {"rXYZ", 3}, {"gXYZ", 4}, {"bXYZ", 5}, {"rTRC", 6}, {"gTRC", 6}, {"bTRC", 6}};
        const size_t tagCount = sizeof(tags) / sizeof(tags[0]);
        std::vector<uint8_t> out(128, 0);
        push32(out, (uint32_t)tagCount);
        const size_t table = out.size();
        out.resize(table + tagCount * 12, 0);
        std::vector<uint32_t> offsets(blocks.size()), sizes(blocks.size());
        for (size_t i = 0; i < blocks.size(); i++) {
            while (out.size() % 4) out.push_back(0);
            offsets[i] = (uint32_t)out.size();
            sizes[i] = (uint32_t)blocks[i].size();
            out.insert(out.end(), blocks[i].begin(), blocks[i].end());
        }
        while (out.size() % 4) out.push_back(0);
        for (size_t t = 0; t < tagCount; t++) {
            const size_t at = table + t * 12;
            for (int k = 0; k < 4; k++) out[at + k] = (uint8_t)tags[t].sig[k];
            put32(out, at + 4, offsets[(size_t)tags[t].data]);
            put32(out, at + 8, sizes[(size_t)tags[t].data]);
        }
        // the header
        put32(out, 0, (uint32_t)out.size());
        put32(out, 8, 0x02100000);          // version 2.1
        const auto sig = [&out](size_t at, const char* s) {
            for (int i = 0; i < 4; i++) out[at + i] = (uint8_t)s[i];
        };
        sig(12, "mntr");                    // display class
        sig(16, "RGB ");
        sig(20, "XYZ ");
        sig(36, "acsp");
        put32(out, 64, 0);                  // perceptual
        put32(out, 68, s15(D50[0]));
        put32(out, 72, s15(D50[1]));
        put32(out, 76, s15(D50[2]));
        return out;
    }

}

double editor::srgbIccTransfer(double v) {
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

void editor::srgbIccMatrix(double out[3][3]) {
    // IEC 61966-2-1: primaries and D65 white as xy chromaticities
    const double xy[3][2] = {{0.64, 0.33}, {0.30, 0.60}, {0.15, 0.06}};
    const double wx = 0.3127, wy = 0.3290;
    double p[3][3];
    for (int c = 0; c < 3; c++) {
        p[0][c] = xy[c][0] / xy[c][1];
        p[1][c] = 1.0;
        p[2][c] = (1.0 - xy[c][0] - xy[c][1]) / xy[c][1];
    }
    const double white[3] = {wx / wy, 1.0, (1.0 - wx - wy) / wy};
    double pInv[3][3];
    invert(p, pInv);
    double s[3];
    for (int i = 0; i < 3; i++) s[i] = pInv[i][0] * white[0] + pInv[i][1] * white[1] + pInv[i][2] * white[2];
    double rgbToXyz[3][3];
    for (int i = 0; i < 3; i++) {
        for (int c = 0; c < 3; c++) rgbToXyz[i][c] = p[i][c] * s[c];
    }
    // Bradford adaptation D65 -> D50
    const double bradford[3][3] = {{0.8951, 0.2664, -0.1614}, {-0.7502, 1.7135, 0.0367}, {0.0389, -0.0685, 1.0296}};
    double bradfordInv[3][3];
    invert(bradford, bradfordInv);
    double src[3], dst[3];
    for (int i = 0; i < 3; i++) {
        src[i] = bradford[i][0] * white[0] + bradford[i][1] * white[1] + bradford[i][2] * white[2];
        dst[i] = bradford[i][0] * D50[0] + bradford[i][1] * D50[1] + bradford[i][2] * D50[2];
    }
    double scale[3][3] = {{dst[0] / src[0], 0, 0}, {0, dst[1] / src[1], 0}, {0, 0, dst[2] / src[2]}};
    double tmp[3][3], adapt[3][3];
    multiply(scale, bradford, tmp);
    multiply(bradfordInv, tmp, adapt);
    multiply(adapt, rgbToXyz, out);
}

const std::vector<uint8_t>& editor::srgbIccProfile() {
    static const std::vector<uint8_t> profile = build();
    return profile;
}
