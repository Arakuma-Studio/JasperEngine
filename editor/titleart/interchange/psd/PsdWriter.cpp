// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Written from the Adobe Photoshop File Formats Specification. The layer plan it serves (every
// layer rendered alone, groups as section dividers) follows Pachislop src/io/psd.ts by SafEight,
// used with the author's permission.

#include "PsdWriter.h"

#include "PsdBytes.h"
#include "PsdReader.h"
#include "titleart/interchange/IccSrgb.h"

#include "miniz.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace jasperengine::editor;
using namespace jasperengine::editor::psd;

namespace {

    struct Channel {
        int16_t id = 0;
        std::vector<uint8_t> data;      // compression + payload
    };

    struct Record {
        const PsdWriteLayer* layer = nullptr;
        int section = 0;                // 0 layer, 1 open group, 2 closed group, 3 end marker
        int top = 0, left = 0, bottom = 0, right = 0;
        std::vector<Channel> channels;
        uint32_t id = 0;
    };

    uint16_t sampleAt(const std::vector<uint8_t>& p, size_t i, int bps) {
        if (bps == 1) return p[i];
        uint16_t v;
        std::memcpy(&v, p.data() + i * 2, 2);
        return v;
    }

    // one plane (w x h samples, big-endian when 16-bit) as a channel: PackBits (8) or ZIP with prediction (16)
    std::vector<uint8_t> encodePlane(const std::vector<uint8_t>& plane, int w, int h, int bps) {
        std::vector<uint8_t> out;
        if (w == 0 || h == 0) {
            out = {0, 0};
            return out;
        }
        if (bps == 1) {
            out = {0, 1};
            std::vector<std::vector<uint8_t>> rows((size_t)h);
            for (int y = 0; y < h; y++) packBits(plane.data() + (size_t)y * w, (size_t)w, rows[(size_t)y]);
            for (const auto& row : rows) {
                out.push_back((uint8_t)(row.size() >> 8));
                out.push_back((uint8_t)row.size());
            }
            for (const auto& row : rows) out.insert(out.end(), row.begin(), row.end());
            return out;
        }
        // 16-bit: per row deltas of big-endian samples, then zlib
        std::vector<uint8_t> delta(plane.size());
        for (int y = 0; y < h; y++) {
            uint16_t prev = 0;
            for (int x = 0; x < w; x++) {
                const size_t i = (size_t)y * w + x;
                uint16_t v;
                std::memcpy(&v, plane.data() + i * 2, 2);
                const uint16_t d = (uint16_t)(v - prev);
                prev = v;
                delta[i * 2] = (uint8_t)(d >> 8);
                delta[i * 2 + 1] = (uint8_t)d;
            }
        }
        mz_ulong length = mz_compressBound((mz_ulong)delta.size());
        std::vector<uint8_t> packed(length);
        mz_compress2(packed.data(), &length, delta.data(), (mz_ulong)delta.size(), 6);
        packed.resize(length);
        out = {0, 3};
        out.insert(out.end(), packed.begin(), packed.end());
        return out;
    }

