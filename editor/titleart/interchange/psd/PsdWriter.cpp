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
#include <fstream>
#include <functional>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::psd;

namespace {

    struct Channel {
        int16_t id = 0;
        std::vector<uint8_t> data;      // compression + payload
        // or, instead of `data`, where it is in the spool
        bool spooled = false;
        uint64_t offset = 0, length = 0;

        uint64_t size() const { return spooled ? length : data.size(); }
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

    using EmitChannel = std::function<bool(int16_t id, std::vector<uint8_t> data)>;

    // the layer's channels: its pixels trimmed to alpha > 0, transparent colour as 0. One plane at
    // a time, so a full-frame layer never holds four planes at once (R49)
    bool pixelChannels(const PsdWriteLayer& l, int bps, Record& rec, const EmitChannel& emit) {
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
        for (int c : {3, 0, 1, 2}) {
            std::vector<uint8_t> plane((size_t)w * h * bps, 0);
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    const size_t src = ((size_t)(y + y0) * l.width + (x + x0)) * 4;
                    const size_t dst = (size_t)y * w + x;
                    const uint16_t a = sampleAt(l.rgba, src + 3, bps);
                    const uint16_t v = c == 3 ? a : (a ? sampleAt(l.rgba, src + c, bps) : 0);
                    if (bps == 1) plane[dst] = (uint8_t)v;
                    else std::memcpy(plane.data() + dst * 2, &v, 2);
                }
            }
            if (!emit(c == 3 ? (int16_t)-1 : (int16_t)c, encodePlane(plane, w, h, bps))) return false;
        }
        return true;
    }

    std::vector<uint8_t> maskChannel(const PsdWriteLayer& l, int bps) {
        std::vector<uint8_t> mask = l.mask;
        if (mask.size() != (size_t)l.maskWidth * l.maskHeight * bps) mask.assign((size_t)l.maskWidth * l.maskHeight * bps, 255);
        return encodePlane(mask, l.maskWidth, l.maskHeight, bps);
    }

    Channel spooledChannel(const PsdSpooledChannel& c) {
        Channel out;
        out.id = c.id;
        out.spooled = true;
        out.offset = c.offset;
        out.length = c.length;
        return out;
    }

    void emptyChannels(Record& rec) {
        for (int16_t id : {(int16_t)-1, (int16_t)0, (int16_t)1, (int16_t)2}) {
            Channel c;
            c.id = id;
            c.data = {0, 0};
            rec.channels.push_back(std::move(c));
        }
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
            if (!l.adjustmentKey.empty()) {
                emptyChannels(rec);
            } else if (l.pixelsSpooled) {
                rec.left = l.spooledLeft;
                rec.top = l.spooledTop;
                rec.right = l.spooledRight;
                rec.bottom = l.spooledBottom;
                for (const PsdSpooledChannel& c : l.spooledPixels) rec.channels.push_back(spooledChannel(c));
            } else {
                pixelChannels(l, bps, rec, [&rec](int16_t id, std::vector<uint8_t> data) {
                    Channel c;
                    c.id = id;
                    c.data = std::move(data);
                    rec.channels.push_back(std::move(c));
                    return true;
                });
            }
            if (l.maskSpooled) {
                rec.channels.push_back(spooledChannel(l.spooledMask));
            } else if (l.hasMask) {
                Channel c;
                c.id = -2;
                c.data = maskChannel(l, bps);
                rec.channels.push_back(std::move(c));
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
            w.u32((uint32_t)c.size());
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

    void resource(ByteWriter& w, uint16_t id, const std::vector<uint8_t>& data) {
        w.fourcc("8BIM");
        w.u16(id);
        w.u16(0);       // empty Pascal name, padded to even
        w.u32((uint32_t)data.size());
        w.bytes(data);
        if (data.size() % 2) w.u8(0);
    }

    // where the file's bytes go: memory, or a file with the channels copied from the spool
    class Sink {
    public:
        virtual ~Sink() = default;
        virtual bool put(const std::vector<uint8_t>& bytes) = 0;
        virtual bool channel(const Channel& c) = 0;
    };

    class MemorySink : public Sink {
    public:
        explicit MemorySink(std::vector<uint8_t>& out) : out(out) {}
        bool put(const std::vector<uint8_t>& bytes) override {
            out.insert(out.end(), bytes.begin(), bytes.end());
            return true;
        }
        bool channel(const Channel& c) override { return put(c.data); }

    private:
        std::vector<uint8_t>& out;
    };

    class FileSink : public Sink {
    public:
        FileSink(io::AtomicFileWriter& out, PsdChannelSpool& spool, std::string& error) : out(out), spool(spool), error(error) {}
        bool put(const std::vector<uint8_t>& bytes) override {
            if (out.write(bytes.data(), bytes.size())) return true;
            error = "write_failed: the disk refused the PSD: " + io::describeWriteError(out.result());
            return false;
        }
        bool channel(const Channel& c) override { return c.spooled ? spool.copy(c.offset, c.length, out, error) : put(c.data); }

    private:
        io::AtomicFileWriter& out;
        PsdChannelSpool& spool;
        std::string& error;
    };

    // the merged image section: compression tag, then the data
    std::vector<uint8_t> encodeMerged(int width, int height, int bps, const std::vector<uint8_t>& merged) {
        const size_t count = (size_t)width * height;
        ByteWriter w;
        if (bps == 1) {
            w.u16(1);
            std::vector<std::vector<uint8_t>> rows;
            rows.reserve((size_t)height * 4);
            std::vector<uint8_t> plane(merged.empty() ? count : 0, 0);
            for (int c = 0; c < 4; c++) {
                if (!merged.empty()) {
                    plane.resize(count);
                    for (size_t i = 0; i < count; i++) plane[i] = merged[i * 4 + c];
                }
                for (int y = 0; y < height; y++) {
                    rows.emplace_back();
                    packBits(plane.data() + (size_t)y * width, (size_t)width, rows.back());
                }
            }
            for (const auto& row : rows) w.u16((uint16_t)row.size());
            for (const auto& row : rows) w.bytes(row);
            return std::move(w.out);
        }
        w.u16(0);
        w.out.resize(2 + count * 8, 0);
        if (!merged.empty()) {
            size_t at = 2;
            for (int c = 0; c < 4; c++) {
                for (size_t i = 0; i < count; i++) {
                    uint16_t v;
                    std::memcpy(&v, merged.data() + (i * 4 + c) * 2, 2);
                    w.out[at++] = (uint8_t)(v >> 8);
                    w.out[at++] = (uint8_t)v;
                }
            }
        }
        return std::move(w.out);
    }

    bool writeDocument(const PsdWriteDocument& doc, Sink& sink, std::string& error) {
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

        // the layer section: its lengths come from the record bytes and the channels' sizes, so
        // the channels themselves never have to be in memory together
        ByteWriter info;
        info.i16((int16_t)-(int)records.size());
        for (const Record& r : records) writeRecord(info, r);
        uint64_t body = info.size();
        for (const Record& r : records) {
            for (const Channel& c : r.channels) body += c.size();
        }
        const uint64_t padded = (body + 3) / 4 * 4;
        if (padded > (uint64_t)std::numeric_limits<int32_t>::max() - 64) {
            error = "psd_too_large: the layers need " + std::to_string(padded) + " bytes, over the format's 2 GB";
            return false;
        }
        if (bps == 1) {
            w.u32((uint32_t)(8 + padded));
            w.u32((uint32_t)padded);
        } else {
            // 16-bit layers live in the document-level Lr16 block
            w.u32((uint32_t)(20 + padded));
            w.u32(0);
            w.u32(0);
            w.fourcc("8BIM");
            w.fourcc("Lr16");
            w.u32((uint32_t)padded);
        }
        if (!sink.put(w.out) || !sink.put(info.out)) return false;
        for (const Record& r : records) {
            for (const Channel& c : r.channels) {
                if (!sink.channel(c)) return false;
            }
        }
        ByteWriter end;
        end.zeros((size_t)(padded - body));
        if (bps == 1) end.u32(0);   // global layer mask
        if (!end.out.empty() && !sink.put(end.out)) return false;

        if (doc.mergedSpooled) {
            Channel merged;
            merged.spooled = true;
            merged.offset = doc.mergedOffset;
            merged.length = doc.mergedLength;
            return sink.channel(merged);
        }
        return sink.put(encodeMerged(doc.width, doc.height, bps, doc.merged));
    }

}

bool psd::writePsd(const PsdWriteDocument& doc, std::vector<uint8_t>& out, std::string& error) {
    std::vector<uint8_t> bytes;
    MemorySink sink(bytes);
    if (!writeDocument(doc, sink, error)) return false;
    out = std::move(bytes);
    return true;
}

bool psd::writePsdFile(const PsdWriteDocument& doc, PsdChannelSpool& spool, const std::filesystem::path& output, std::string& error) {
    io::AtomicWriteOptions options;
    options.durability = io::Durability::Cache;
    io::AtomicFileWriter file;
    if (!file.open(output, options)) {
        error = "write_failed: cannot write " + output.generic_string() + ": " + io::describeWriteError(file.result());
        return false;
    }
    FileSink sink(file, spool, error);
    if (!writeDocument(doc, sink, error)) return false;
    const io::WriteResult published = file.commit();
    if (!published.ok()) {
        error = "write_failed: the disk refused " + output.generic_string() + ": " + io::describeWriteError(published);
        return false;
    }
    return true;
}

bool psd::psdSpoolPixels(PsdWriteLayer& layer, int bitDepth, PsdChannelSpool& spool, std::string& error) {
    Record rec;
    const bool ok = pixelChannels(layer, bitDepth / 8, rec, [&](int16_t id, std::vector<uint8_t> data) {
        PsdSpooledChannel c;
        c.id = id;
        c.length = data.size();
        if (!spool.append(data, c.offset, error)) return false;
        layer.spooledPixels.push_back(c);
        return true;
    });
    if (!ok) return false;
    layer.spooledLeft = rec.left;
    layer.spooledTop = rec.top;
    layer.spooledRight = rec.right;
    layer.spooledBottom = rec.bottom;
    layer.pixelsSpooled = true;
    std::vector<uint8_t>().swap(layer.rgba);
    return true;
}

bool psd::psdSpoolMask(PsdWriteLayer& layer, int bitDepth, PsdChannelSpool& spool, std::string& error) {
    std::vector<uint8_t> data = maskChannel(layer, bitDepth / 8);
    layer.spooledMask.id = -2;
    layer.spooledMask.length = data.size();
    if (!spool.append(data, layer.spooledMask.offset, error)) return false;
    layer.maskSpooled = true;
    std::vector<uint8_t>().swap(layer.mask);
    return true;
}

bool psd::psdSpoolMerged(PsdWriteDocument& doc, PsdChannelSpool& spool, std::string& error) {
    const std::vector<uint8_t> data = encodeMerged(doc.width, doc.height, doc.bitDepth / 8, doc.merged);
    doc.mergedLength = data.size();
    if (!spool.append(data, doc.mergedOffset, error)) return false;
    doc.mergedSpooled = true;
    std::vector<uint8_t>().swap(doc.merged);
    return true;
}

// ---- the spool ----

PsdChannelSpool::~PsdChannelSpool() = default;

bool PsdChannelSpool::open(const std::filesystem::path& where, std::string& error) {
    size = 0;
    io::AtomicWriteOptions options;
    options.durability = io::Durability::Cache;
    if (!working.open(where, options)) {
        error = "write_failed: cannot write " + where.generic_string() + ": " + io::describeWriteError(working.result());
        return false;
    }
    return true;
}

bool PsdChannelSpool::append(const std::vector<uint8_t>& data, uint64_t& offset, std::string& error) {
    offset = size;
    if (!working.write(data.data(), data.size())) {
        error = "write_failed: the disk refused the PSD's working file: " + io::describeWriteError(working.result());
        return false;
    }
    size += data.size();
    return true;
}

bool PsdChannelSpool::copy(uint64_t offset, uint64_t length, io::AtomicFileWriter& to, std::string& error) {
    std::ifstream file(working.temporaryPath(), std::ios::binary);
    file.seekg((std::streamoff)offset);
    std::vector<char> buffer(1 << 20);
    for (uint64_t left = length; left > 0;) {
        const size_t n = (size_t)std::min<uint64_t>(left, buffer.size());
        file.read(buffer.data(), (std::streamsize)n);
        if (!file || !to.write(buffer.data(), n)) {
            error = "write_failed: copying the PSD's channels from " + working.temporaryPath().generic_string();
            return false;
        }
        left -= n;
    }
    return true;
}
