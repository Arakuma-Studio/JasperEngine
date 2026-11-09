// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Written from the Adobe Photoshop File Formats Specification (file header, image resources,
// layer and mask information, image data, additional layer information).

#include "PsdReader.h"

#include "miniz.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace jasperengine::editor;
using namespace jasperengine::editor::psd;

namespace {

    constexpr int MAX_SIDE = 300000;
    constexpr size_t MAX_LAYERS = 4096;
    constexpr size_t MAX_CHANNEL_BYTES = 64u << 20;

    // additional information keys whose length is 8 bytes in a PSB
    bool wideKey(const std::string& key) {
        static const char* keys[] = {"LMsk", "Lr16", "Lr32", "Layr", "Mt16", "Mt32", "Mtrn", "Alph", "FMsk", "lnk2", "FEid", "FXid", "PxSD"};
        for (const char* k : keys) if (key == k) return true;
        return false;
    }

    bool adjustmentKey(const std::string& k) {
        static const char* keys[] = {"levl", "curv", "hue2", "hue ", "brit", "CgEd", "expA", "vibA", "blnc", "blwh", "phfl", "grdm", "nvrt",
                                     "post", "thrs", "selc", "mixr", "clrL"};
        for (const char* key : keys) if (k == key) return true;
        return false;
    }

    struct ChannelInfo {
        int16_t id = 0;
        uint64_t length = 0;
    };

    // decodes one plane of w x h samples (bytes per sample `bps`) in compression `c` from `in`
    bool decodePlane(ByteReader& in, uint16_t c, int w, int h, int bps, bool psb, PsdPlane& out, const std::string& what) {
        const size_t rowBytes = (size_t)w * bps;
        const size_t total = rowBytes * h;
        if (total > MAX_CHANNEL_BYTES) return in.fail(what + " is larger than 64 MiB");
        out.samples.assign(total, 0);
        if (total == 0) return true;
        if (c == 0) {
            const uint8_t* p = in.view(total);
            if (!p) return false;
            std::copy(p, p + total, out.samples.begin());
        } else if (c == 1) {
            std::vector<uint32_t> counts((size_t)h);
            for (int y = 0; y < h; y++) counts[(size_t)y] = psb ? in.u32() : in.u16();
            for (int y = 0; y < h && in.ok(); y++) {
                if (!unpackBits(in, counts[(size_t)y], out.samples.data() + (size_t)y * rowBytes, rowBytes)) return in.fail(what + ": a PackBits row is malformed");
            }
        } else if (c == 2 || c == 3) {
            const size_t n = in.remaining();
            const uint8_t* p = in.view(n);
            if (!p) return false;
            mz_ulong length = (mz_ulong)total;
            if (mz_uncompress(out.samples.data(), &length, p, (mz_ulong)n) != MZ_OK || length != total) return in.fail(what + ": its ZIP data does not inflate");
            if (c == 3) {
                for (int y = 0; y < h; y++) {
                    uint8_t* row = out.samples.data() + (size_t)y * rowBytes;
                    if (bps == 1) {
                        for (int x = 1; x < w; x++) row[x] = (uint8_t)(row[x] + row[x - 1]);
                    } else {
                        uint16_t prev = (uint16_t)(row[0] << 8 | row[1]);
                        for (int x = 1; x < w; x++) {
                            const uint16_t v = (uint16_t)(prev + (uint16_t)(row[2 * x] << 8 | row[2 * x + 1]));
                            row[2 * x] = (uint8_t)(v >> 8);
                            row[2 * x + 1] = (uint8_t)v;
                            prev = v;
                        }
                    }
                }
            }
        } else {
            return in.fail(what + ": compression " + std::to_string(c) + " is unknown");
        }
        // 16-bit samples are big-endian in the file: native from here on
        if (bps == 2) {
            for (size_t i = 0; i + 1 < total; i += 2) {
                const uint16_t v = (uint16_t)(out.samples[i] << 8 | out.samples[i + 1]);
                std::memcpy(out.samples.data() + i, &v, 2);
            }
        }
        return in.ok();
    }

