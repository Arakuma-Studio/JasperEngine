// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/V1-vertex-colors.md R8, R9, R92, R131 (AC4, AC13): the .vcolors file

#include "TempDir.h"
#include "io/AtomicFile.h"
#include "texture/TexturePrepare.h"
#include "vertexcolor/VertexColorFile.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>

using namespace jasperengine;
using namespace jasperengine::vertexcolor;

namespace {

    TargetData makeTarget(int32_t node, int32_t primitive, int32_t submesh, uint32_t vertices) {
        TargetData t;
        t.key.node = node;
        t.key.primitive = primitive;
        t.key.submesh = submesh;
        t.fingerprint.vertexCount = vertices;
        t.fingerprint.indexCount = vertices * 3;
        t.fingerprint.topologyHash = 0x0123456789ABCDEFull + (uint64_t)node;
        t.fingerprint.positionHash = 0xFEDCBA9876543210ull - (uint64_t)node;
        for (uint32_t v = 0; v < vertices; v++) {
            t.base.push_back(Vector4((float)(v % 5) * 51.0f / 255.0f, 0.0f, 1.0f, (float)(v % 2)));
            t.snapshotPositions.push_back(Vector3((float)v, 0.5f * (float)v, -1.25f));
            t.snapshotNormals.push_back(Vector3(0.0f, 1.0f, 0.0f));
        }
        if (vertices > 2) {
            t.corners.push_back(CornerValue{1, Vector4(1.0f, 0.0f, 0.0f, 1.0f)});
            t.corners.push_back(CornerValue{7, Vector4(0.0f, 0.2f, 0.0f, 1.0f)});
        }
        return t;
    }

    Data sample(Format format) {
        Data data;
        data.format = format;
        data.targets.push_back(makeTarget(-1, -1, 0, 4));
        data.targets.push_back(makeTarget(2, 0, -1, 6));
        return data;
    }

    // a value as the file stores it, written out here (8-bit rounding or half precision)
    Vector4 stored(const Vector4& v, Format format) {
        float c[4] = {v.x, v.y, v.z, v.w};
        for (float& x : c) {
            if (format == Format::RGBA8) x = std::floor(std::clamp(x, 0.0f, 1.0f) * 255.0f + 0.5f) / 255.0f;
            else x = halfToFloat(floatToHalf(x));
        }
        return Vector4(c[0], c[1], c[2], c[3]);
    }

    // `b` holds `a`'s values at the file's precision
    void expectSame(const Data& a, const Data& b, bool snapshot) {
        ASSERT_EQ(a.format, b.format);
        ASSERT_EQ(a.targets.size(), b.targets.size());
        for (size_t i = 0; i < a.targets.size(); i++) {
            const TargetData& x = a.targets[i];
            const TargetData& y = b.targets[i];
            EXPECT_EQ(x.key, y.key);
            EXPECT_EQ(x.fingerprint, y.fingerprint);
            ASSERT_EQ(x.base.size(), y.base.size());
            for (size_t v = 0; v < x.base.size(); v++) EXPECT_EQ(stored(x.base[v], a.format), y.base[v]) << "target " << i << " vertex " << v;
            ASSERT_EQ(x.corners.size(), y.corners.size());
            for (size_t c = 0; c < x.corners.size(); c++) {
                EXPECT_EQ(x.corners[c].corner, y.corners[c].corner);
                EXPECT_EQ(stored(x.corners[c].value, a.format), y.corners[c].value);
            }
            if (snapshot) {
                EXPECT_EQ(x.snapshotPositions, y.snapshotPositions);
                EXPECT_EQ(x.snapshotNormals, y.snapshotNormals);
            } else {
                EXPECT_TRUE(y.snapshotPositions.empty());
                EXPECT_TRUE(y.snapshotNormals.empty());
            }
        }
    }

    bool decodes(const std::vector<uint8_t>& bytes, std::string& error) {
        Data out;
        return decode(bytes.data(), bytes.size(), out, error);
    }

