// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The style block layout follows Pachislop src/io/psStyle.ts (aslStyles) by SafEight, used with
// the author's permission; the pattern records and virtual memory arrays are written from the
// Adobe Photoshop File Formats Specification.

#include "AslReader.h"

#include "PsdReader.h"

#include <algorithm>

using namespace jasperengine::editor;
using namespace jasperengine::editor::psd;

namespace {

    constexpr size_t MAX_ITEMS = 4096;
    constexpr int MAX_PATTERN_SIDE = 8192;

    // one virtual memory array (a channel of a pattern): w x h samples, raw or PackBits
    bool readVmArray(ByteReader& r, int expectW, int expectH, std::vector<uint8_t>& out, bool& present) {
        present = r.u32() != 0;
        if (!present) return r.ok();
        const uint64_t length = r.length(false, "a pattern channel");
        ByteReader a = r.sub((size_t)length);
        if (length == 0) {
            present = false;
            return r.ok();
        }
        const uint32_t depth = a.u32();
        const int32_t top = a.i32(), left = a.i32(), bottom = a.i32(), right = a.i32();
        a.u16();
        const uint8_t compression = a.u8();
        const int w = right - left, h = bottom - top;
        if (!a.ok() || depth != 8 || w != expectW || h != expectH || w <= 0 || h <= 0) {
            present = false;
            return a.ok();
        }
        out.assign((size_t)w * h, 0);
        if (compression == 0) {
            const uint8_t* p = a.view(out.size());
            if (!p) return false;
            std::copy(p, p + out.size(), out.begin());
        } else {
            std::vector<uint16_t> counts((size_t)h);
            for (auto& c : counts) c = a.u16();
            for (int y = 0; y < h && a.ok(); y++) {
                if (!unpackBits(a, counts[(size_t)y], out.data() + (size_t)y * w, (size_t)w)) return a.fail("a pattern row is malformed");
            }
        }
        return a.ok();
    }

    bool readPattern(ByteReader& r, PsPattern& p, std::string& error) {
        r.u32();    // version 1
        const uint32_t mode = r.u32();
        const int h = r.u16(), w = r.u16();
        p.name = r.unicode();
        p.id = r.pascal(1);
        if (mode == 2) r.skip(256 * 3 + 4);
        if (!r.ok()) {
            error = r.failure();
            return false;
        }
        if (mode != 3 && mode != 1) {
            error = "pattern '" + p.name + "' is " + psdColorModeName((uint16_t)mode) + ": only RGB and Grayscale patterns are read";
            return false;
        }
        if (w <= 0 || h <= 0 || w > MAX_PATTERN_SIDE || h > MAX_PATTERN_SIDE) {
            error = "pattern '" + p.name + "' is " + std::to_string(w) + " x " + std::to_string(h) + " px";
            return false;
        }
        r.u32();    // virtual memory array list version 3
        ByteReader list = r.sub((size_t)r.length(false, "a pattern image"));
        list.skip(16);
        const uint32_t channels = list.u32();
        if (channels > 64) {
            error = "pattern '" + p.name + "' has " + std::to_string(channels) + " channels";
            return false;
        }
        std::vector<std::vector<uint8_t>> planes;
        // the colour channels, then the user and sheet masks (one of which may be the alpha)
        for (uint32_t c = 0; c < channels + 2 && list.remaining() >= 4 && list.ok(); c++) {
            std::vector<uint8_t> plane;
            bool present = false;
            if (!readVmArray(list, w, h, plane, present)) {
                error = "pattern '" + p.name + "': " + list.failure();
                return false;
            }
            if (present) planes.push_back(std::move(plane));
        }
        const size_t colors = mode == 3 ? 3 : 1;
        if (planes.size() < colors) {
            error = "pattern '" + p.name + "' has no image";
            return false;
        }
        p.width = w;
        p.height = h;
        p.rgba.assign((size_t)w * h * 4, 255);
        for (size_t i = 0; i < (size_t)w * h; i++) {
            for (size_t c = 0; c < 3; c++) p.rgba[i * 4 + c] = planes[colors == 3 ? c : 0][i];
            if (planes.size() > colors) p.rgba[i * 4 + 3] = planes[colors][i];
        }
        return true;
    }

}

void psd::readPatterns(const uint8_t* data, size_t size, std::vector<PsPattern>& out, std::vector<std::string>* skipped) {
    ByteReader r(data, size);
    while (r.remaining() >= 4 && r.ok() && out.size() < MAX_ITEMS) {
        const uint64_t length = r.length(false, "a pattern");
        if (!r.ok() || length == 0) break;
        ByteReader one = r.sub((size_t)length);
        // records are padded to 4 bytes
        while (r.remaining() && r.position() % 4) r.skip(1);
        PsPattern p;
        std::string error;
        if (readPattern(one, p, error)) out.push_back(std::move(p));
        else if (skipped) skipped->push_back(error);
    }
}

bool psd::readAsl(const uint8_t* data, size_t size, AslFile& out, std::string& error) {
    out = AslFile();
    ByteReader r(data, size);
    const uint16_t version = r.u16();
    if (version != 2 || r.fourcc() != "8BSL") {
        error = "not a Photoshop style library (.asl: version 2, '8BSL')";
        return false;
    }
    r.u16();    // patterns version
    {
        const uint64_t length = r.length(false, "the pattern block");
        const uint8_t* p = r.view((size_t)length);
        if (p) readPatterns(p, (size_t)length, out.patterns, &out.skipped);
    }
    const uint32_t count = r.u32();
    if (!r.ok()) {
        error = r.failure();
        return false;
    }
    if (count > MAX_ITEMS) {
        error = "a style library of " + std::to_string(count) + " styles (at most 4,096)";
        return false;
    }
    for (uint32_t i = 0; i < count && r.ok(); i++) {
        const uint64_t length = r.length(false, "a style");
        ByteReader block = r.sub((size_t)length);
        // style blocks are padded to 4 bytes
        const size_t padded = (size_t)((length + 3) / 4 * 4);
        if (padded > length) r.skip(std::min(padded - (size_t)length, r.remaining()));
        AslStyle style;
        style.name = "Style " + std::to_string(i + 1);
        Json ident;
        if (!readVersionedDescriptor(block, ident)) {
            out.skipped.push_back(style.name + ": " + block.failure());
            continue;
        }
        const std::string name = descText(ident, "Nm  ");
        if (!name.empty()) style.name = name;
        Json desc;
        if (!readVersionedDescriptor(block, desc)) {
            out.skipped.push_back(style.name + ": " + block.failure());
            continue;
        }
        if (const Json* fx = descObject(desc, "Lefx")) style.effects = *fx;
        style.style = std::move(desc);
        out.styles.push_back(std::move(style));
    }
    if (!r.ok()) {
        error = r.failure();
        return false;
    }
    return true;
}