    bool readRect(ByteReader& r, PsdRect& rect) {
        rect.top = r.i32();
        rect.left = r.i32();
        rect.bottom = r.i32();
        rect.right = r.i32();
        if (!r.ok()) return false;
        if (std::abs((int64_t)rect.bottom - rect.top) > MAX_SIDE || std::abs((int64_t)rect.right - rect.left) > MAX_SIDE) {
            return r.fail("a layer of " + std::to_string((int64_t)rect.right - rect.left) + " x " + std::to_string((int64_t)rect.bottom - rect.top) + " px");
        }
        if (rect.bottom < rect.top || rect.right < rect.left) return r.fail("a layer's bounds are inverted");
        return true;
    }

    void readAdditional(ByteReader& r, PsdLayerRecord& layer, bool psb) {
        while (r.remaining() >= 12 && r.ok()) {
            const std::string sig = r.fourcc();
            if (sig != "8BIM" && sig != "8B64") {
                r.fail("an additional information block without its '8BIM' signature");
                return;
            }
            const std::string key = r.fourcc();
            const uint64_t length = r.length(psb && wideKey(key), ("block '" + key + "'").c_str());
            ByteReader block = r.sub((size_t)length);
            // blocks are padded to an even length
            if (length % 2 && r.remaining()) r.skip(1);
            layer.keys.push_back(key);
            if (key == "luni") {
                layer.name = block.unicode();
            } else if (key == "lyid") {
                layer.id = block.u32();
            } else if (key == "lsct" || key == "lsdk") {
                layer.section = (int)block.u32();
                if (block.remaining() >= 8 && block.fourcc() == "8BIM") layer.sectionBlend = block.fourcc();
            } else if (key == "iOpa") {
                layer.fillOpacity = block.u8();
            } else if (key == "knko") {
                layer.knockout = block.u8() != 0;
            } else if (key == "lfx2" || key == "lmfx") {
                block.u32();    // object effects version
                Json fx;
                if (readVersionedDescriptor(block, fx) && (key == "lmfx" || layer.effects.is_null())) layer.effects = std::move(fx);
            } else if (key == "TySh") {
                layer.text = true;
                block.u16();
                for (double& v : layer.transform) v = block.f64();
                block.u16();
                Json text;
                if (readVersionedDescriptor(block, text)) layer.textDescriptor = std::move(text);
                block.u16();
                Json warp;
                if (block.ok() && readVersionedDescriptor(block, warp)) {
                    const std::string style = descEnum(warp, "warpStyle");
                    layer.warp = !style.empty() && style != "warpNone";
                }
            } else if (adjustmentKey(key)) {
                layer.adjustmentKey = key;
                const size_t rest = block.remaining();
                if (const uint8_t* p = block.view(rest)) layer.adjustmentData.assign(p, p + rest);
            } else if (key == "SoCo" || key == "GdFl" || key == "PtFl") {
                layer.fillKey = key;
                Json d;
                if (readVersionedDescriptor(block, d)) layer.fillDescriptor = std::move(d);
            } else if (key == "SoLd" || key == "PlLd" || key == "SoLE") {
                layer.smartObject = true;
            } else if (key == "vmsk" || key == "vsms") {
                block.u32();
                const uint32_t flags = block.u32();
                layer.vectorMaskInvert = flags & 1;
                layer.vectorMaskDisabled = flags & 4;
                const size_t rest = block.remaining();
                if (const uint8_t* p = block.view(rest)) layer.vectorMask.assign(p, p + rest);
            }
        }
    }