    // the layer's channels: its pixels trimmed to alpha > 0, transparent colour as 0
    void pixelChannels(const PsdWriteLayer& l, int bps, Record& rec) {
        int x0 = l.width, y0 = l.height, x1 = -1, y1 = -1;
        const size_t pixelBytes = 4 * (size_t)bps;
        const bool ok = l.width > 0 && l.height > 0 && l.rgba.size() == (size_t)l.width * l.height * pixelBytes;
        if (ok) {
            for (int y = 0; y < l.height; y++) {
                for (int x = 0; x < l.width; x++) {
                    if (sampleAt(l.rgba, ((size_t)y * l.width + x) * 4 + 3, bps) == 0) continue;
                    x0 = std::min(x0, x);
                    y0 = std::min(y0, y);
                    x1 = std::max(x1, x);
                    y1 = std::max(y1, y);
                }
            }
        }
        const int w = x1 >= x0 ? x1 - x0 + 1 : 0, h = y1 >= y0 ? y1 - y0 + 1 : 0;
        rec.left = w ? l.x + x0 : 0;
        rec.top = h ? l.y + y0 : 0;
        rec.right = rec.left + w;
        rec.bottom = rec.top + h;
        std::vector<uint8_t> planes[4];
        for (auto& p : planes) p.assign((size_t)w * h * bps, 0);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const size_t src = ((size_t)(y + y0) * l.width + (x + x0)) * 4;
                const size_t dst = (size_t)y * w + x;
                const uint16_t a = sampleAt(l.rgba, src + 3, bps);
                for (int c = 0; c < 4; c++) {
                    const uint16_t v = c == 3 ? a : (a ? sampleAt(l.rgba, src + c, bps) : 0);
                    if (bps == 1) planes[c][dst] = (uint8_t)v;
                    else std::memcpy(planes[c].data() + dst * 2, &v, 2);
                }
            }
        }
        rec.channels.push_back({-1, encodePlane(planes[3], w, h, bps)});
        for (int c = 0; c < 3; c++) rec.channels.push_back({(int16_t)c, encodePlane(planes[c], w, h, bps)});
        if (l.hasMask) {
            std::vector<uint8_t> mask = l.mask;
            if (mask.size() != (size_t)l.maskWidth * l.maskHeight * bps) mask.assign((size_t)l.maskWidth * l.maskHeight * bps, 255);
            rec.channels.push_back({-2, encodePlane(mask, l.maskWidth, l.maskHeight, bps)});
        }
    }

    void emptyChannels(Record& rec) {
        for (int16_t id : {(int16_t)-1, (int16_t)0, (int16_t)1, (int16_t)2}) rec.channels.push_back({id, {0, 0}});
    }

    void flatten(const std::vector<PsdWriteLayer>& layers, int bps, std::vector<Record>& out, uint32_t& nextId) {
        for (const PsdWriteLayer& l : layers) {
            if (l.group) {
                Record end;
                end.layer = &l;
                end.section = 3;
                emptyChannels(end);
                end.id = nextId++;
                out.push_back(std::move(end));
                flatten(l.children, bps, out, nextId);
                Record open;
                open.layer = &l;
                open.section = l.groupOpen ? 1 : 2;
                emptyChannels(open);
                open.id = nextId++;
                out.push_back(std::move(open));
                continue;
            }
            Record rec;
            rec.layer = &l;
            if (l.adjustmentKey.empty()) {
                pixelChannels(l, bps, rec);
            } else {
                emptyChannels(rec);
                if (l.hasMask) {
                    std::vector<uint8_t> mask = l.mask;
                    if (mask.size() != (size_t)l.maskWidth * l.maskHeight * bps) mask.assign((size_t)l.maskWidth * l.maskHeight * bps, 255);
                    rec.channels.push_back({-2, encodePlane(mask, l.maskWidth, l.maskHeight, bps)});
                }
            }
            rec.id = nextId++;
            out.push_back(std::move(rec));
        }
    }

    void block(ByteWriter& w, const char* key, const std::vector<uint8_t>& data) {
        w.fourcc("8BIM");
        w.fourcc(key);
        std::vector<uint8_t> padded = data;
        while (padded.size() % 4) padded.push_back(0);
        w.u32((uint32_t)padded.size());
        w.bytes(padded);
    }

    void writeRecord(ByteWriter& w, const Record& rec) {
        const PsdWriteLayer& l = *rec.layer;
        w.i32(rec.top);
        w.i32(rec.left);
        w.i32(rec.bottom);
        w.i32(rec.right);
        w.u16((uint16_t)rec.channels.size());
        for (const Channel& c : rec.channels) {
            w.i16(c.id);
            w.u32((uint32_t)c.data.size());
        }
        w.fourcc("8BIM");
        const std::string blend = rec.section == 3 ? std::string("norm") : (l.blendKey.size() == 4 ? l.blendKey : std::string("norm"));
        w.fourcc(blend == "pass" ? "norm" : blend.c_str());
        w.u8(rec.section == 3 ? 255 : l.opacity);
        w.u8(rec.section == 0 && l.clipping ? 1 : 0);
        uint8_t flags = 0x08;
        if (!l.visible && rec.section != 3) flags |= 0x02;
        if (rec.section != 0) flags |= 0x10;
        w.u8(flags);
        w.u8(0);
        const size_t extra = w.placeholder32();
        if (l.hasMask && rec.section == 0) {
            w.u32(20);
            w.i32(l.maskY);
            w.i32(l.maskX);
            w.i32(l.maskY + l.maskHeight);
            w.i32(l.maskX + l.maskWidth);
            w.u8(l.maskDefault);
            w.u8(0);
            w.u16(0);
        } else {
            w.u32(0);
        }
        w.u32(0);       // blending ranges
        const std::string name = rec.section == 3 ? std::string("</Layer group>") : l.name;
        w.pascal(name, 4);
        {
            ByteWriter u;
            u.unicode(name);
            block(w, "luni", u.out);
        }
        {
            ByteWriter id;
            id.u32(rec.id);
            block(w, "lyid", id.out);
        }
        if (rec.section != 0) {
            ByteWriter s;
            s.u32((uint32_t)rec.section);
            if (rec.section != 3) {
                s.fourcc("8BIM");
                s.fourcc(l.blendKey == "pass" ? "pass" : (l.blendKey.size() == 4 ? l.blendKey.c_str() : "norm"));
            }
            block(w, "lsct", s.out);
        }
        if (rec.section == 0 && l.fillOpacity != 255) block(w, "iOpa", {l.fillOpacity, 0, 0, 0});
        if (rec.section == 0 && !l.adjustmentKey.empty() && l.adjustmentKey.size() == 4) block(w, l.adjustmentKey.c_str(), l.adjustmentData);
        w.patchLength(extra);
    }

    // count, records, channel data
    std::vector<uint8_t> layerInfoBody(const std::vector<Record>& records, bool mergedAlpha) {
        ByteWriter w;
        const int16_t count = (int16_t)records.size();
        w.i16(mergedAlpha ? (int16_t)-count : count);
        for (const Record& r : records) writeRecord(w, r);
        for (const Record& r : records) {
            for (const Channel& c : r.channels) w.bytes(c.data);
        }
        return w.out;
    }

    void resource(ByteWriter& w, uint16_t id, const std::vector<uint8_t>& data) {
        w.fourcc("8BIM");
        w.u16(id);
        w.u16(0);       // empty Pascal name, padded to even
        w.u32((uint32_t)data.size());
        w.bytes(data);
        if (data.size() % 2) w.u8(0);
    }

}