    void putU32(std::vector<uint8_t>& bytes, size_t at, uint32_t v) {
        for (int i = 0; i < 4; i++) bytes[at + (size_t)i] = (uint8_t)(v >> (8 * i));
    }

    // one target of one vertex, uncompressed (too small to gain), no editor section
    std::vector<uint8_t> tinyFile(uint32_t overrides = 0) {
        Data data;
        TargetData t = makeTarget(-1, -1, 0, 1);
        t.corners.clear();
        for (uint32_t i = 0; i < overrides; i++) t.corners.push_back(CornerValue{i, Vector4(1.0f, 0.0f, 0.0f, 1.0f)});
        data.targets.push_back(t);
        return encode(data, false);
    }

    constexpr size_t HEADER = 12;
    constexpr size_t TARGET_FIXED = 12 + 24;   // key + fingerprint

}

TEST(VertexColorFile, RoundTrip) {
    for (const Format format : {Format::RGBA8, Format::RGBA16F}) {
        const Data data = sample(format);
        const std::vector<uint8_t> bytes = encode(data, true);
        Data out;
        std::string error;
        ASSERT_TRUE(decode(bytes.data(), bytes.size(), out, error)) << error;
        expectSame(data, out, true);
    }
}

TEST(VertexColorFile, HeaderLayoutIsLittleEndian) {
    const std::vector<uint8_t> bytes = tinyFile();
    ASSERT_GE(bytes.size(), HEADER + 4 + TARGET_FIXED);
    const uint8_t header[HEADER] = {'J', 'E', 'V', 'C', 1, 0, 0, 0, 0 /* RGBA8 */, 0 /* flags */, 0, 0};
    EXPECT_EQ(0, std::memcmp(bytes.data(), header, HEADER));
    // target count 1, then the key -1, -1, 0
    const uint8_t start[16] = {1, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0};
    EXPECT_EQ(0, std::memcmp(bytes.data() + HEADER, start, 16));
    // fingerprint: vertexCount 1, indexCount 3, the topology hash low byte first
    EXPECT_EQ(bytes[HEADER + 16], 1);
    EXPECT_EQ(bytes[HEADER + 20], 3);
    EXPECT_EQ(bytes[HEADER + 24], 0xEE);   // 0x0123456789ABCDEF - 1
    EXPECT_EQ(bytes[HEADER + 31], 0x01);
    // 1 value of 4 bytes, 0 overrides, nothing after
    EXPECT_EQ(bytes.size(), HEADER + 4 + TARGET_FIXED + 4 + 4 + 4);
}

TEST(VertexColorFile, EncodesValuesInTheFormat) {
    Data data;
    TargetData t = makeTarget(-1, -1, 0, 1);
    t.base[0] = Vector4(0.5f, 1.2f, -0.3f, 0.2f);
    data.targets.push_back(t);
    const size_t at = HEADER + 4 + TARGET_FIXED + 4;

    data.format = Format::RGBA8;
    std::vector<uint8_t> bytes = encode(data, false);
    EXPECT_EQ(bytes[at], 128);       // rounded
    EXPECT_EQ(bytes[at + 1], 255);   // clamped
    EXPECT_EQ(bytes[at + 2], 0);
    EXPECT_EQ(bytes[at + 3], 51);

    data.format = Format::RGBA16F;
    bytes = encode(data, false);
    const uint16_t halves[4] = {floatToHalf(0.5f), floatToHalf(1.2f), floatToHalf(-0.3f), floatToHalf(0.2f)};
    for (size_t i = 0; i < 4; i++) {
        EXPECT_EQ(bytes[at + i * 2], (uint8_t)halves[i]);
        EXPECT_EQ(bytes[at + i * 2 + 1], (uint8_t)(halves[i] >> 8));
    }
    Data out;
    std::string error;
    ASSERT_TRUE(decode(bytes.data(), bytes.size(), out, error)) << error;
    EXPECT_EQ(out.targets[0].base[0].y, halfToFloat(floatToHalf(1.2f)));   // HDR kept
    EXPECT_EQ(out.targets[0].base[0].z, halfToFloat(floatToHalf(-0.3f)));
}