    // the layer records and their channel data (a layer info section's body)
    bool readLayerInfo(ByteReader& r, PsdFile& file, bool psb, int bps, const PsdReadOptions& options) {
        const int16_t rawCount = r.i16();
        const size_t count = (size_t)std::abs((int)rawCount);
        if (!r.ok()) return false;
        if (count > MAX_LAYERS) return r.fail("a file of " + std::to_string(count) + " layers (at most 4,096)");
        std::vector<PsdLayerRecord> layers(count);
        std::vector<std::vector<ChannelInfo>> channels(count);
        for (size_t i = 0; i < count && r.ok(); i++) {
            PsdLayerRecord& l = layers[i];
            if (!readRect(r, l.bounds)) return false;
            const uint16_t n = r.u16();
            if (n > 56) return r.fail("a layer with " + std::to_string(n) + " channels");
            for (uint16_t c = 0; c < n; c++) {
                ChannelInfo ci;
                ci.id = r.i16();
                ci.length = psb ? r.u64() : r.u32();
                channels[i].push_back(ci);
            }
            if (r.fourcc() != "8BIM" && r.ok()) return r.fail("a layer record without its blend signature");
            l.blendKey = r.fourcc();
            l.opacity = r.u8();
            l.clipping = r.u8() != 0;
            const uint8_t flags = r.u8();
            l.transparencyProtected = flags & 1;
            l.visible = !(flags & 2);
            r.u8();
            const uint64_t extra = r.length(false, "a layer's extra data");
            ByteReader e = r.sub((size_t)extra);
            const uint64_t maskLength = e.length(false, "a layer mask");
            ByteReader mask = e.sub((size_t)maskLength);
            if (maskLength >= 18) {
                l.hasMask = true;
                readRect(mask, l.maskBounds);
                l.maskDefault = mask.u8();
                const uint8_t mflags = mask.u8();
                l.maskDisabled = mflags & 2;
                l.maskInvert = mflags & 4;
            }
            const uint64_t blendLength = e.length(false, "blending ranges");
            e.skip((size_t)blendLength);
            l.name = e.pascal(4);
            readAdditional(e, l, psb);
            if (!e.ok()) return r.fail("layer " + std::to_string(i) + " ('" + l.name + "'): " + e.failure());
        }
        // channel image data, layer by layer
        for (size_t i = 0; i < count && r.ok(); i++) {
            PsdLayerRecord& l = layers[i];
            for (const ChannelInfo& ci : channels[i]) {
                if (ci.length > r.remaining()) return r.fail("channel data of layer '" + l.name + "' passes the end");
                ByteReader ch = r.sub((size_t)ci.length);
                if (!options.layerPixels || ci.length < 2 || !file.rgbOrGray() || (file.depth != 8 && file.depth != 16)) continue;
                const uint16_t compression = ch.u16();
                PsdPlane* plane = nullptr;
                int w = l.bounds.width(), h = l.bounds.height();
                if (ci.id == -2) {
                    plane = &l.maskPlane;
                    w = l.maskBounds.width();
                    h = l.maskBounds.height();
                } else if (ci.id == -1) {
                    plane = &l.alpha;
                } else if (ci.id >= 0 && ci.id < 3) {
                    plane = &l.planes[ci.id];
                }
                if (!plane || w == 0 || h == 0) continue;
                if (!decodePlane(ch, compression, w, h, bps, psb, *plane, "layer '" + l.name + "' channel " + std::to_string(ci.id))) {
                    return r.fail(ch.failure());
                }
            }
        }
        if (!r.ok()) return false;
        for (PsdLayerRecord& l : layers) file.layers.push_back(std::move(l));
        return true;
    }

    void readResources(ByteReader& r, PsdFile& file) {
        while (r.remaining() >= 12 && r.ok()) {
            const std::string sig = r.fourcc();
            if (sig != "8BIM" && sig != "MeSa" && sig != "AgHg" && sig != "PHUT" && sig != "DCSR") {
                r.fail("an image resource without its '8BIM' signature");
                return;
            }
            const uint16_t id = r.u16();
            r.pascal(2);
            const uint64_t length = r.length(false, "an image resource");
            ByteReader block = r.sub((size_t)length);
            if (length % 2) r.skip(1);
            if (id == 0x03ED && length >= 16) {
                const double hRes = block.u32() / 65536.0;
                const uint16_t unit = block.u16();
                file.dpi = unit == 2 ? hRes * 2.54 : hRes;
            } else if (id == 0x040D && length >= 4) {
                file.globalAngle = block.i32();
            } else if (id == 0x0419 && length >= 4) {
                file.globalAltitude = block.i32();
            } else if (id == 0x040F) {
                const uint8_t* p = block.view((size_t)length);
                if (p) file.icc.assign(p, p + (size_t)length);
            } else if (id == 0x0421 && length >= 5) {
                block.u32();
                file.hasRealMergedData = block.u8() != 0;
            }
        }
    }

}

