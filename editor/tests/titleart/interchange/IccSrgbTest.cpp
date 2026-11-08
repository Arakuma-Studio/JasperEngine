// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The generated sRGB profile (docs/specs/S87-title-localisation-interchange.md R44, §6.10, AC24):
// header, tag table and D50; the matrix columns sum to D50; the TRC is IEC 61966-2-1's; the
// three TRC tags share one curve; the bytes are pinned by their SHA-256, so Linux, Windows and
// macOS CI give the same profile.

#include <gtest/gtest.h>

#include "titleart/interchange/IccSrgb.h"
#include "titleart/interchange/psd/PsdReader.h"
#include "util/Sha256.h"

#include <cmath>
#include <map>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    uint32_t be32(const std::vector<uint8_t>& b, size_t at) {
        return (uint32_t)b[at] << 24 | (uint32_t)b[at + 1] << 16 | (uint32_t)b[at + 2] << 8 | b[at + 3];
    }

    double s15(const std::vector<uint8_t>& b, size_t at) {
        return (int32_t)be32(b, at) / 65536.0;
    }

    std::string sig(const std::vector<uint8_t>& b, size_t at) {
        return std::string(reinterpret_cast<const char*>(b.data() + at), 4);
    }

}

TEST(IccSrgb, ProfileIsValid) {
    const std::vector<uint8_t>& icc = srgbIccProfile();
    ASSERT_GE(icc.size(), 132u);
    EXPECT_EQ(be32(icc, 0), icc.size());
    EXPECT_LT(icc.size(), 3000u) << "about 2.6 KB";
    EXPECT_EQ(be32(icc, 8), 0x02100000u) << "ICC v2.1";
    EXPECT_EQ(sig(icc, 12), "mntr");
    EXPECT_EQ(sig(icc, 16), "RGB ");
    EXPECT_EQ(sig(icc, 20), "XYZ ");
    EXPECT_EQ(sig(icc, 36), "acsp");
    EXPECT_NEAR(s15(icc, 68), 0.9642, 1e-4);
    EXPECT_NEAR(s15(icc, 72), 1.0, 1e-4);
    EXPECT_NEAR(s15(icc, 76), 0.8249, 1e-4);
    const uint32_t count = be32(icc, 128);
    std::map<std::string, std::pair<uint32_t, uint32_t>> tags;
    for (uint32_t i = 0; i < count; i++) {
        const size_t at = 132 + 12 * i;
        const uint32_t offset = be32(icc, at + 4), size = be32(icc, at + 8);
        EXPECT_LE(offset + size, icc.size());
        EXPECT_EQ(offset % 4, 0u) << "tag data is 4-byte aligned";
        tags[sig(icc, at)] = {offset, size};
    }
    for (const char* t : {"desc", "cprt", "wtpt", "rXYZ", "gXYZ", "bXYZ", "rTRC", "gTRC", "bTRC"}) EXPECT_TRUE(tags.count(t)) << t;
    EXPECT_EQ(tags["rTRC"], tags["gTRC"]);
    EXPECT_EQ(tags["rTRC"], tags["bTRC"]) << "one shared curve";
    EXPECT_EQ(psd::iccDescription(icc), "sRGB (JasperEngine)");
    // the matrix columns sum to D50
    double sum[3] = {0, 0, 0};
    for (const char* t : {"rXYZ", "gXYZ", "bXYZ"}) {
        const uint32_t at = tags[t].first;
        EXPECT_EQ(sig(icc, at), "XYZ ");
        for (int k = 0; k < 3; k++) sum[k] += s15(icc, at + 8 + 4 * k);
    }
    EXPECT_NEAR(sum[0], 0.9642, 1e-4);
    EXPECT_NEAR(sum[1], 1.0, 1e-4);
    EXPECT_NEAR(sum[2], 0.8249, 1e-4);
    // the white point is D50
    EXPECT_NEAR(s15(icc, tags["wtpt"].first + 8), 0.9642, 1e-4);
    // the TRC: IEC 61966-2-1 within 1/65535
    const uint32_t curve = tags["rTRC"].first;
    EXPECT_EQ(sig(icc, curve), "curv");
    const uint32_t entries = be32(icc, curve + 8);
    ASSERT_EQ(entries, 1024u);
    for (uint32_t i = 0; i < entries; i++) {
        const double x = (double)i / (entries - 1);
        const double expected = x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
        const double stored = (icc[curve + 12 + 2 * i] << 8 | icc[curve + 13 + 2 * i]) / 65535.0;
        EXPECT_NEAR(stored, expected, 1.0 / 65535 + 1e-12) << i;
    }
    // the sRGB primaries as known (Bradford-adapted to D50)
    double m[3][3];
    srgbIccMatrix(m);
    EXPECT_NEAR(m[0][0], 0.4361, 2e-4);
    EXPECT_NEAR(m[1][1], 0.7169, 2e-4);
    EXPECT_NEAR(m[2][2], 0.7142, 2e-4);
}

TEST(IccSrgb, BytesArePinned) {
    // the same bytes on every platform (integer rounding of the computed values)
    EXPECT_EQ(sha256Hex(std::string_view(reinterpret_cast<const char*>(srgbIccProfile().data()), srgbIccProfile().size())),
              "PIN") << "the generated profile changed: if on purpose, pin the new hash";
}