TEST(VertexColorFile, ExportStripsTheEditorSection) {
    const Data data = sample(Format::RGBA8);
    const std::vector<uint8_t> editor = encode(data, true);
    const std::vector<uint8_t> exported = encode(data, false);
    EXPECT_EQ(editor[9] & 1, 1);
    EXPECT_EQ(exported[9] & 1, 0);
    Data out;
    std::string error;
    ASSERT_TRUE(decode(exported.data(), exported.size(), out, error)) << error;
    expectSame(data, out, false);
}

TEST(VertexColorFile, CompressesTheBodyOnlyWhenSmaller) {
    Data big;
    big.targets.push_back(makeTarget(-1, -1, 0, 0));
    big.targets[0].base.assign(10000, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    big.targets[0].corners.push_back(CornerValue{5, Vector4(0.0f, 0.0f, 0.0f, 1.0f)});
    const std::vector<uint8_t> bytes = encode(big, false);
    EXPECT_EQ(bytes[9] & 2, 2);
    EXPECT_LT(bytes.size(), 10000u * 4u / 10u);
    Data out;
    std::string error;
    ASSERT_TRUE(decode(bytes.data(), bytes.size(), out, error)) << error;
    expectSame(big, out, false);

    EXPECT_EQ(tinyFile()[9] & 2, 0);
}

TEST(VertexColorFile, RefusesFutureVersion) {
    std::vector<uint8_t> bytes = encode(sample(Format::RGBA8), true);
    putU32(bytes, 4, 2);
    Data out = sample(Format::RGBA16F);
    std::string error;
    EXPECT_FALSE(decode(bytes.data(), bytes.size(), out, error));
    EXPECT_EQ(error, "made by a newer JasperEngine (version 2); this editor reads up to 1");
    // the caller's data is left alone
    EXPECT_EQ(out.format, Format::RGBA16F);
    EXPECT_EQ(out.targets.size(), 2u);

    // S03: reading a future file leaves its bytes untouched
    test::TempDir dir;
    const std::filesystem::path path = dir.file("future.vcolors");
    {
        std::ofstream file(path, std::ios::binary);
        file.write((const char*)bytes.data(), (std::streamsize)bytes.size());
    }
    EXPECT_FALSE(readFile(path, out, error));
    EXPECT_NE(error.find("made by a newer JasperEngine (version 2)"), std::string::npos) << error;
    std::ifstream in(path, std::ios::binary);
    const std::vector<uint8_t> after((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(after, bytes);
}

TEST(VertexColorFile, RejectsMalformedBytes) {
    const std::vector<uint8_t> good = tinyFile(2);
    std::string error;
    ASSERT_TRUE(decodes(good, error)) << error;
    ASSERT_EQ(good[9], 0) << "the offsets below assume an uncompressed body";
    const size_t overrides = HEADER + 4 + TARGET_FIXED + 4 + 4;   // the override count

    struct Case {
        const char* name;
        std::vector<uint8_t> bytes;
        const char* message;
    };
    std::vector<Case> cases;
    cases.push_back({"empty", {}, "truncated"});
    cases.push_back({"short header", std::vector<uint8_t>(good.begin(), good.begin() + 8), "truncated"});
    {
        std::vector<uint8_t> b = good;
        b[0] = 'X';
        cases.push_back({"bad magic", b, "bad magic"});
    }
    {
        std::vector<uint8_t> b = good;
        putU32(b, 4, 0);
        cases.push_back({"version 0", b, "version 0"});
    }
    {
        std::vector<uint8_t> b = good;
        b[8] = 7;
        cases.push_back({"unknown format", b, "unknown format"});
    }
    {
        std::vector<uint8_t> b = good;
        b[9] = 0x80;
        cases.push_back({"unknown flags", b, "unknown flags"});
    }
    cases.push_back({"no target count", std::vector<uint8_t>(good.begin(), good.begin() + HEADER + 2), "truncated"});
    {
        std::vector<uint8_t> b = good;
        putU32(b, HEADER, 0x40000000u);
        cases.push_back({"corrupt target count", b, "corrupt target count"});
    }
    {
        std::vector<uint8_t> b = good;
        putU32(b, HEADER + 4 + TARGET_FIXED, 1000);
        cases.push_back({"corrupt vertex count", b, "corrupt vertex count"});
    }
    {
        std::vector<uint8_t> b = good;
        putU32(b, overrides, 1000);
        cases.push_back({"corrupt override count", b, "corrupt override count"});
    }
    {
        std::vector<uint8_t> b = good;
        putU32(b, overrides + 4 + 8, 0);   // second override's corner = first's
        cases.push_back({"repeated override", b, "not sorted or repeated"});
    }
    cases.push_back({"truncated overrides", std::vector<uint8_t>(good.begin(), good.end() - 3), "corrupt override count"});
    {
        std::vector<uint8_t> b = good;
        b.push_back(0);
        cases.push_back({"trailing bytes", b, "after the last target"});
    }
    {
        // the editor flag on a file without the section: the snapshot count is missing
        std::vector<uint8_t> b = good;
        b[9] = 1;
        cases.push_back({"missing snapshot", b, "no snapshot count"});
    }
    {
        Data two;
        two.targets.push_back(makeTarget(3, 0, -1, 1));
        two.targets.push_back(makeTarget(1, 0, -1, 1));
        cases.push_back({"targets out of order", encode(two, false), "not sorted by key"});
    }
    {
        // a body that says zstd but is not
        std::vector<uint8_t> b = good;
        b[9] = 2;
        cases.push_back({"bad zstd", b, "compressed body"});
    }
    {
        Data big;
        big.targets.push_back(makeTarget(-1, -1, 0, 0));
        big.targets[0].base.assign(4000, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
        std::vector<uint8_t> b = encode(big, false);
        ASSERT_EQ(b[9] & 2, 2);
        b.resize(b.size() - 4);
        cases.push_back({"truncated zstd", b, "compressed body"});
    }

    for (const Case& c : cases) {
        Data out = sample(Format::RGBA16F);
        error.clear();
        EXPECT_FALSE(decode(c.bytes.data(), c.bytes.size(), out, error)) << c.name;
        EXPECT_NE(error.find(c.message), std::string::npos) << c.name << ": " << error;
        EXPECT_EQ(out.targets.size(), 2u) << c.name << ": a failed decode must not touch the output";
    }
}

TEST(VertexColorFile, WriteFileIsAtomic) {
    test::TempDir dir;
    const std::filesystem::path path = dir.file("Rock.vcolors");
    const Data data = sample(Format::RGBA16F);
    std::string error;
    ASSERT_TRUE(writeFile(path, data, true, error)) << error;
    Data out;
    ASSERT_TRUE(readFile(path, out, error)) << error;
    expectSame(data, out, true);
    std::ifstream in(path, std::ios::binary);
    const std::vector<uint8_t> first((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    // a failed publish leaves the previous bytes (AtomicFile's rename step)
    io::setAtomicFileFaultInjector([](std::string_view step, const std::filesystem::path&) {
        return step == "rename" ? std::make_error_code(std::errc::io_error) : std::error_code();
    });
    Data changed = data;
    changed.targets[0].base[0] = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    const bool written = writeFile(path, changed, true, error);
    io::setAtomicFileFaultInjector(nullptr);
    EXPECT_FALSE(written);
    EXPECT_FALSE(error.empty());
    std::ifstream again(path, std::ios::binary);
    const std::vector<uint8_t> after((std::istreambuf_iterator<char>(again)), std::istreambuf_iterator<char>());
    EXPECT_EQ(after, first);
    for (const auto& entry : std::filesystem::directory_iterator(dir.path()))
        EXPECT_FALSE(io::isTemporarySiblingName(entry.path().filename().string())) << entry.path();

    EXPECT_FALSE(readFile(dir.file("missing.vcolors"), out, error));
    EXPECT_NE(error.find("cannot open"), std::string::npos) << error;
}