const char* psd::psdColorModeName(uint16_t mode) {
    switch (mode) {
        case 0: return "Bitmap";
        case 1: return "Grayscale";
        case 2: return "Indexed";
        case 3: return "RGB";
        case 4: return "CMYK";
        case 7: return "Multichannel";
        case 8: return "Duotone";
        case 9: return "Lab";
    }
    return "unknown";
}

bool psd::unpackBits(ByteReader& in, size_t packedBytes, uint8_t* out, size_t rowBytes) {
    if (packedBytes > in.remaining()) return false;
    ByteReader row = in.sub(packedBytes);
    size_t written = 0;
    while (!row.atEnd() && row.ok()) {
        const int8_t n = (int8_t)row.u8();
        if (n >= 0) {
            const size_t count = (size_t)n + 1;
            if (written + count > rowBytes) return false;
            const uint8_t* p = row.view(count);
            if (!p) return false;
            std::copy(p, p + count, out + written);
            written += count;
        } else if (n != -128) {
            const size_t count = (size_t)(1 - n);
            if (written + count > rowBytes || row.atEnd()) return false;
            const uint8_t v = row.u8();
            std::fill(out + written, out + written + count, v);
            written += count;
        }
    }
    return row.ok() && written == rowBytes;
}

void psd::packBits(const uint8_t* row, size_t n, std::vector<uint8_t>& out) {
    size_t i = 0;
    while (i < n) {
        // a run of 3 or more equal bytes, else a literal up to the next such run
        size_t run = 1;
        while (i + run < n && run < 128 && row[i + run] == row[i]) run++;
        if (run >= 3 || (run == 2 && i + run == n)) {
            out.push_back((uint8_t)(int8_t)(1 - (int)run));
            out.push_back(row[i]);
            i += run;
            continue;
        }
        size_t start = i, len = 0;
        while (i < n && len < 128) {
            size_t r = 1;
            while (i + r < n && r < 3 && row[i + r] == row[i]) r++;
            if (r >= 3) break;
            i++;
            len++;
        }
        out.push_back((uint8_t)(len - 1));
        out.insert(out.end(), row + start, row + start + len);
    }
}