bool psd::writePsd(const PsdWriteDocument& doc, std::vector<uint8_t>& out, std::string& error) {
    if (doc.bitDepth != 8 && doc.bitDepth != 16) {
        error = "bad_bit_depth: a PSD is 8- or 16-bit, not " + std::to_string(doc.bitDepth);
        return false;
    }
    if (doc.width <= 0 || doc.height <= 0 || doc.width > PSD_MAX_SIDE || doc.height > PSD_MAX_SIDE) {
        error = "psd_too_large: a PSD is at most " + std::to_string(PSD_MAX_SIDE) + " px a side; this one is " + std::to_string(doc.width) + " x " +
                std::to_string(doc.height);
        return false;
    }
    const int bps = doc.bitDepth / 8;
    const size_t mergedBytes = (size_t)doc.width * doc.height * 4 * bps;
    if (!doc.merged.empty() && doc.merged.size() != mergedBytes) {
        error = "bad_pixels: the merged image is not " + std::to_string(doc.width) + " x " + std::to_string(doc.height) + " RGBA";
        return false;
    }
    std::vector<Record> records;
    uint32_t nextId = 1;
    flatten(doc.layers, bps, records, nextId);
    if (records.size() > 32767) {
        error = "psd_too_large: more than 32,767 layer records";
        return false;
    }

    ByteWriter w;
    w.fourcc("8BPS");
    w.u16(1);
    w.zeros(6);
    w.u16(4);
    w.u32((uint32_t)doc.height);
    w.u32((uint32_t)doc.width);
    w.u16((uint16_t)doc.bitDepth);
    w.u16(3);
    w.u32(0);       // colour mode data

    {
        ByteWriter res;
        ByteWriter r;
        const uint32_t fixedDpi = (uint32_t)std::lround(std::clamp(doc.dpi, 1.0, 30000.0) * 65536.0);
        r.u32(fixedDpi);
        r.u16(1);
        r.u16(1);
        r.u32(fixedDpi);
        r.u16(1);
        r.u16(1);
        resource(res, 0x03ED, r.out);
        resource(res, 0x040F, doc.icc.empty() ? srgbIccProfile() : doc.icc);
        ByteWriter v;
        v.u32(1);
        v.u8(1);
        v.unicode("JasperEngine");
        v.unicode("JasperEngine");
        v.u32(1);
        resource(res, 0x0421, v.out);
        w.u32((uint32_t)res.size());
        w.bytes(res.out);
    }

    {
        const size_t section = w.placeholder32();
        const std::vector<uint8_t> body = layerInfoBody(records, true);
        if (bps == 1) {
            std::vector<uint8_t> padded = body;
            while (padded.size() % 4) padded.push_back(0);
            w.u32((uint32_t)padded.size());
            w.bytes(padded);
            w.u32(0);   // global layer mask
        } else {
            // 16-bit layers live in the document-level Lr16 block
            w.u32(0);
            w.u32(0);
            block(w, "Lr16", body);
        }
        w.patchLength(section);
    }

    // merged image
    std::vector<uint8_t> planes[4];
    for (auto& p : planes) p.assign((size_t)doc.width * doc.height * bps, 0);
    if (!doc.merged.empty()) {
        for (size_t i = 0; i < (size_t)doc.width * doc.height; i++) {
            for (int c = 0; c < 4; c++) {
                if (bps == 1) {
                    planes[c][i] = doc.merged[i * 4 + c];
                } else {
                    uint16_t v;
                    std::memcpy(&v, doc.merged.data() + (i * 4 + c) * 2, 2);
                    planes[c][i * 2] = (uint8_t)(v >> 8);
                    planes[c][i * 2 + 1] = (uint8_t)v;
                }
            }
        }
    }
    if (bps == 1) {
        w.u16(1);
        std::vector<std::vector<uint8_t>> rows;
        for (int c = 0; c < 4; c++) {
            for (int y = 0; y < doc.height; y++) {
                rows.emplace_back();
                packBits(planes[c].data() + (size_t)y * doc.width, (size_t)doc.width, rows.back());
            }
        }
        for (const auto& row : rows) w.u16((uint16_t)row.size());
        for (const auto& row : rows) w.bytes(row);
    } else {
        w.u16(0);
        for (int c = 0; c < 4; c++) w.bytes(planes[c]);
    }
    out = std::move(w.out);
    return true;
}