bool psd::readPsd(const uint8_t* data, size_t size, PsdFile& out, std::string& error, const PsdReadOptions& options) {
    out = PsdFile();
    ByteReader r(data, size);
    if (r.fourcc() != "8BPS") {
        error = "not a Photoshop document (no '8BPS' signature)";
        return false;
    }
    out.version = r.u16();
    if (r.ok() && out.version != 1 && out.version != 2) {
        error = "Photoshop document version " + std::to_string(out.version) + " is unknown (1 = PSD, 2 = PSB)";
        return false;
    }
    const bool psb = out.version == 2;
    r.skip(6);
    out.channels = r.u16();
    const uint32_t height = r.u32(), width = r.u32();
    out.depth = r.u16();
    out.mode = r.u16();
    if (!r.ok()) {
        error = r.failure();
        return false;
    }
    if (width == 0 || height == 0 || width > (uint32_t)MAX_SIDE || height > (uint32_t)MAX_SIDE || out.channels == 0 || out.channels > 56) {
        error = "a document of " + std::to_string(width) + " x " + std::to_string(height) + " px, " + std::to_string(out.channels) + " channels";
        return false;
    }
    if (out.depth != 1 && out.depth != 8 && out.depth != 16 && out.depth != 32) {
        error = "bit depth " + std::to_string(out.depth) + " is unknown";
        return false;
    }
    out.width = (int)width;
    out.height = (int)height;
    const int bps = out.bytesPerSample();
    r.skip((size_t)r.length(false, "the color mode data"));
    {
        ByteReader resources = r.sub((size_t)r.length(false, "the image resources"));
        readResources(resources, out);
        if (!resources.ok()) {
            error = resources.failure();
            return false;
        }
    }
    {
        const uint64_t sectionLength = r.length(psb, "the layer and mask section");
        ByteReader section = r.sub((size_t)sectionLength);
        if (sectionLength > 0) {
            const uint64_t infoLength = section.length(psb, "the layer info");
            if (infoLength > 0) {
                ByteReader info = section.sub((size_t)infoLength);
                if (!readLayerInfo(info, out, psb, bps, options)) {
                    error = info.failure();
                    return false;
                }
            }
            section.skip((size_t)section.length(false, "the global layer mask"));
            // document-level additional information: 16- and 32-bit layers live in Lr16 / Lr32
            while (section.remaining() >= 12 && section.ok()) {
                const std::string sig = section.fourcc();
                if (sig != "8BIM" && sig != "8B64") break;
                const std::string key = section.fourcc();
                const uint64_t length = section.length(psb && wideKey(key), ("block '" + key + "'").c_str());
                ByteReader block = section.sub((size_t)length);
                while (section.remaining() && section.position() % 4) section.skip(1);
                if (key == "Patt" || key == "Pat2" || key == "Pat3") {
                    const size_t rest = block.remaining();
                    if (const uint8_t* p = block.view(rest)) out.patterns.insert(out.patterns.end(), p, p + rest);
                } else if ((key == "Lr16" || key == "Lr32") && out.layers.empty()) {
                    out.layersFrom16 = true;
                    if (!readLayerInfo(block, out, psb, bps, options)) {
                        error = block.failure();
                        return false;
                    }
                }
            }
        }
        if (!section.ok()) {
            error = section.failure();
            return false;
        }
    }
    // a length of the top-level sections that passed the end stops the reader here
    if (!r.ok()) {
        error = r.failure();
        return false;
    }
    if (options.mergedImage && r.remaining() >= 2 && out.rgbOrGray() && (out.depth == 8 || out.depth == 16)) {
        out.mergedCompression = r.u16();
        const int planes = std::min<int>(out.channels, out.mode == 3 ? 4 : 2);
        const size_t rowBytes = (size_t)out.width * bps;
        if (out.mergedCompression == 1) {
            const size_t rows = (size_t)out.channels * out.height;
            std::vector<uint32_t> counts(rows);
            for (size_t i = 0; i < rows; i++) counts[i] = psb ? r.u32() : r.u16();
            for (int c = 0; c < out.channels && r.ok(); c++) {
                PsdPlane plane;
                if (c < planes) plane.samples.assign(rowBytes * out.height, 0);
                for (int y = 0; y < out.height && r.ok(); y++) {
                    std::vector<uint8_t> row(rowBytes);
                    if (!unpackBits(r, counts[(size_t)c * out.height + y], row.data(), rowBytes)) {
                        r.fail("a PackBits row of the merged image is malformed");
                        break;
                    }
                    if (c < planes) std::copy(row.begin(), row.end(), plane.samples.begin() + (long)((size_t)y * rowBytes));
                }
                if (c < planes) out.merged[c] = std::move(plane);
            }
        } else if (out.mergedCompression == 0) {
            for (int c = 0; c < out.channels && r.ok(); c++) {
                const uint8_t* p = r.view(rowBytes * out.height);
                if (p && c < planes) out.merged[c].samples.assign(p, p + rowBytes * out.height);
            }
        } else {
            r.fail("merged image compression " + std::to_string(out.mergedCompression) + " is not raw or PackBits");
        }
        if (bps == 2) {
            for (PsdPlane& plane : out.merged) {
                for (size_t i = 0; i + 1 < plane.samples.size(); i += 2) {
                    const uint16_t v = (uint16_t)(plane.samples[i] << 8 | plane.samples[i + 1]);
                    std::memcpy(plane.samples.data() + i, &v, 2);
                }
            }
        }
        if (!r.ok()) {
            error = r.failure();
            return false;
        }
    }
    return true;
}

std::vector<PsdNode> psd::psdTree(const PsdFile& file) {
    // file order is bottom first: an end marker (3) opens a group below its children, the group
    // record (1, 2) above them closes it
    std::vector<std::vector<PsdNode>> stack(1);
    for (size_t i = 0; i < file.layers.size(); i++) {
        const PsdLayerRecord& l = file.layers[i];
        if (l.section == 3) {
            stack.emplace_back();
        } else if ((l.section == 1 || l.section == 2) && stack.size() > 1) {
            PsdNode group;
            group.layer = (int)i;
            group.children = std::move(stack.back());
            stack.pop_back();
            stack.back().push_back(std::move(group));
        } else {
            PsdNode node;
            node.layer = (int)i;
            stack.back().push_back(std::move(node));
        }
    }
    // unbalanced markers: what is left joins the top level
    while (stack.size() > 1) {
        std::vector<PsdNode> rest = std::move(stack.back());
        stack.pop_back();
        for (PsdNode& n : rest) stack.back().push_back(std::move(n));
    }
    return std::move(stack.front());
}

std::vector<uint8_t> psd::psdLayerRgba8(const PsdFile& file, const PsdLayerRecord& layer) {
    const size_t n = (size_t)layer.bounds.width() * layer.bounds.height();
    std::vector<uint8_t> out(n * 4, 0);
    const bool gray = file.mode == (uint16_t)PsdColorMode::Grayscale;
    const auto sample = [&](const PsdPlane& p, size_t i, uint8_t fallback) -> uint8_t {
        if (!p.present()) return fallback;
        if (file.depth == 16) {
            uint16_t v;
            std::memcpy(&v, p.samples.data() + i * 2, 2);
            return (uint8_t)((v * 255u + 32767u) / 65535u);
        }
        return p.samples[i];
    };
    for (size_t i = 0; i < n; i++) {
        const uint8_t r = sample(layer.planes[0], i, 0);
        out[i * 4 + 0] = r;
        out[i * 4 + 1] = gray ? r : sample(layer.planes[1], i, 0);
        out[i * 4 + 2] = gray ? r : sample(layer.planes[2], i, 0);
        out[i * 4 + 3] = sample(layer.alpha, i, 255);
    }
    return out;
}

std::vector<uint8_t> psd::psdMergedRgba(const PsdFile& file, int bitDepth) {
    const size_t n = (size_t)file.width * file.height;
    const int outBytes = bitDepth == 16 ? 2 : 1;
    std::vector<uint8_t> out(n * 4 * outBytes, 0);
    const bool gray = file.mode == (uint16_t)PsdColorMode::Grayscale;
    const PsdPlane* planes[4] = {&file.merged[0], gray ? &file.merged[0] : &file.merged[1], gray ? &file.merged[0] : &file.merged[2],
                                 gray ? &file.merged[1] : &file.merged[3]};
    for (size_t i = 0; i < n; i++) {
        for (int c = 0; c < 4; c++) {
            const PsdPlane& p = *planes[c];
            uint16_t v16 = 65535;
            if (p.present()) {
                if (file.depth == 16) std::memcpy(&v16, p.samples.data() + i * 2, 2);
                else v16 = (uint16_t)(p.samples[i] * 257);
            }
            if (outBytes == 2) {
                std::memcpy(out.data() + (i * 4 + c) * 2, &v16, 2);
            } else {
                out[i * 4 + c] = (uint8_t)((v16 * 255u + 32767u) / 65535u);
            }
        }
    }
    return out;
}

std::string psd::iccDescription(const std::vector<uint8_t>& profile) {
    ByteReader r(profile.data(), profile.size());
    if (profile.size() < 132) return "";
    r.skip(128);
    const uint32_t count = r.u32();
    for (uint32_t i = 0; i < count && r.ok() && i < 1000; i++) {
        const std::string sig = r.fourcc();
        const uint32_t offset = r.u32(), size = r.u32();
        if (sig != "desc" || (uint64_t)offset + size > profile.size() || size < 12) continue;
        ByteReader tag(profile.data() + offset, size, offset);
        const std::string type = tag.fourcc();
        tag.skip(4);
        if (type == "desc") {
            const uint32_t n = tag.u32();
            const uint8_t* p = tag.view(n);
            if (!p) return "";
            std::string s(reinterpret_cast<const char*>(p), n);
            while (!s.empty() && s.back() == 0) s.pop_back();
            return s;
        }
        if (type == "mluc") {
            const uint32_t records = tag.u32();
            tag.u32();
            if (records == 0) return "";
            tag.skip(4);
            const uint32_t length = tag.u32(), at = tag.u32();
            if ((uint64_t)at + length > size) return "";
            ByteReader text(profile.data() + offset + at, length);
            std::string s;
            for (uint32_t k = 0; k + 1 < length; k += 2) {
                const uint16_t c = text.u16();
                if (c) ByteReader::appendUtf8(s, c);
            }
            return s;
        }
    }
    return "";
}
